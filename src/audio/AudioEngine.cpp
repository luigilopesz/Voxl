#include "audio/AudioEngine.hpp"

#include "core/Log.hpp"
#include "render/Camera.hpp"

// These two must match external/miniaudio_impl.c exactly. They gate struct
// members as well as functions, so a consumer that disagrees with the
// implementation TU would compute different offsets for the same object - a
// silent ABI mismatch rather than a link error. MINIAUDIO_IMPLEMENTATION is
// deliberately NOT defined here; the implementation lives in that one C file.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>

namespace voxl::audio {
namespace {

/// Voice state words. See the handshake description in the header.
constexpr std::uint32_t kVoiceFree   = 0;
constexpr std::uint32_t kVoiceActive = 1;

/// Frames the mixer processes between parameter refreshes. Small enough that a
/// moving source does not audibly step, large enough that the per-block
/// trigonometry is amortised over hundreds of samples.
constexpr std::uint32_t kMixBlockFrames = 256;

/// Playback rate limits. A zero or negative rate would leave a voice's read
/// cursor stationary and the voice would never end.
constexpr float kMinPitch = 0.05f;
constexpr float kMaxPitch = 8.0f;

constexpr float kPi = 3.14159265358979323846f;

/// Cutoff of the distance filter at the near and far ends of a voice's range.
/// Air absorbs high frequencies with distance; without this, a block breaking
/// forty metres away is quiet but still crisp, which reads as "close but
/// turned down" rather than "far away".
constexpr float kNearCutoffHz = 18000.0f;
constexpr float kFarCutoffHz  = 900.0f;

/// The listener transform as the audio thread sees it.
struct ListenerSnapshot {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
};

[[nodiscard]] float randomFactor(SynthRng& rng, float jitter) noexcept
{
    if (jitter <= 0.0f) {
        return 1.0f;
    }
    return 1.0f + rng.range(-jitter, jitter);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Impl
// ---------------------------------------------------------------------------

struct AudioEngine::Impl {
    /// One mixer voice.
    ///
    /// Field groups are annotated with who may touch them; the grouping is the
    /// whole safety argument, so keep new members inside an existing group or
    /// add a group with the same care.
    struct Voice {
        // ---- handshake ----
        std::atomic<std::uint32_t> state{kVoiceFree};
        std::atomic<std::uint32_t> generation{0};
        /// Fade-out length in frames, or 0. Set by the game thread, cleared by
        /// whichever side gets there first; a lost update at worst delays the
        /// stop by one block.
        std::atomic<std::uint32_t> stopRequest{0};

        // ---- written by the game thread before the state release-store,
        //      read-only on the audio thread while Active ----
        const float* samples    = nullptr;
        std::size_t  frames     = 0;
        double       rateRatio  = 1.0;  ///< clip rate / device rate
        bool         looping    = false;
        std::uint8_t category   = 0;

        /// Which cue this voice is playing, and the gain the cue asked for
        /// before any per-call multiplier. Both are read back by `sustain` and
        /// `updateVoice` on the game side; atomic so that a caller on a second
        /// game-side thread is not a formal data race.
        std::atomic<CueId> cue{kInvalidCue};
        std::atomic<float> baseGain{1.0f};

        // ---- live parameters: relaxed atomics, torn reads tolerated ----
        std::atomic<float>         gain{1.0f};
        std::atomic<float>         pitch{1.0f};
        std::atomic<std::uint32_t> spatial{0};
        std::atomic<float>         posX{0.0f};
        std::atomic<float>         posY{0.0f};
        std::atomic<float>         posZ{0.0f};
        std::atomic<float>         minDistance{2.0f};
        std::atomic<float>         maxDistance{56.0f};

        // ---- audio thread only ----
        double cursor      = 0.0;
        float  smoothedL   = 0.0f;
        float  smoothedR   = 0.0f;
        float  fade        = 1.0f;
        float  fadeStep    = 0.0f;
        float  lowpass     = 0.0f;
        bool   gainsPrimed = false;
        bool   started     = false;
    };

    explicit Impl(const AudioConfig& configuration)
        : config(configuration), rng(configuration.seed)
    {
        voiceCount = std::max(1u, config.voiceCount);
        voices     = std::make_unique<Voice[]>(voiceCount);
        // The only heap allocation the mixer ever needs, made here so the audio
        // thread never allocates.
        scratch.assign(static_cast<std::size_t>(kMixBlockFrames) * 2u, 0.0f);

        masterVolume.store(std::clamp(config.masterVolume, 0.0f, 4.0f), std::memory_order_relaxed);
        categoryVolume[static_cast<std::size_t>(SoundCategory::World)].store(
            std::clamp(config.worldVolume, 0.0f, 4.0f), std::memory_order_relaxed);
        categoryVolume[static_cast<std::size_t>(SoundCategory::Ui)].store(
            std::clamp(config.uiVolume, 0.0f, 4.0f), std::memory_order_relaxed);

        // Identity listener until the first frame supplies one; an all-zero
        // basis would make the very first spatial cue divide by zero.
        listenerFwd[2].store(-1.0f, std::memory_order_relaxed);
        listenerUp[1].store(1.0f, std::memory_order_relaxed);

        if (!config.enabled) {
            reason = "audio disabled by configuration";
            VOXL_LOG_WARN("Audio: {}; every sound is a no-op", reason);
            return;
        }
        openDevice();
    }

    ~Impl()
    {
        if (deviceReady) {
            // Uninit stops the device first, so the callback is provably not
            // running once this returns and the voices' sample pointers can be
            // dropped safely.
            ma_device_uninit(&device);
            deviceReady = false;
        }
    }

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    // ------------------------------------------------------------- device --

    void openDevice()
    {
        ma_device_config deviceConfig  = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = ma_format_f32;
        deviceConfig.playback.channels = 2;
        deviceConfig.sampleRate        = config.sampleRate;
        deviceConfig.periodSizeInMilliseconds = std::max(1u, config.periodMilliseconds);
        deviceConfig.dataCallback      = &Impl::deviceCallback;
        deviceConfig.pUserData         = this;

        const ma_result initResult = ma_device_init(nullptr, &deviceConfig, &device);
        if (initResult != MA_SUCCESS) {
            // THE graceful-degradation path: one warning, then silence forever.
            // A CI agent has no playback device and must still boot the game.
            reason = "no playback device could be opened";
            VOXL_LOG_WARN("Audio: {} (miniaudio error {}); every sound is a no-op", reason,
                          static_cast<int>(initResult));
            return;
        }
        deviceReady = true;

        deviceSampleRate = device.sampleRate != 0 ? device.sampleRate : 48000u;
        deviceChannels   = std::max(1u, device.playback.channels);

        const ma_result startResult = ma_device_start(&device);
        if (startResult != MA_SUCCESS) {
            ma_device_uninit(&device);
            deviceReady = false;
            reason      = "the playback device refused to start";
            VOXL_LOG_WARN("Audio: {} (miniaudio error {}); every sound is a no-op", reason,
                          static_cast<int>(startResult));
            return;
        }

        const char* backend =
            device.pContext != nullptr ? ma_get_backend_name(device.pContext->backend) : "unknown";
        VOXL_LOG_INFO("Audio: {} backend at {} Hz, {} channel(s), {} voice(s)", backend,
                      deviceSampleRate, deviceChannels, voiceCount);
    }

    [[nodiscard]] bool available() const noexcept { return deviceReady; }

    // ------------------------------------------------------- audio thread --

    static void deviceCallback(ma_device* devicePtr, void* output, const void* input,
                               ma_uint32 frameCount) noexcept
    {
        (void)input;  // playback only
        auto* self = static_cast<Impl*>(devicePtr->pUserData);
        if (self == nullptr || output == nullptr) {
            return;
        }
        self->mix(static_cast<float*>(output), static_cast<std::uint32_t>(frameCount));
    }

    /// AUDIO THREAD. No locks, no allocation, no logging.
    void mix(float* output, std::uint32_t frameCount) noexcept
    {
        const std::size_t total = static_cast<std::size_t>(frameCount) * deviceChannels;
        std::fill_n(output, total, 0.0f);
        if (frameCount == 0) {
            return;
        }

        const float master =
            muted.load(std::memory_order_relaxed) ? 0.0f : masterVolume.load(std::memory_order_relaxed);
        std::array<float, kSoundCategoryCount> busGain{};
        for (std::size_t i = 0; i < kSoundCategoryCount; ++i) {
            busGain[i] = master * categoryVolume[i].load(std::memory_order_relaxed);
        }

        const ListenerSnapshot ears = readListener();

        for (std::uint32_t offset = 0; offset < frameCount;) {
            const std::uint32_t block = std::min(kMixBlockFrames, frameCount - offset);
            std::fill_n(scratch.data(), static_cast<std::size_t>(block) * 2u, 0.0f);

            for (std::uint32_t v = 0; v < voiceCount; ++v) {
                Voice& voice = voices[v];
                // Acquire pairs with the game thread's release store; it is what
                // makes the non-atomic fields below safe to read.
                if (voice.state.load(std::memory_order_acquire) != kVoiceActive) {
                    continue;
                }
                renderVoice(voice, scratch.data(), block, ears, busGain);
            }

            writeBlock(output + static_cast<std::size_t>(offset) * deviceChannels, scratch.data(),
                       block);
            offset += block;
        }
    }

    /// AUDIO THREAD.
    void renderVoice(Voice& voice, float* stereo, std::uint32_t frames,
                     const ListenerSnapshot&                      ears,
                     const std::array<float, kSoundCategoryCount>& busGain) noexcept
    {
        if (voice.samples == nullptr || voice.frames == 0) {
            releaseVoice(voice);
            return;
        }

        if (!voice.started) {
            voice.cursor      = 0.0;
            voice.fade        = 1.0f;
            voice.fadeStep    = 0.0f;
            voice.lowpass     = 0.0f;
            voice.gainsPrimed = false;
            voice.started     = true;
        }

        const std::uint32_t stopFrames = voice.stopRequest.load(std::memory_order_relaxed);
        if (stopFrames != 0 && voice.fadeStep == 0.0f) {
            voice.fadeStep = 1.0f / static_cast<float>(stopFrames);
        }

        const float busVolume =
            busGain[voice.category < kSoundCategoryCount ? voice.category : 0u];
        const float level = voice.gain.load(std::memory_order_relaxed) * busVolume;

        float targetL   = level;
        float targetR   = level;
        float filterCoefficient = 1.0f;  // 1 means "pass everything"

        if (voice.spatial.load(std::memory_order_relaxed) != 0u) {
            const glm::vec3 source{voice.posX.load(std::memory_order_relaxed),
                                   voice.posY.load(std::memory_order_relaxed),
                                   voice.posZ.load(std::memory_order_relaxed)};
            const glm::vec3 offset   = source - ears.position;
            const float     distance = glm::length(offset);
            const float     minimum  = std::max(voice.minDistance.load(std::memory_order_relaxed), 0.01f);
            const float     maximum =
                std::max(voice.maxDistance.load(std::memory_order_relaxed), minimum + 0.01f);

            // Inverse-distance inside the range, multiplied by a squared taper
            // so the tail reaches exact silence at maxDistance instead of being
            // audibly truncated there.
            const float clamped = std::clamp(distance, minimum, maximum);
            const float taper   = 1.0f - (clamped - minimum) / (maximum - minimum);
            float       gain    = (minimum / clamped) * taper * taper;

            float pan   = 0.0f;
            float front = 1.0f;
            if (distance > 1.0e-4f) {
                const glm::vec3 direction = offset / distance;
                // 0.85 rather than 1.0: hard-panning a sound to one ear is
                // disorienting on headphones and vanishes entirely on a mono
                // speaker.
                pan   = std::clamp(glm::dot(direction, ears.right), -1.0f, 1.0f) * 0.85f;
                front = std::clamp(glm::dot(direction, ears.forward), -1.0f, 1.0f);
            }
            // Sounds behind the listener are slightly quieter and duller. A real
            // HRTF is out of scope; this is the cheap cue that still resolves
            // front from back.
            gain *= 0.86f + 0.14f * front;

            const float angle = (pan * 0.5f + 0.5f) * (0.5f * kPi);
            targetL           = level * gain * std::cos(angle);
            targetR           = level * gain * std::sin(angle);

            const float openness = taper * (0.75f + 0.25f * std::max(front, 0.0f));
            const float cutoff   = kFarCutoffHz + (kNearCutoffHz - kFarCutoffHz) * openness;
            const float rate     = static_cast<float>(deviceSampleRate);
            filterCoefficient =
                std::clamp(1.0f - std::exp(-2.0f * kPi * cutoff / rate), 0.0f, 1.0f);
        }

        if (!voice.gainsPrimed) {
            // First block: snap. Ramping up from zero would put a fade on every
            // transient, which is exactly what a break sound must not have.
            voice.smoothedL   = targetL;
            voice.smoothedR   = targetR;
            voice.gainsPrimed = true;
        }
        const float inverseFrames = 1.0f / static_cast<float>(frames);
        const float stepL         = (targetL - voice.smoothedL) * inverseFrames;
        const float stepR         = (targetR - voice.smoothedR) * inverseFrames;

        const double length  = static_cast<double>(voice.frames);
        const double advance = voice.rateRatio *
                               static_cast<double>(std::clamp(
                                   voice.pitch.load(std::memory_order_relaxed), kMinPitch, kMaxPitch));

        bool finished = false;
        for (std::uint32_t i = 0; i < frames; ++i) {
            if (voice.cursor >= length) {
                if (!voice.looping) {
                    finished = true;
                    break;
                }
                voice.cursor = std::fmod(voice.cursor, length);
            }

            const auto        index = static_cast<std::size_t>(voice.cursor);
            const float       frac  = static_cast<float>(voice.cursor - static_cast<double>(index));
            const std::size_t next =
                index + 1 < voice.frames ? index + 1 : (voice.looping ? std::size_t{0} : index);
            float sample = voice.samples[index] +
                           (voice.samples[next] - voice.samples[index]) * frac;

            if (filterCoefficient < 0.999f) {
                voice.lowpass += filterCoefficient * (sample - voice.lowpass);
                // Flush the tail to true zero. An exponential decay spends its
                // last stretch in denormal range, and denormal arithmetic on
                // x86 costs orders of magnitude more than normal arithmetic -
                // in an audio callback that is a source of underruns, not just
                // wasted cycles.
                if (std::fabs(voice.lowpass) < 1.0e-20f) {
                    voice.lowpass = 0.0f;
                }
                sample = voice.lowpass;
            }

            voice.smoothedL += stepL;
            voice.smoothedR += stepR;

            const std::size_t slot = static_cast<std::size_t>(i) * 2u;
            stereo[slot] += sample * voice.smoothedL * voice.fade;
            stereo[slot + 1] += sample * voice.smoothedR * voice.fade;

            if (voice.fadeStep > 0.0f) {
                voice.fade -= voice.fadeStep;
                if (voice.fade <= 0.0f) {
                    finished = true;
                    break;
                }
            }
            voice.cursor += advance;
        }

        if (finished) {
            releaseVoice(voice);
        }
    }

    /// AUDIO THREAD. Hands the slot back to the game thread.
    void releaseVoice(Voice& voice) noexcept
    {
        voice.started = false;
        voice.samples = nullptr;
        voice.frames  = 0;
        voice.stopRequest.store(0, std::memory_order_relaxed);
        voice.state.store(kVoiceFree, std::memory_order_release);
    }

    /// AUDIO THREAD.
    void writeBlock(float* output, const float* stereo, std::uint32_t frames) noexcept
    {
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float left  = std::clamp(stereo[static_cast<std::size_t>(i) * 2u], -1.0f, 1.0f);
            const float right = std::clamp(stereo[static_cast<std::size_t>(i) * 2u + 1u], -1.0f, 1.0f);
            float* frame      = output + static_cast<std::size_t>(i) * deviceChannels;
            if (deviceChannels == 1) {
                frame[0] = (left + right) * 0.5f;
            } else {
                frame[0] = left;
                frame[1] = right;
                // Channels beyond stereo were zeroed by mix(); a surround device
                // gets a front-only image rather than a wrong one.
            }
        }
    }

    // ------------------------------------------------------- game thread --

    [[nodiscard]] ListenerSnapshot readListener() const noexcept
    {
        ListenerSnapshot snapshot;
        snapshot.position = glm::vec3{listenerPos[0].load(std::memory_order_relaxed),
                                      listenerPos[1].load(std::memory_order_relaxed),
                                      listenerPos[2].load(std::memory_order_relaxed)};
        snapshot.forward  = glm::vec3{listenerFwd[0].load(std::memory_order_relaxed),
                                     listenerFwd[1].load(std::memory_order_relaxed),
                                     listenerFwd[2].load(std::memory_order_relaxed)};
        const glm::vec3 up{listenerUp[0].load(std::memory_order_relaxed),
                           listenerUp[1].load(std::memory_order_relaxed),
                           listenerUp[2].load(std::memory_order_relaxed)};

        // A torn read can hand us a degenerate or non-unit basis for one block;
        // renormalising here is cheaper than making the write atomic as a whole
        // and removes the only way a NaN could enter the mixer.
        const float forwardLength = glm::length(snapshot.forward);
        snapshot.forward =
            forwardLength > 1.0e-5f ? snapshot.forward / forwardLength : glm::vec3{0.0f, 0.0f, -1.0f};
        glm::vec3   right       = glm::cross(snapshot.forward, up);
        const float rightLength = glm::length(right);
        snapshot.right = rightLength > 1.0e-5f ? right / rightLength : glm::vec3{1.0f, 0.0f, 0.0f};
        return snapshot;
    }

    [[nodiscard]] std::uint32_t nextGenerationValue() noexcept
    {
        // Never issue 0: VoiceHandle uses it as the "invalid" marker.
        ++generationCounter;
        if (generationCounter == 0) {
            generationCounter = 1;
        }
        return generationCounter;
    }

    /// Caller must hold `mutex`.
    ///
    /// `baseGain` is the cue's own level after randomisation; `gainMultiplier`
    /// is what the call site asked for. They are kept apart so `updateVoice`
    /// can re-apply a new multiplier later without having to re-derive the
    /// randomised part - re-rolling it mid-sustain would make a held mining
    /// sound jump in level every frame.
    VoiceHandle startVoice(CueId cueId, const Cue& cue, const PcmClip& clip, float baseGain,
                           float gainMultiplier, float pitch, bool spatial, bool looping,
                           const glm::vec3& position)
    {
        for (std::uint32_t attempt = 0; attempt < voiceCount; ++attempt) {
            const std::uint32_t index = (nextSlot + attempt) % voiceCount;
            Voice&              voice = voices[index];
            if (voice.state.load(std::memory_order_acquire) != kVoiceFree) {
                continue;
            }
            nextSlot = (index + 1u) % voiceCount;

            voice.samples   = clip.samples.data();
            voice.frames    = clip.frameCount();
            voice.rateRatio = static_cast<double>(clip.sampleRate == 0 ? kSynthSampleRate
                                                                       : clip.sampleRate) /
                              static_cast<double>(deviceSampleRate);
            voice.looping  = looping;
            voice.category = static_cast<std::uint8_t>(cue.style.category);
            voice.cue.store(cueId, std::memory_order_relaxed);
            voice.baseGain.store(baseGain, std::memory_order_relaxed);

            voice.gain.store(baseGain * gainMultiplier, std::memory_order_relaxed);
            voice.pitch.store(std::clamp(pitch, kMinPitch, kMaxPitch), std::memory_order_relaxed);
            voice.spatial.store(spatial ? 1u : 0u, std::memory_order_relaxed);
            voice.posX.store(position.x, std::memory_order_relaxed);
            voice.posY.store(position.y, std::memory_order_relaxed);
            voice.posZ.store(position.z, std::memory_order_relaxed);
            voice.minDistance.store(cue.style.minDistance, std::memory_order_relaxed);
            voice.maxDistance.store(cue.style.maxDistance, std::memory_order_relaxed);
            voice.stopRequest.store(0, std::memory_order_relaxed);

            const std::uint32_t generation = nextGenerationValue();
            voice.generation.store(generation, std::memory_order_relaxed);

            // Publishes every write above.
            voice.state.store(kVoiceActive, std::memory_order_release);
            started.fetch_add(1, std::memory_order_relaxed);
            return VoiceHandle{index, generation};
        }

        dropped.fetch_add(1, std::memory_order_relaxed);
        return VoiceHandle{};
    }

    [[nodiscard]] bool handleMatches(const VoiceHandle& handle) const noexcept
    {
        if (!handle.valid() || handle.slot >= voiceCount) {
            return false;
        }
        const Voice& voice = voices[handle.slot];
        return voice.generation.load(std::memory_order_relaxed) == handle.generation &&
               voice.state.load(std::memory_order_acquire) == kVoiceActive;
    }

    /// Frees every slot without waiting for a fade. Only legal when the device
    /// is stopped (or was never started), because it writes fields the audio
    /// thread would otherwise be reading.
    void resetVoicesWhileStopped() noexcept
    {
        for (std::uint32_t i = 0; i < voiceCount; ++i) {
            Voice& voice  = voices[i];
            voice.samples = nullptr;
            voice.frames  = 0;
            voice.started = false;
            voice.stopRequest.store(0, std::memory_order_relaxed);
            voice.state.store(kVoiceFree, std::memory_order_release);
        }
    }

    // ------------------------------------------------------------- state --

    AudioConfig config;

    ma_device device{};
    bool      deviceReady = false;

    std::uint32_t deviceSampleRate = 48000;
    std::uint32_t deviceChannels   = 2;
    std::string   reason;

    std::uint32_t             voiceCount = 0;
    std::unique_ptr<Voice[]>  voices;
    std::vector<float>        scratch;

    /// Guards the free-slot search, the play RNG and the bank pointer. Taken
    /// only by game-side callers; the audio thread never touches it.
    mutable std::mutex                mutex;
    std::shared_ptr<const SoundBank>  bank;
    SynthRng                          rng{0};
    std::uint32_t                     nextSlot          = 0;
    std::uint32_t                     generationCounter = 0;

    std::atomic<float> masterVolume{1.0f};
    std::array<std::atomic<float>, kSoundCategoryCount> categoryVolume{};
    std::atomic<bool>  muted{false};

    std::array<std::atomic<float>, 3> listenerPos{};
    std::array<std::atomic<float>, 3> listenerFwd{};
    std::array<std::atomic<float>, 3> listenerUp{};

    std::atomic<std::uint64_t> started{0};
    std::atomic<std::uint64_t> dropped{0};
    /// Mirrored out of the bank so `stats()` can stay lock-free and noexcept.
    std::atomic<std::size_t> bankBytes{0};
};

// ---------------------------------------------------------------------------
//  AudioEngine
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(const AudioConfig& configuration)
    : m_impl(std::make_unique<Impl>(configuration))
{
}

AudioEngine::~AudioEngine() = default;

bool AudioEngine::available() const noexcept { return m_impl->available(); }

std::string_view AudioEngine::unavailableReason() const noexcept { return m_impl->reason; }

const AudioConfig& AudioEngine::config() const noexcept { return m_impl->config; }

// ------------------------------------------------------------------ content --

void AudioEngine::setSoundBank(std::shared_ptr<const SoundBank> soundBank)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    if (!m_impl->deviceReady) {
        m_impl->bank = std::move(soundBank);
        m_impl->bankBytes.store(m_impl->bank ? m_impl->bank->sampleBytes() : 0,
                                std::memory_order_relaxed);
        return;
    }

    // Stopping the device is the synchronisation: when it returns, the callback
    // is not running, so no voice can be holding a pointer into the outgoing
    // bank while we drop the last reference to it. This is why swapping banks
    // is a load-time operation and not something to do mid-frame.
    const ma_result stopResult = ma_device_stop(&m_impl->device);
    if (stopResult != MA_SUCCESS) {
        VOXL_LOG_ERROR("Audio: could not stop the device to swap sound banks (error {}); "
                       "keeping the current bank",
                       static_cast<int>(stopResult));
        return;
    }

    m_impl->resetVoicesWhileStopped();
    m_impl->bank = std::move(soundBank);
    m_impl->bankBytes.store(m_impl->bank ? m_impl->bank->sampleBytes() : 0,
                            std::memory_order_relaxed);

    const ma_result startResult = ma_device_start(&m_impl->device);
    if (startResult != MA_SUCCESS) {
        // The bank is installed but nothing will be heard. Degrade rather than
        // fail: the game is still perfectly playable.
        VOXL_LOG_ERROR("Audio: the device did not restart after the bank swap (error {})",
                       static_cast<int>(startResult));
    }
}

std::shared_ptr<const SoundBank> AudioEngine::soundBank() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->bank;
}

CueId AudioEngine::findCue(std::string_view name) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->bank ? m_impl->bank->find(name) : kInvalidCue;
}

CueId AudioEngine::blockCue(std::string_view soundGroup, BlockSoundEvent event) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    return m_impl->bank ? m_impl->bank->blockCue(soundGroup, event) : kInvalidCue;
}

// ------------------------------------------------------------------ volumes --

void AudioEngine::setMasterVolume(float volume) noexcept
{
    m_impl->masterVolume.store(std::clamp(volume, 0.0f, 4.0f), std::memory_order_relaxed);
}

float AudioEngine::masterVolume() const noexcept
{
    return m_impl->masterVolume.load(std::memory_order_relaxed);
}

void AudioEngine::setCategoryVolume(SoundCategory category, float volume) noexcept
{
    const auto index = static_cast<std::size_t>(category);
    if (index < kSoundCategoryCount) {
        m_impl->categoryVolume[index].store(std::clamp(volume, 0.0f, 4.0f),
                                            std::memory_order_relaxed);
    }
}

float AudioEngine::categoryVolume(SoundCategory category) const noexcept
{
    const auto index = static_cast<std::size_t>(category);
    return index < kSoundCategoryCount
               ? m_impl->categoryVolume[index].load(std::memory_order_relaxed)
               : 0.0f;
}

void AudioEngine::setMuted(bool value) noexcept
{
    m_impl->muted.store(value, std::memory_order_relaxed);
}

bool AudioEngine::muted() const noexcept { return m_impl->muted.load(std::memory_order_relaxed); }

// ----------------------------------------------------------------- listener --

void AudioEngine::setListener(const ListenerState& state) noexcept
{
    m_impl->listenerPos[0].store(state.position.x, std::memory_order_relaxed);
    m_impl->listenerPos[1].store(state.position.y, std::memory_order_relaxed);
    m_impl->listenerPos[2].store(state.position.z, std::memory_order_relaxed);
    m_impl->listenerFwd[0].store(state.forward.x, std::memory_order_relaxed);
    m_impl->listenerFwd[1].store(state.forward.y, std::memory_order_relaxed);
    m_impl->listenerFwd[2].store(state.forward.z, std::memory_order_relaxed);
    m_impl->listenerUp[0].store(state.up.x, std::memory_order_relaxed);
    m_impl->listenerUp[1].store(state.up.y, std::memory_order_relaxed);
    m_impl->listenerUp[2].store(state.up.z, std::memory_order_relaxed);
}

ListenerState AudioEngine::listener() const noexcept
{
    ListenerState state;
    state.position = glm::vec3{m_impl->listenerPos[0].load(std::memory_order_relaxed),
                               m_impl->listenerPos[1].load(std::memory_order_relaxed),
                               m_impl->listenerPos[2].load(std::memory_order_relaxed)};
    state.forward  = glm::vec3{m_impl->listenerFwd[0].load(std::memory_order_relaxed),
                              m_impl->listenerFwd[1].load(std::memory_order_relaxed),
                              m_impl->listenerFwd[2].load(std::memory_order_relaxed)};
    state.up       = glm::vec3{m_impl->listenerUp[0].load(std::memory_order_relaxed),
                         m_impl->listenerUp[1].load(std::memory_order_relaxed),
                         m_impl->listenerUp[2].load(std::memory_order_relaxed)};
    return state;
}

void AudioEngine::setListenerFromCamera(const Camera& camera) noexcept
{
    ListenerState state;
    state.position = camera.position();
    state.forward  = camera.forward();
    state.up       = camera.up();
    setListener(state);
}

// ----------------------------------------------------------------- playback --

VoiceHandle AudioEngine::play(CueId cueId, const PlayParams& params)
{
    Impl& impl = *m_impl;
    if (!impl.deviceReady || cueId == kInvalidCue) {
        return VoiceHandle{};
    }

    std::lock_guard<std::mutex> lock(impl.mutex);
    if (!impl.bank) {
        return VoiceHandle{};
    }
    const Cue* cue = impl.bank->get(cueId);
    if (cue == nullptr) {
        return VoiceHandle{};
    }

    const PcmClip* clip = impl.bank->variation(cueId, impl.rng.nextUInt());
    if (clip == nullptr || clip->empty()) {
        return VoiceHandle{};
    }

    const float baseGain = cue->style.gain * randomFactor(impl.rng, cue->style.gainJitter);
    const float pitch    = randomFactor(impl.rng, cue->style.pitchJitter) * params.pitch;

    const bool spatial = params.spatialisation == Spatialisation::FromCue
                             ? cue->style.spatial
                             : params.spatialisation == Spatialisation::Positional;
    const bool looping = params.loop == LoopMode::FromCue ? cue->style.looping
                                                          : params.loop == LoopMode::Loop;

    return impl.startVoice(cueId, *cue, *clip, baseGain, std::max(params.gain, 0.0f), pitch,
                           spatial, looping, params.position);
}

VoiceHandle AudioEngine::playAt(CueId cueId, const glm::vec3& position, float gain)
{
    PlayParams params;
    params.position       = position;
    params.gain           = gain;
    params.spatialisation = Spatialisation::Positional;
    return play(cueId, params);
}

VoiceHandle AudioEngine::playUi(CueId cueId, float gain)
{
    PlayParams params;
    params.gain           = gain;
    params.spatialisation = Spatialisation::Flat;
    return play(cueId, params);
}

VoiceHandle AudioEngine::playBlockEvent(std::string_view soundGroup, BlockSoundEvent event,
                                        const glm::vec3& position, float gain)
{
    return playAt(blockCue(soundGroup, event), position, gain);
}

VoiceHandle AudioEngine::playBlockBreak(std::string_view soundGroup, const glm::vec3& position)
{
    return playBlockEvent(soundGroup, BlockSoundEvent::Break, position);
}

VoiceHandle AudioEngine::playBlockPlace(std::string_view soundGroup, const glm::vec3& position)
{
    return playBlockEvent(soundGroup, BlockSoundEvent::Place, position);
}

VoiceHandle AudioEngine::playFootstep(std::string_view soundGroup, const glm::vec3& position)
{
    return playBlockEvent(soundGroup, BlockSoundEvent::Step, position);
}

// -------------------------------------------------------- sustained voices --

VoiceHandle AudioEngine::sustain(VoiceHandle current, CueId cueId, const PlayParams& params)
{
    Impl& impl = *m_impl;
    if (!impl.deviceReady || cueId == kInvalidCue) {
        stopVoice(current);
        return VoiceHandle{};
    }

    if (impl.handleMatches(current) &&
        impl.voices[current.slot].cue.load(std::memory_order_relaxed) == cueId) {
        // Same sound, still running: refresh it in place. No allocation, no
        // restart, no click, and crucially no re-rolled randomisation.
        updateVoice(current, params);
        return current;
    }

    stopVoice(current);

    PlayParams looped = params;
    looped.loop       = LoopMode::Loop;
    return play(cueId, looped);
}

void AudioEngine::updateVoice(VoiceHandle handle, const PlayParams& params) noexcept
{
    Impl& impl = *m_impl;
    if (!impl.deviceReady || !impl.handleMatches(handle)) {
        return;
    }
    Impl::Voice& voice = impl.voices[handle.slot];
    voice.posX.store(params.position.x, std::memory_order_relaxed);
    voice.posY.store(params.position.y, std::memory_order_relaxed);
    voice.posZ.store(params.position.z, std::memory_order_relaxed);
    voice.gain.store(voice.baseGain.load(std::memory_order_relaxed) * std::max(params.gain, 0.0f),
                     std::memory_order_relaxed);
    if (params.spatialisation != Spatialisation::FromCue) {
        voice.spatial.store(params.spatialisation == Spatialisation::Positional ? 1u : 0u,
                            std::memory_order_relaxed);
    }
    voice.pitch.store(std::clamp(params.pitch, kMinPitch, kMaxPitch), std::memory_order_relaxed);
}

void AudioEngine::stopVoice(VoiceHandle handle, float fadeSeconds) noexcept
{
    Impl& impl = *m_impl;
    if (!impl.deviceReady || !impl.handleMatches(handle)) {
        return;
    }
    const float frames = std::max(fadeSeconds, 0.0f) * static_cast<float>(impl.deviceSampleRate);
    // At least one frame: a zero-length fade would leave fadeStep at 0 and the
    // voice would keep playing forever.
    impl.voices[handle.slot].stopRequest.store(
        std::max(1u, static_cast<std::uint32_t>(frames)), std::memory_order_relaxed);
}

bool AudioEngine::voicePlaying(VoiceHandle handle) const noexcept
{
    return m_impl->deviceReady && m_impl->handleMatches(handle);
}

void AudioEngine::stopAll(float fadeSeconds) noexcept
{
    Impl& impl = *m_impl;
    if (!impl.deviceReady) {
        return;
    }
    const auto frames = std::max(
        1u, static_cast<std::uint32_t>(std::max(fadeSeconds, 0.0f) *
                                       static_cast<float>(impl.deviceSampleRate)));
    for (std::uint32_t i = 0; i < impl.voiceCount; ++i) {
        Impl::Voice& voice = impl.voices[i];
        if (voice.state.load(std::memory_order_acquire) == kVoiceActive) {
            voice.stopRequest.store(frames, std::memory_order_relaxed);
        }
    }
}

AudioStats AudioEngine::stats() const noexcept
{
    const Impl& impl = *m_impl;
    AudioStats  out;
    out.available        = impl.deviceReady;
    out.deviceSampleRate = impl.deviceSampleRate;
    out.deviceChannels   = impl.deviceChannels;
    out.voiceCapacity    = impl.voiceCount;
    out.voicesStarted    = impl.started.load(std::memory_order_relaxed);
    out.voicesDropped    = impl.dropped.load(std::memory_order_relaxed);
    // Deliberately lock-free: the debug overlay samples this every frame, and
    // taking a lock inside a noexcept function would turn a contended mutex
    // into std::terminate.
    out.bankBytes = impl.bankBytes.load(std::memory_order_relaxed);

    for (std::uint32_t i = 0; i < impl.voiceCount; ++i) {
        if (impl.voices[i].state.load(std::memory_order_relaxed) == kVoiceActive) {
            ++out.activeVoices;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

void SustainedVoice::update(AudioEngine& engine, CueId cue, const PlayParams& params)
{
    if (cue == kInvalidCue) {
        stop(engine);
        return;
    }
    // `sustain` already handles the "cue changed" case by fading the old voice
    // and starting a new one, so this stays a one-liner.
    m_handle = engine.sustain(m_handle, cue, params);
    m_cue    = m_handle.valid() ? cue : kInvalidCue;
}

void SustainedVoice::stop(AudioEngine& engine, float fadeSeconds) noexcept
{
    engine.stopVoice(m_handle, fadeSeconds);
    m_handle = VoiceHandle{};
    m_cue    = kInvalidCue;
}

bool RetriggerVoice::update(AudioEngine& engine, CueId cue, const PlayParams& params,
                            float deltaSeconds, float intervalSeconds)
{
    const float interval = std::max(intervalSeconds, 1.0e-3f);
    if (m_primed) {
        m_primed      = false;
        m_accumulator = 0.0f;
        engine.play(cue, params);
        return true;
    }

    m_accumulator += std::max(deltaSeconds, 0.0f);
    if (m_accumulator < interval) {
        return false;
    }
    // Subtract rather than zero, so a frame spike does not shift the whole
    // rhythm; but never let more than one strike queue up.
    m_accumulator = std::min(m_accumulator - interval, interval);
    engine.play(cue, params);
    return true;
}

void RetriggerVoice::reset() noexcept
{
    m_accumulator = 0.0f;
    m_primed      = true;
}

}  // namespace voxl::audio
