#pragma once

// Procedural sound synthesis.
//
// WHY THIS EXISTS
// ---------------
// The project ships no audio assets and adding a dependency (or a pile of
// binary .ogg files) is out of scope, so every sound the game makes is
// generated as PCM at load time by the functions below. Nothing here is
// sampled from a recording; each cue is built from noise sources, resonant
// filters and damped partials chosen to imitate the physics of the material.
//
// REPLACING THE SYNTHESIS WITH REAL FILES
// ---------------------------------------
// Nothing downstream of `PcmClip` knows that these buffers were synthesised.
// To move to authored assets:
//
//   1. Decode the file into a `PcmClip` - mono, float32, any sample rate; the
//      mixer resamples. miniaudio's `ma_decoder` does this in a dozen lines and
//      is available (MA_NO_ENCODING removes the *encoders* only). Keep the
//      decode off the main thread; it is ordinary CPU work with no GL in it.
//   2. Run `conditionClip()` on the result. It guarantees the invariants the
//      mixer and the tests rely on: finite samples inside [-1, 1] with both
//      edges at zero, so no cue can click when a voice starts or stops.
//   3. Hand the clip to `SoundBank::add()` under the same cue name the synth
//      version used. `SoundBank::createDefault()` is the only caller of the
//      `synth*` functions, so a file-backed bank simply does not call them.
//
// DETERMINISM
// -----------
// Every generator is a pure function of (material, seed). Same seed, same
// buffer, bit for bit, for a given build - which is what the tests assert. It
// is *not* a cross-compiler guarantee: std::sin and std::exp are not specified
// to the last ulp. Audio does not need world-regeneration-grade reproducibility,
// so this is deliberate; the world generator's determinism contract is
// unaffected because no world state is derived from any of this.
//
// Threading: all free functions here are pure and re-entrant. They touch no
// global state and may be called from any thread, including a JobSystem worker.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace voxl::audio {

/// Synthesis sample rate. The mixer resamples to whatever the device asked for,
/// so this only needs to be high enough to carry the brightest cue (glass
/// shards reach ~6 kHz).
inline constexpr std::uint32_t kSynthSampleRate = 48000;

/// Mono, float32, normalised to [-1, 1].
///
/// Mono on purpose: positional cues are panned by the mixer from the listener
/// transform, and a stereo source would fight that. A genuinely stereo bed can
/// be played as two voices or added as a non-positional cue.
struct PcmClip {
    std::vector<float> samples;
    std::uint32_t      sampleRate = kSynthSampleRate;

    /// Content hint: the buffer's tail has been crossfaded onto its head, so
    /// the mixer may wrap the read cursor without an audible seam.
    bool loopable = false;

    [[nodiscard]] std::size_t frameCount() const noexcept { return samples.size(); }
    [[nodiscard]] bool        empty() const noexcept { return samples.empty(); }
    [[nodiscard]] float       durationSeconds() const noexcept
    {
        return sampleRate == 0 ? 0.0f
                               : static_cast<float>(samples.size()) / static_cast<float>(sampleRate);
    }
    [[nodiscard]] std::size_t byteSize() const noexcept { return samples.size() * sizeof(float); }
};

/// PCG-XSH-RR 32/64. Small, fast, and - unlike std::mt19937 seeded from a
/// device - completely reproducible from an integer, which is the whole point:
/// a bank built from seed N is the same bank on every machine and every run.
class SynthRng {
public:
    explicit constexpr SynthRng(std::uint64_t seed) noexcept
        : m_state(0), m_increment((seed << 1u) | 1u)
    {
        // Two rounds of the state update mix the seed properly; seeding the
        // state directly makes nearby seeds produce correlated first outputs,
        // which would make two "different" variations sound identical.
        step();
        m_state += seed;
        step();
    }

    [[nodiscard]] constexpr std::uint32_t nextUInt() noexcept
    {
        const std::uint64_t previous = m_state;
        step();
        const auto xorshifted = static_cast<std::uint32_t>(((previous >> 18u) ^ previous) >> 27u);
        const auto rotation   = static_cast<std::uint32_t>(previous >> 59u);
        return (xorshifted >> rotation) | (xorshifted << ((32u - rotation) & 31u));
    }

    /// Uniform in [0, 1). 24-bit mantissa's worth of resolution, which is more
    /// than any parameter here needs.
    [[nodiscard]] float nextFloat() noexcept
    {
        return static_cast<float>(nextUInt() >> 8u) * (1.0f / 16777216.0f);
    }

    /// Uniform in [-1, 1). The white-noise source for every generator.
    [[nodiscard]] float nextBipolar() noexcept { return nextFloat() * 2.0f - 1.0f; }

    [[nodiscard]] float range(float low, float high) noexcept
    {
        return low + (high - low) * nextFloat();
    }

private:
    constexpr void step() noexcept
    {
        m_state = m_state * 6364136223846793005ull + m_increment;
    }

    std::uint64_t m_state;
    std::uint64_t m_increment;
};

/// Acoustic material classes. These are the resolved form of
/// `BlockType::soundGroup`; the registry's string is looked up once per event.
///
/// `Gravel` and `Snow` are here because world/Block.cpp registers blocks with
/// those groups even though the milestone brief only listed seven. Anything
/// unrecognised - including air's "none" - falls back to Stone at the material
/// level, but `SoundBank` refuses to resolve a cue for "none" at all so silent
/// blocks stay silent.
enum class SoundMaterial : std::uint8_t {
    Stone = 0,
    Dirt,
    Grass,
    Sand,
    Gravel,
    Wood,
    Glass,
    Snow,
    Water,
    Count,
};

inline constexpr std::size_t kSoundMaterialCount = static_cast<std::size_t>(SoundMaterial::Count);

/// Canonical group name, matching the strings in world/Block.cpp.
[[nodiscard]] std::string_view toString(SoundMaterial material) noexcept;

/// Maps a `BlockType::soundGroup` string onto a material. Unknown groups
/// resolve to Stone rather than failing, so a block added later still makes a
/// plausible noise instead of none.
[[nodiscard]] SoundMaterial materialFromSoundGroup(std::string_view soundGroup) noexcept;

/// Per-material timings. Exposed because the clip length is a pure function of
/// the material and the event - the seed varies the *content*, never the
/// length - which is what lets a test assert an exact buffer size.
struct MaterialTimings {
    float breakSeconds    = 0.30f;
    float placeSeconds    = 0.16f;
    float footstepSeconds = 0.12f;
    float miningSeconds   = 0.75f;
};

[[nodiscard]] const MaterialTimings& materialTimings(SoundMaterial material) noexcept;

/// Frames a clip of `seconds` occupies. Truncates, so a length is never a
/// rounding coin-flip between two builds.
[[nodiscard]] std::size_t synthFrameCount(float seconds,
                                          std::uint32_t sampleRate = kSynthSampleRate) noexcept;

// ---------------------------------------------------------------- utilities --

/// Enforces the clip contract: replaces any non-finite sample with silence,
/// soft-limits into [-1, 1], and applies a raised-cosine fade at both ends so
/// the first and last samples are exactly zero.
///
/// `fadeSeconds` should stay tiny for loopable content - the fade is applied
/// across the loop seam and a long one is audible as a periodic dip.
void conditionClip(PcmClip& clip, float fadeSeconds = 0.003f);

/// Scales the clip so its largest magnitude is `peak`. A clip that is already
/// silent is left alone rather than being amplified into noise.
void normalisePeak(PcmClip& clip, float peak);

/// Crossfades the last `seconds` of `clip` onto its head and shortens the
/// buffer accordingly, producing content whose end matches its start. Sets
/// `loopable`. Used by the wind bed and the mining loops.
void makeLoopable(PcmClip& clip, float seconds);

// --------------------------------------------------------------- generators --

/// Block destruction: a bandpass-filtered noise burst whose cutoff sweeps
/// downward (the "crack" collapsing into a "thud") over a material-dependent
/// envelope, plus two or three damped resonant partials that give the material
/// its body. Glass additionally scatters delayed shards; gravel scatters
/// grains; water swaps the whole thing for a splash.
[[nodiscard]] PcmClip synthBlockBreak(SoundMaterial material, std::uint64_t seed);

/// Placement: the same machinery as `synthBlockBreak` but shorter, darker and
/// quieter - one soft thud with almost no high content, because putting a block
/// down should never be as loud as destroying one.
[[nodiscard]] PcmClip synthBlockPlace(SoundMaterial material, std::uint64_t seed);

/// Footstep: quietest of the three, pitched and filtered by material, with a
/// two-stage envelope (heel impact, then a short scuff) so a walk cycle has
/// some texture instead of being a metronome of identical taps.
[[nodiscard]] PcmClip synthFootstep(SoundMaterial material, std::uint64_t seed);

/// Seamlessly loopable "still mining" bed: a filtered noise grind under a train
/// of chips at the material's characteristic rate. Intended to be started when
/// a break timer begins and stopped when it ends.
[[nodiscard]] PcmClip synthMiningLoop(SoundMaterial material, std::uint64_t seed);

/// Loopable outdoor wind: three bands of noise with independent slow LFOs on
/// gain and cutoff, so it breathes instead of hissing. Deliberately dull - an
/// ambient bed that draws attention is a bug.
[[nodiscard]] PcmClip synthAmbientWind(std::uint64_t seed, float seconds = 6.0f);

/// Length of both UI blips. Fixed rather than per-variant so a menu's timing
/// can be written against a constant.
inline constexpr float kUiClickSeconds = 0.10f;

/// UI blip. `back` gives the falling-pitch variant used for cancel/close.
[[nodiscard]] PcmClip synthUiClick(std::uint64_t seed, bool back = false);

}  // namespace voxl::audio
