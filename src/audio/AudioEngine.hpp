#pragma once

// Playback device, voice pool and 3D mixer.
//
// ============================================================================
//  THREADING MODEL - read this before touching anything in audio/
// ============================================================================
//  There are exactly two threads in this subsystem.
//
//  GAME THREAD (in practice the main thread, but any single thread will do):
//    owns every public method below. Claims voices, writes their parameters,
//    swaps the sound bank, moves the listener. Never blocks on the audio
//    thread; the only lock it takes is a mutex the audio thread has never
//    heard of, held for the handful of instructions it takes to find a free
//    voice slot. A play call is therefore bounded work with no allocation and
//    no syscall, and CANNOT stall a frame.
//
//  AUDIO THREAD (created and owned by miniaudio):
//    calls back every few milliseconds asking for N frames. It reads voice
//    parameters, mixes, and writes to the device buffer. It NEVER locks, never
//    allocates, never frees, never logs and never touches std::shared_ptr.
//
//  The handshake between them is a per-voice state word:
//
//      Free --(game thread, release store)--> Active --(audio thread, release
//      store)--> Free
//
//    Only the game thread performs Free->Active, and only after it has written
//    the voice's immutable fields (sample pointer, length, loop flag). Only the
//    audio thread performs Active->Free, and only after it has stopped reading
//    them. The release/acquire pair on that one word is what publishes
//    everything else, so no other field needs to be atomic for correctness.
//
//    Parameters that change while a voice plays - gain, pitch, world position -
//    are individually relaxed atomics. They can be read torn across components
//    (a position from halfway through a write); at 5 ms of mixing per block
//    that is inaudible, and the alternative (a lock, or a seqlock retry loop)
//    would put the audio thread at the mercy of the game thread's scheduler.
//    The same reasoning applies to the listener transform.
//
//  SAMPLE LIFETIME: a playing voice holds a raw pointer into a SoundBank clip.
//    The engine keeps the bank alive with a shared_ptr<const SoundBank>, and
//    `setSoundBank` stops the device before swapping - i.e. the audio thread is
//    provably not inside the callback - so a voice can never read a freed
//    buffer. This is the audio-side analogue of the "no writing to a chunk a
//    worker may be reading" rule.
//
// ============================================================================
//  GRACEFUL DEGRADATION
// ============================================================================
//  A build agent, a headless capture run and a machine with no sound card must
//  all start the game. If the device cannot be initialised - or if
//  `AudioConfig::enabled` is false - the engine logs ONE warning, reports
//  `available() == false`, and turns every play call into a no-op that returns
//  an invalid handle. No later call logs again, nothing throws, and no caller
//  needs to check anything.

#include "audio/SoundBank.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

#include <glm/vec3.hpp>

namespace voxl {

class Camera;

namespace audio {

/// Device and mixer configuration. Everything here is fixed at construction.
struct AudioConfig {
    /// False starts the engine in the same no-op state a missing device
    /// produces. Used by tests and by a `--no-audio` style switch.
    bool enabled = true;

    /// 0 asks the device for its own preferred rate, which avoids a resampler
    /// in the driver. Clips are resampled by the mixer either way.
    std::uint32_t sampleRate = 48000;

    /// Simultaneous voices. Past this, new play calls are dropped and counted
    /// rather than stealing an audible voice; 48 is far more than a voxel
    /// sandbox generates outside a chain-reaction bug.
    std::uint32_t voiceCount = 48;

    /// Requested device period. Smaller is tighter but risks underruns on a
    /// loaded machine; 10 ms is a comfortable compromise for a game.
    std::uint32_t periodMilliseconds = 10;

    float masterVolume = 1.0f;
    float worldVolume  = 1.0f;
    float uiVolume     = 1.0f;

    /// Seeds the per-play pitch/gain randomisation. Fixed by default so a
    /// recorded session is reproducible.
    std::uint64_t seed = 0x2545F4914F6CDD1Dull;
};

/// Where the ears are. Right-handed, matching Camera's conventions.
struct ListenerState {
    glm::vec3 position{0.0f};
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

/// Reference to a playing voice.
///
/// Handles are safe to hold indefinitely. Once the voice ends, its slot is
/// reused with a new generation and the stale handle silently stops matching,
/// so `stopVoice` on a finished sound can never cut off an unrelated one.
struct VoiceHandle {
    std::uint32_t slot       = 0;
    std::uint32_t generation = 0;  ///< 0 is never issued

    [[nodiscard]] bool valid() const noexcept { return generation != 0; }
    [[nodiscard]] friend bool operator==(const VoiceHandle& a, const VoiceHandle& b) noexcept
    {
        return a.slot == b.slot && a.generation == b.generation;
    }
};

/// Whether a play call positions the sound in the world.
enum class Spatialisation : std::uint8_t {
    /// Use the cue's own default. Almost always what you want.
    FromCue = 0,
    Positional,
    /// Straight to both ears at full level, ignoring the listener.
    Flat,
};

enum class LoopMode : std::uint8_t {
    FromCue = 0,
    Loop,
    OneShot,
};

struct PlayParams {
    glm::vec3 position{0.0f};

    /// Multiplies the cue's own gain, after randomisation.
    float gain = 1.0f;
    /// Multiplies the randomised pitch. 2.0 is an octave up.
    float pitch = 1.0f;

    Spatialisation spatialisation = Spatialisation::FromCue;
    LoopMode       loop           = LoopMode::FromCue;
};

struct AudioStats {
    bool          available        = false;
    std::uint32_t deviceSampleRate = 0;
    std::uint32_t deviceChannels   = 0;
    std::uint32_t voiceCapacity    = 0;
    std::uint32_t activeVoices     = 0;
    std::uint64_t voicesStarted    = 0;
    /// Play calls that found no free slot. A steadily climbing number means
    /// `AudioConfig::voiceCount` is too small or something is leaking loops.
    std::uint64_t voicesDropped = 0;
    std::size_t   bankBytes     = 0;
};

class AudioEngine {
public:
    /// Default fade applied by `stopVoice`. Long enough that cutting a loop
    /// never clicks, short enough to feel instant.
    static constexpr float kDefaultStopFadeSeconds = 0.03f;

    explicit AudioEngine(const AudioConfig& config = AudioConfig{});
    ~AudioEngine();

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&)                 = delete;
    AudioEngine& operator=(AudioEngine&&)      = delete;

    /// False when there is no device. Every other method stays callable and
    /// does nothing; checking this is optional, not required.
    [[nodiscard]] bool available() const noexcept;
    /// Human-readable explanation, empty when the engine is available.
    [[nodiscard]] std::string_view unavailableReason() const noexcept;
    [[nodiscard]] const AudioConfig& config() const noexcept;

    // ------------------------------------------------------------- content --

    /// Attaches a bank. Stops the device around the swap, so it is a load-time
    /// operation (a few milliseconds), not something to do per frame. Any
    /// playing voice is stopped, because its samples belong to the old bank.
    void setSoundBank(std::shared_ptr<const SoundBank> bank);
    [[nodiscard]] std::shared_ptr<const SoundBank> soundBank() const;

    /// Convenience lookups that tolerate a missing bank.
    [[nodiscard]] CueId findCue(std::string_view name) const;
    [[nodiscard]] CueId blockCue(std::string_view soundGroup, BlockSoundEvent event) const;

    // ------------------------------------------------------------- volumes --

    void setMasterVolume(float volume) noexcept;
    [[nodiscard]] float masterVolume() const noexcept;

    void setCategoryVolume(SoundCategory category, float volume) noexcept;
    [[nodiscard]] float categoryVolume(SoundCategory category) const noexcept;

    /// Instant, and independent of the volume sliders, so the menu can restore
    /// exactly what the player had set.
    void setMuted(bool muted) noexcept;
    [[nodiscard]] bool muted() const noexcept;

    // ------------------------------------------------------------ listener --

    /// Call once per frame. Cheap: nine relaxed atomic stores.
    void setListener(const ListenerState& listener) noexcept;
    [[nodiscard]] ListenerState listener() const noexcept;
    /// Same, taken straight from the render camera's eye transform.
    void setListenerFromCamera(const Camera& camera) noexcept;

    // ------------------------------------------------------------ playback --

    /// Starts `cue`. Returns an invalid handle when audio is unavailable, the
    /// cue does not exist, or no voice slot is free - all of which are normal
    /// and none of which need handling at the call site.
    VoiceHandle play(CueId cue, const PlayParams& params = PlayParams{});

    /// Positional one-shot.
    VoiceHandle playAt(CueId cue, const glm::vec3& position, float gain = 1.0f);

    /// Flat, UI-bus one-shot.
    VoiceHandle playUi(CueId cue, float gain = 1.0f);

    /// Block events by `BlockType::soundGroup`. A "none" group (air) is silent.
    /// `position` should be the centre of the block, not its corner.
    VoiceHandle playBlockEvent(std::string_view soundGroup, BlockSoundEvent event,
                               const glm::vec3& position, float gain = 1.0f);
    VoiceHandle playBlockBreak(std::string_view soundGroup, const glm::vec3& position);
    VoiceHandle playBlockPlace(std::string_view soundGroup, const glm::vec3& position);
    VoiceHandle playFootstep(std::string_view soundGroup, const glm::vec3& position);

    // ---------------------------------------------------- sustained voices --

    /// The hook for a continuous sound whose lifetime tracks a gameplay state -
    /// the mining bed being the case this was written for.
    ///
    /// Pass the handle from last frame. If it is still playing `cue`, the
    /// voice's position/gain/pitch are updated in place and the same handle
    /// comes back. Otherwise the old voice is faded out and a new looping one
    /// is started. Calling it every frame while the break timer runs, and
    /// `stopVoice` when it stops, is the entire integration.
    VoiceHandle sustain(VoiceHandle current, CueId cue, const PlayParams& params);

    /// Moves or re-levels a playing voice. Safe on a stale handle.
    void updateVoice(VoiceHandle handle, const PlayParams& params) noexcept;

    /// Fades the voice out and releases its slot. Safe on a stale handle.
    void stopVoice(VoiceHandle handle,
                   float       fadeSeconds = kDefaultStopFadeSeconds) noexcept;

    [[nodiscard]] bool voicePlaying(VoiceHandle handle) const noexcept;

    void stopAll(float fadeSeconds = kDefaultStopFadeSeconds) noexcept;

    [[nodiscard]] AudioStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
//  Gameplay-side helpers
// ---------------------------------------------------------------------------

/// Remembers one sustained voice so a gameplay system does not have to.
///
/// Non-owning and trivially cheap; keep one as a member of whatever drives the
/// sound (the mining verb, a "swimming" state, a machine block).
class SustainedVoice {
public:
    /// Starts or refreshes the sound. Switching `cue` mid-sustain - the player
    /// dragging the cursor from stone onto wood - crossfades by stopping the
    /// old voice and starting the new one.
    void update(AudioEngine& engine, CueId cue, const PlayParams& params);

    void stop(AudioEngine& engine, float fadeSeconds = 0.06f) noexcept;

    [[nodiscard]] bool  active() const noexcept { return m_cue != kInvalidCue; }
    [[nodiscard]] CueId cue() const noexcept { return m_cue; }
    [[nodiscard]] VoiceHandle handle() const noexcept { return m_handle; }

private:
    VoiceHandle m_handle{};
    CueId       m_cue = kInvalidCue;
};

/// Fires a one-shot on a fixed interval for as long as it keeps being updated.
///
/// The alternative to `SustainedVoice` for mining: a loop is right for a drill,
/// a retrigger is right for a pickaxe. Both are offered because which one suits
/// the break timer is a gameplay decision this module does not get to make.
class RetriggerVoice {
public:
    /// Fires immediately on the first call after construction or `reset()`,
    /// then every `intervalSeconds`. Returns true on the frames it fired.
    bool update(AudioEngine& engine, CueId cue, const PlayParams& params, float deltaSeconds,
                float intervalSeconds);

    /// Call when the gameplay state ends, so the next start is immediate rather
    /// than waiting out a leftover interval.
    void reset() noexcept;

private:
    float m_accumulator = 0.0f;
    bool  m_primed      = true;
};

}  // namespace audio
}  // namespace voxl
