#include "audio/SynthSounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace voxl::audio {
namespace {

constexpr float kPi     = 3.14159265358979323846f;
constexpr float kTwoPi  = 2.0f * kPi;

// ---------------------------------------------------------------------------
//  Small DSP building blocks
// ---------------------------------------------------------------------------

/// Chamberlin state-variable filter.
///
/// Chosen over a biquad because all three outputs (low/band/high) fall out of
/// one update and because the coefficient is a plain function of the cutoff,
/// which makes a per-sample cutoff sweep - the thing that turns a flat noise
/// burst into a "crack" - a one-line change instead of a coefficient rebuild.
///
/// Its stability limit is the reason `configure` clamps to fs/4.2: above that
/// the recursion blows up into NaN rather than merely sounding wrong.
class Svf {
public:
    void configure(float cutoffHz, float q, float sampleRate) noexcept
    {
        const float limit = sampleRate * 0.238f;
        const float fc    = std::clamp(cutoffHz, 15.0f, limit);
        m_f               = 2.0f * std::sin(kPi * fc / sampleRate);
        m_damping         = std::clamp(1.0f / std::clamp(q, 0.35f, 16.0f), 0.02f, 1.8f);
    }

    void process(float input) noexcept
    {
        m_low += m_f * m_band;
        m_high = input - m_low - m_damping * m_band;
        m_band += m_f * m_high;
    }

    [[nodiscard]] float low() const noexcept { return m_low; }
    [[nodiscard]] float band() const noexcept { return m_band; }
    [[nodiscard]] float high() const noexcept { return m_high; }

private:
    float m_f       = 0.1f;
    float m_damping = 1.0f;
    float m_low     = 0.0f;
    float m_band    = 0.0f;
    float m_high    = 0.0f;
};

/// One-pole lowpass, used for gentle tone shaping and for the slow LFOs that
/// drive the wind bed (a lowpassed noise source is a far more natural control
/// signal than a sine).
class OnePole {
public:
    void configure(float cutoffHz, float sampleRate) noexcept
    {
        const float fc = std::clamp(cutoffHz, 0.01f, sampleRate * 0.45f);
        m_a            = 1.0f - std::exp(-kTwoPi * fc / sampleRate);
    }
    float process(float input) noexcept
    {
        m_y += m_a * (input - m_y);
        return m_y;
    }
    [[nodiscard]] float value() const noexcept { return m_y; }

private:
    float m_a = 0.1f;
    float m_y = 0.0f;
};

/// Percussive envelope: an exponential attack so nothing starts on a
/// discontinuity, times an exponential decay.
[[nodiscard]] float percussiveEnvelope(float t, float attackSeconds, float decaySeconds) noexcept
{
    const float attack = attackSeconds <= 0.0f ? 1.0f : 1.0f - std::exp(-t / attackSeconds);
    return attack * std::exp(-t / std::max(decaySeconds, 1.0e-4f));
}

/// Adds an exponentially damped sinusoid - one resonant mode of the material.
/// Several of these layered under a noise burst are what make stone read as
/// stone and wood as hollow.
void addPartial(std::vector<float>& out, std::size_t offset, float sampleRate, float frequencyHz,
                float amplitude, float decaySeconds, float phase) noexcept
{
    if (amplitude == 0.0f || offset >= out.size()) {
        return;
    }
    const float step = kTwoPi * frequencyHz / sampleRate;
    // Stop once the mode is 60 dB down; running it to the end of a long buffer
    // is wasted work that also risks denormal stalls.
    const auto  span =
        static_cast<std::size_t>(std::max(decaySeconds, 1.0e-4f) * 7.0f * sampleRate);
    const std::size_t last = std::min(out.size(), offset + span);

    for (std::size_t i = offset; i < last; ++i) {
        const float t = static_cast<float>(i - offset) / sampleRate;
        out[i] += amplitude * std::exp(-t / std::max(decaySeconds, 1.0e-4f)) *
                  std::sin(step * static_cast<float>(i - offset) + phase);
    }
}

/// Adds a short band-limited noise transient. The workhorse for chips, grains,
/// shards and footstep scuffs.
void addNoiseBurst(std::vector<float>& out, std::size_t offset, float sampleRate, SynthRng& rng,
                   float centreHz, float q, float decaySeconds, float amplitude,
                   float durationSeconds) noexcept
{
    if (offset >= out.size()) {
        return;
    }
    Svf filter;
    filter.configure(centreHz, q, sampleRate);

    const auto        span = static_cast<std::size_t>(durationSeconds * sampleRate);
    const std::size_t last = std::min(out.size(), offset + span);
    for (std::size_t i = offset; i < last; ++i) {
        const float t = static_cast<float>(i - offset) / sampleRate;
        filter.process(rng.nextBipolar());
        out[i] += filter.band() * amplitude * percussiveEnvelope(t, 0.0008f, decaySeconds);
    }
}

/// A droplet: a sine whose pitch sweeps upward as it decays. Two or three of
/// these scattered through a splash is what stops water sounding like static.
void addDroplet(std::vector<float>& out, std::size_t offset, float sampleRate, float startHz,
                float endHz, float decaySeconds, float amplitude) noexcept
{
    if (offset >= out.size()) {
        return;
    }
    const auto        span = static_cast<std::size_t>(decaySeconds * 6.0f * sampleRate);
    const std::size_t last = std::min(out.size(), offset + span);
    const float       total = std::max(decaySeconds * 6.0f, 1.0e-4f);

    float phase = 0.0f;
    for (std::size_t i = offset; i < last; ++i) {
        const float t     = static_cast<float>(i - offset) / sampleRate;
        const float sweep = std::clamp(t / total, 0.0f, 1.0f);
        // Exponential interpolation: a linear sweep in Hz sounds like it slows
        // down, because pitch perception is logarithmic.
        const float hz = startHz * std::pow(endHz / startHz, sweep);
        phase += kTwoPi * hz / sampleRate;
        out[i] += amplitude * std::exp(-t / std::max(decaySeconds, 1.0e-4f)) * std::sin(phase);
    }
}

// ---------------------------------------------------------------------------
//  Material profiles
// ---------------------------------------------------------------------------

/// Everything that distinguishes one material from another, in one table so the
/// generators stay generic and the tuning is reviewable at a glance.
///
/// Frequencies are Hz, decays are seconds (exponential time constants), gains
/// are pre-normalisation and only matter relative to each other within a cue.
struct Profile {
    MaterialTimings timings{};

    // Break: a noise burst swept from `breakCentreHz` down to a fraction of it.
    float breakCentreHz = 1400.0f;
    float breakQ        = 1.2f;
    float breakDecay    = 0.075f;
    float breakSweep    = 0.35f;  ///< cutoff floor as a fraction of the centre
    float breakNoise    = 1.0f;   ///< noise-burst gain

    std::array<float, 3> partialHz{180.0f, 520.0f, 1150.0f};
    std::array<float, 3> partialGain{0.45f, 0.26f, 0.12f};
    float                partialDecay = 0.085f;

    // Place: shorter, darker, softer.
    float placeCentreHz = 520.0f;
    float placeDecay    = 0.050f;
    float placeGain     = 0.55f;

    // Footstep.
    float stepCentreHz = 900.0f;
    float stepQ        = 0.9f;
    float stepDecay    = 0.032f;
    float stepGain     = 0.30f;

    // Mining loop.
    float mineChipsPerSecond = 11.0f;
    float mineGrindGain      = 0.22f;

    /// Peak the finished break cue is normalised to. Everything else is scaled
    /// relative to this, which is how the mix stays balanced without a
    /// per-material volume knob at the call site.
    float breakPeak = 0.80f;
};

// Tuned by ear against the physical intuition for each material: hard and
// bright materials get a high, high-Q burst and long partials; loose granular
// ones get a low-Q hiss with no partials at all.
constexpr std::array<Profile, kSoundMaterialCount> kProfiles = {
    // ---- Stone: sharp crack over a dull thud, three clear modes.
    Profile{MaterialTimings{0.32f, 0.17f, 0.12f, 0.75f},
            1500.0f, 1.5f, 0.070f, 0.30f, 1.00f,
            {175.0f, 540.0f, 1180.0f}, {0.48f, 0.27f, 0.13f}, 0.085f,
            520.0f, 0.050f, 0.55f,
            950.0f, 1.0f, 0.030f, 0.30f,
            11.0f, 0.24f, 0.80f},

    // ---- Dirt: almost all low-frequency, no ring at all.
    Profile{MaterialTimings{0.24f, 0.15f, 0.11f, 0.70f},
            460.0f, 0.75f, 0.060f, 0.45f, 1.00f,
            {95.0f, 215.0f, 0.0f}, {0.34f, 0.14f, 0.0f}, 0.045f,
            270.0f, 0.045f, 0.50f,
            420.0f, 0.7f, 0.028f, 0.26f,
            8.0f, 0.30f, 0.68f},

    // ---- Grass: a soft high swish, no body whatsoever.
    Profile{MaterialTimings{0.26f, 0.15f, 0.11f, 0.70f},
            2700.0f, 0.60f, 0.085f, 0.55f, 1.00f,
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.03f,
            1500.0f, 0.055f, 0.42f,
            2100.0f, 0.55f, 0.036f, 0.20f,
            7.0f, 0.26f, 0.55f},

    // ---- Sand: pure granular hiss, slightly brighter than grass.
    Profile{MaterialTimings{0.25f, 0.15f, 0.11f, 0.70f},
            3300.0f, 0.50f, 0.080f, 0.40f, 1.00f,
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.03f,
            1700.0f, 0.050f, 0.42f,
            2500.0f, 0.50f, 0.034f, 0.22f,
            9.0f, 0.34f, 0.60f},

    // ---- Gravel: sand plus discrete stone grains (added in the generator).
    Profile{MaterialTimings{0.28f, 0.16f, 0.12f, 0.72f},
            1900.0f, 0.85f, 0.070f, 0.35f, 0.85f,
            {150.0f, 430.0f, 0.0f}, {0.22f, 0.14f, 0.0f}, 0.040f,
            900.0f, 0.048f, 0.48f,
            1500.0f, 0.70f, 0.032f, 0.26f,
            13.0f, 0.28f, 0.72f},

    // ---- Wood: mid burst over a long hollow ring.
    Profile{MaterialTimings{0.30f, 0.17f, 0.12f, 0.75f},
            950.0f, 2.2f, 0.065f, 0.32f, 0.85f,
            {155.0f, 305.0f, 640.0f}, {0.42f, 0.30f, 0.17f}, 0.130f,
            390.0f, 0.055f, 0.55f,
            740.0f, 1.3f, 0.030f, 0.28f,
            10.0f, 0.20f, 0.78f},

    // ---- Glass: brief high crack, long high partials, scattered shards.
    Profile{MaterialTimings{0.55f, 0.16f, 0.11f, 0.70f},
            4300.0f, 3.0f, 0.045f, 0.55f, 0.75f,
            {2650.0f, 3950.0f, 5350.0f}, {0.30f, 0.22f, 0.14f}, 0.240f,
            2400.0f, 0.035f, 0.45f,
            2900.0f, 2.0f, 0.022f, 0.18f,
            14.0f, 0.16f, 0.75f},

    // ---- Snow: quiet, dry, mid-high crunch.
    Profile{MaterialTimings{0.24f, 0.14f, 0.11f, 0.70f},
            2300.0f, 0.65f, 0.062f, 0.45f, 1.00f,
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.03f,
            1300.0f, 0.045f, 0.40f,
            1900.0f, 0.60f, 0.030f, 0.18f,
            9.0f, 0.22f, 0.50f},

    // ---- Water: replaced wholesale by a splash in the generator; the fields
    //      here only seed the shared envelope maths.
    Profile{MaterialTimings{0.42f, 0.26f, 0.16f, 0.70f},
            1200.0f, 0.7f, 0.110f, 1.60f, 1.00f,
            {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 0.03f,
            800.0f, 0.090f, 0.60f,
            1100.0f, 0.6f, 0.055f, 0.24f,
            6.0f, 0.30f, 0.62f},
};

static_assert(kProfiles.size() == kSoundMaterialCount, "one profile per material");

[[nodiscard]] const Profile& profileOf(SoundMaterial material) noexcept
{
    const auto index = static_cast<std::size_t>(material);
    return kProfiles[index < kProfiles.size() ? index : 0];
}

[[nodiscard]] std::vector<float> silentBuffer(float seconds, float sampleRate)
{
    return std::vector<float>(synthFrameCount(seconds, static_cast<std::uint32_t>(sampleRate)),
                              0.0f);
}

// ---------------------------------------------------------------------------
//  Shared layers
// ---------------------------------------------------------------------------

/// The swept, band-limited noise transient common to break/place/step. Written
/// once because the difference between "smashing" and "setting down" is the
/// parameters, not the algorithm.
void addSweptBurst(std::vector<float>& out, float sampleRate, SynthRng& rng, float centreHz,
                   float q, float decaySeconds, float sweepFloor, float amplitude) noexcept
{
    Svf         filter;
    const float total = static_cast<float>(out.size()) / sampleRate;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / sampleRate;
        // Cutoff falls towards `sweepFloor * centre` on roughly the envelope's
        // own time constant. This is the single most important gesture in the
        // whole file: a fixed-cutoff burst sounds synthetic, a falling one
        // sounds like something breaking.
        const float fall = std::exp(-t / std::max(decaySeconds * 0.8f, 1.0e-4f));
        filter.configure(centreHz * (sweepFloor + (1.0f - sweepFloor) * fall), q, sampleRate);
        filter.process(rng.nextBipolar());

        // A trace of the lowpass output puts weight under the band, so the
        // burst has a floor rather than sounding like a telephone.
        const float voice = filter.band() + 0.35f * filter.low();
        out[i] += voice * amplitude * percussiveEnvelope(t, 0.0012f, decaySeconds) *
                  // Very slight overall taper so nothing is still ringing at the
                  // buffer's end, whatever the decay constant says.
                  (1.0f - std::clamp(t / std::max(total, 1.0e-4f), 0.0f, 1.0f) *
                              std::clamp(t / std::max(total, 1.0e-4f), 0.0f, 1.0f));
    }
}

/// Water is different enough from every solid that it gets its own routine:
/// the bandpass sweeps *upward* (the sound of a cavity opening and closing),
/// and the character comes from droplets rather than from resonant modes.
void addSplash(std::vector<float>& out, float sampleRate, SynthRng& rng, float amplitude,
               float decaySeconds, int droplets)
{
    Svf         filter;
    const float total = static_cast<float>(out.size()) / sampleRate;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t    = static_cast<float>(i) / sampleRate;
        const float rise = 1.0f - std::exp(-t / std::max(decaySeconds * 0.35f, 1.0e-4f));
        filter.configure(320.0f + 2600.0f * rise, 0.8f, sampleRate);
        filter.process(rng.nextBipolar());
        out[i] += filter.band() * amplitude * percussiveEnvelope(t, 0.004f, decaySeconds);
    }

    for (int d = 0; d < droplets; ++d) {
        const float onset = rng.range(0.02f, total * 0.55f);
        addDroplet(out, static_cast<std::size_t>(onset * sampleRate), sampleRate,
                   rng.range(380.0f, 900.0f), rng.range(1400.0f, 2600.0f), rng.range(0.012f, 0.03f),
                   amplitude * rng.range(0.18f, 0.38f));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Material naming
// ---------------------------------------------------------------------------

std::string_view toString(SoundMaterial material) noexcept
{
    switch (material) {
        case SoundMaterial::Stone:  return "stone";
        case SoundMaterial::Dirt:   return "dirt";
        case SoundMaterial::Grass:  return "grass";
        case SoundMaterial::Sand:   return "sand";
        case SoundMaterial::Gravel: return "gravel";
        case SoundMaterial::Wood:   return "wood";
        case SoundMaterial::Glass:  return "glass";
        case SoundMaterial::Snow:   return "snow";
        case SoundMaterial::Water:  return "water";
        case SoundMaterial::Count:  break;
    }
    return "stone";
}

SoundMaterial materialFromSoundGroup(std::string_view soundGroup) noexcept
{
    for (std::size_t i = 0; i < kSoundMaterialCount; ++i) {
        const auto material = static_cast<SoundMaterial>(i);
        if (toString(material) == soundGroup) {
            return material;
        }
    }
    return SoundMaterial::Stone;
}

const MaterialTimings& materialTimings(SoundMaterial material) noexcept
{
    return profileOf(material).timings;
}

std::size_t synthFrameCount(float seconds, std::uint32_t sampleRate) noexcept
{
    if (!(seconds > 0.0f)) {
        return 0;
    }
    return static_cast<std::size_t>(seconds * static_cast<float>(sampleRate));
}

// ---------------------------------------------------------------------------
//  Clip conditioning
// ---------------------------------------------------------------------------

void conditionClip(PcmClip& clip, float fadeSeconds)
{
    std::vector<float>& samples = clip.samples;
    if (samples.empty()) {
        return;
    }

    for (float& sample : samples) {
        // A NaN anywhere in a buffer poisons the mixer's accumulator for the
        // whole frame, so it is scrubbed here rather than defended against in
        // the audio callback, which must stay branch-light.
        if (!std::isfinite(sample)) {
            sample = 0.0f;
            continue;
        }
        // Soft knee above 0.9 so a loud transient rounds off instead of
        // clipping into buzz, then a hard clamp for the contract.
        const float magnitude = std::fabs(sample);
        if (magnitude > 0.9f) {
            const float excess = magnitude - 0.9f;
            const float shaped = 0.9f + 0.1f * std::tanh(excess * 10.0f);
            sample             = std::copysign(shaped, sample);
        }
        sample = std::clamp(sample, -1.0f, 1.0f);
    }

    const auto rate = static_cast<float>(clip.sampleRate == 0 ? kSynthSampleRate : clip.sampleRate);
    // At most a quarter of the clip per edge, so a very short cue is faded
    // rather than erased.
    const std::size_t fade =
        std::min(static_cast<std::size_t>(std::max(fadeSeconds, 0.0f) * rate), samples.size() / 4);
    if (fade == 0) {
        // Even with no room for a ramp the contract still holds: the first and
        // last samples must be zero.
        samples.front() = 0.0f;
        samples.back()  = 0.0f;
        return;
    }

    for (std::size_t i = 0; i < fade; ++i) {
        // Raised cosine rather than linear: a linear ramp has a slope
        // discontinuity at both ends, which is itself faintly audible.
        const float phase = static_cast<float>(i) / static_cast<float>(fade);
        const float gain  = 0.5f - 0.5f * std::cos(kPi * phase);
        samples[i] *= gain;
        samples[samples.size() - 1 - i] *= gain;
    }
    samples.front() = 0.0f;
    samples.back()  = 0.0f;
}

void normalisePeak(PcmClip& clip, float peak)
{
    float loudest = 0.0f;
    for (const float sample : clip.samples) {
        if (std::isfinite(sample)) {
            loudest = std::max(loudest, std::fabs(sample));
        }
    }
    if (loudest <= 1.0e-6f) {
        return;  // silence stays silence; scaling it just amplifies rounding dust
    }
    const float scale = std::clamp(peak, 0.0f, 1.0f) / loudest;
    for (float& sample : clip.samples) {
        sample *= scale;
    }
}

void makeLoopable(PcmClip& clip, float seconds)
{
    const auto rate = static_cast<float>(clip.sampleRate == 0 ? kSynthSampleRate : clip.sampleRate);
    const std::size_t fade = static_cast<std::size_t>(std::max(seconds, 0.0f) * rate);
    if (fade == 0 || clip.samples.size() <= fade + 1) {
        clip.loopable = true;
        return;
    }

    const std::size_t kept = clip.samples.size() - fade;
    for (std::size_t i = 0; i < fade; ++i) {
        // Equal-power crossfade. A linear one dips by 3 dB in the middle, which
        // on a continuous bed like wind is heard as a periodic lull exactly at
        // the loop point - the artefact this is meant to remove.
        const float phase = static_cast<float>(i) / static_cast<float>(fade);
        const float head  = std::sin(0.5f * kPi * phase);
        const float tail  = std::cos(0.5f * kPi * phase);
        clip.samples[i]   = clip.samples[i] * head + clip.samples[kept + i] * tail;
    }
    clip.samples.resize(kept);
    clip.samples.shrink_to_fit();
    clip.loopable = true;
}

// ---------------------------------------------------------------------------
//  Generators
// ---------------------------------------------------------------------------

PcmClip synthBlockBreak(SoundMaterial material, std::uint64_t seed)
{
    const Profile& profile = profileOf(material);
    const auto     rate    = static_cast<float>(kSynthSampleRate);

    PcmClip clip;
    clip.samples = silentBuffer(profile.timings.breakSeconds, rate);
    if (clip.samples.empty()) {
        return clip;
    }

    SynthRng rng{seed};
    // Per-variation deviation. Small enough that every variation is recognisably
    // the same material, large enough that four of them in a row do not sound
    // like one sample retriggered.
    const float detune = rng.range(0.86f, 1.16f);
    const float damp   = rng.range(0.82f, 1.24f);

    if (material == SoundMaterial::Water) {
        addSplash(clip.samples, rate, rng, 1.0f, profile.breakDecay * damp, 4);
    } else {
        addSweptBurst(clip.samples, rate, rng, profile.breakCentreHz * detune, profile.breakQ,
                      profile.breakDecay * damp, profile.breakSweep, profile.breakNoise);

        for (std::size_t p = 0; p < profile.partialHz.size(); ++p) {
            if (profile.partialGain[p] <= 0.0f || profile.partialHz[p] <= 0.0f) {
                continue;
            }
            addPartial(clip.samples, 0, rate, profile.partialHz[p] * detune * rng.range(0.97f, 1.03f),
                       profile.partialGain[p], profile.partialDecay * damp, rng.range(0.0f, kTwoPi));
        }
    }

    // ---- material-specific scatter ----
    switch (material) {
        case SoundMaterial::Glass: {
            // Shards land after the initial break and keep ringing; this is what
            // makes glass unmistakable.
            const int shards = 5 + static_cast<int>(rng.nextUInt() % 4u);
            for (int s = 0; s < shards; ++s) {
                const float onset = rng.range(0.03f, profile.timings.breakSeconds * 0.65f);
                addPartial(clip.samples, static_cast<std::size_t>(onset * rate), rate,
                           rng.range(2200.0f, 6400.0f) * detune, rng.range(0.05f, 0.16f),
                           rng.range(0.03f, 0.12f), rng.range(0.0f, kTwoPi));
            }
            break;
        }
        case SoundMaterial::Gravel: {
            // Loose stones tumbling: short bright clicks over the first half.
            const int grains = 6 + static_cast<int>(rng.nextUInt() % 5u);
            for (int g = 0; g < grains; ++g) {
                const float onset = rng.range(0.005f, profile.timings.breakSeconds * 0.5f);
                addNoiseBurst(clip.samples, static_cast<std::size_t>(onset * rate), rate, rng,
                              rng.range(1400.0f, 3600.0f), 2.5f, rng.range(0.004f, 0.012f),
                              rng.range(0.10f, 0.28f), 0.05f);
            }
            break;
        }
        case SoundMaterial::Snow:
        case SoundMaterial::Sand: {
            // Granular materials collapse in stages rather than all at once.
            const int packets = 3 + static_cast<int>(rng.nextUInt() % 3u);
            for (int k = 0; k < packets; ++k) {
                const float onset = rng.range(0.01f, profile.timings.breakSeconds * 0.45f);
                addNoiseBurst(clip.samples, static_cast<std::size_t>(onset * rate), rate, rng,
                              profile.breakCentreHz * rng.range(0.7f, 1.4f), 0.8f,
                              rng.range(0.015f, 0.04f), rng.range(0.12f, 0.26f), 0.12f);
            }
            break;
        }
        default:
            break;
    }

    normalisePeak(clip, profile.breakPeak);
    conditionClip(clip);
    return clip;
}

PcmClip synthBlockPlace(SoundMaterial material, std::uint64_t seed)
{
    const Profile& profile = profileOf(material);
    const auto     rate    = static_cast<float>(kSynthSampleRate);

    PcmClip clip;
    clip.samples = silentBuffer(profile.timings.placeSeconds, rate);
    if (clip.samples.empty()) {
        return clip;
    }

    SynthRng    rng{seed ^ 0xA5A5A5A5A5A5A5A5ull};
    const float detune = rng.range(0.90f, 1.12f);
    const float damp   = rng.range(0.88f, 1.15f);

    if (material == SoundMaterial::Water) {
        addSplash(clip.samples, rate, rng, 0.8f, profile.placeDecay * damp, 2);
    } else {
        // Half the Q and a much lower centre than the break cue: setting a block
        // down excites the body, not the surface.
        addSweptBurst(clip.samples, rate, rng, profile.placeCentreHz * detune,
                      std::max(profile.breakQ * 0.5f, 0.5f), profile.placeDecay * damp, 0.5f, 1.0f);

        // Only the lowest mode, and briefly - a place cue that rings reads as a
        // second, unexplained event.
        if (profile.partialHz[0] > 0.0f) {
            addPartial(clip.samples, 0, rate, profile.partialHz[0] * detune,
                       profile.partialGain[0] * 0.7f, profile.partialDecay * 0.5f * damp,
                       rng.range(0.0f, kTwoPi));
        }
    }

    normalisePeak(clip, profile.breakPeak * profile.placeGain);
    conditionClip(clip);
    return clip;
}

PcmClip synthFootstep(SoundMaterial material, std::uint64_t seed)
{
    const Profile& profile = profileOf(material);
    const auto     rate    = static_cast<float>(kSynthSampleRate);

    PcmClip clip;
    clip.samples = silentBuffer(profile.timings.footstepSeconds, rate);
    if (clip.samples.empty()) {
        return clip;
    }

    SynthRng    rng{seed ^ 0x5DEECE66Dull};
    const float detune = rng.range(0.84f, 1.20f);
    const float damp   = rng.range(0.80f, 1.25f);

    // Stage 1: the heel. Short, filtered, quiet.
    addSweptBurst(clip.samples, rate, rng, profile.stepCentreHz * detune, profile.stepQ,
                  profile.stepDecay * damp, 0.45f, 1.0f);

    // Stage 2: the scuff, a few milliseconds later. This is the difference
    // between a walk cycle and a metronome; without it every step is one tap.
    const float scuffOnset = rng.range(0.012f, 0.035f);
    addNoiseBurst(clip.samples, static_cast<std::size_t>(scuffOnset * rate), rate, rng,
                  profile.stepCentreHz * detune * rng.range(1.1f, 1.9f), 0.6f,
                  rng.range(0.010f, 0.026f), rng.range(0.25f, 0.5f),
                  profile.timings.footstepSeconds);

    if (material == SoundMaterial::Water) {
        addDroplet(clip.samples, static_cast<std::size_t>(rng.range(0.01f, 0.05f) * rate), rate,
                   rng.range(500.0f, 900.0f), rng.range(1500.0f, 2400.0f), 0.015f, 0.25f);
    }
    if (profile.partialHz[0] > 0.0f) {
        // A hint of the material's lowest mode keeps stone from sounding like
        // gravel at low volume.
        addPartial(clip.samples, 0, rate, profile.partialHz[0] * detune,
                   profile.partialGain[0] * 0.30f, profile.partialDecay * 0.35f, 0.0f);
    }

    normalisePeak(clip, profile.breakPeak * profile.stepGain);
    conditionClip(clip, 0.002f);
    return clip;
}

PcmClip synthMiningLoop(SoundMaterial material, std::uint64_t seed)
{
    const Profile& profile   = profileOf(material);
    const auto     rate      = static_cast<float>(kSynthSampleRate);
    const float    loopFade  = 0.06f;
    const std::size_t target = synthFrameCount(profile.timings.miningSeconds);

    PcmClip clip;
    if (target == 0) {
        return clip;
    }
    // Generated long, then crossfaded back down to exactly `target`, so the
    // clip length stays a pure function of the material.
    clip.samples.assign(target + static_cast<std::size_t>(loopFade * rate), 0.0f);

    SynthRng rng{seed ^ 0xC0FFEEull};

    // ---- layer 1: the grind. Continuous bandpassed noise whose cutoff wanders
    //      under a slow lowpassed-noise LFO, so it never sits on one pitch.
    {
        Svf     filter;
        OnePole lfo;
        lfo.configure(3.5f, rate);
        for (std::size_t i = 0; i < clip.samples.size(); ++i) {
            const float wander = lfo.process(rng.nextBipolar());
            filter.configure(profile.breakCentreHz * (0.55f + 0.30f * wander), 1.1f, rate);
            filter.process(rng.nextBipolar());
            clip.samples[i] += filter.band() * profile.mineGrindGain;
        }
    }

    // ---- layer 2: the chips. Placed on an exact grid that divides the *final*
    //      length, so the pattern is periodic across the loop seam and no chip
    //      is cut in half by the crossfade below.
    const auto chipCount =
        std::max<std::size_t>(1, static_cast<std::size_t>(profile.timings.miningSeconds *
                                                          profile.mineChipsPerSecond));
    const std::size_t chipStride = target / chipCount;
    if (chipStride > 0) {
        const float chipSeconds =
            0.55f * static_cast<float>(chipStride) / rate;  // always ends before the next chip
        for (std::size_t c = 0; c < chipCount; ++c) {
            addNoiseBurst(clip.samples, c * chipStride, rate, rng,
                          profile.breakCentreHz * rng.range(0.75f, 1.45f), 2.2f,
                          rng.range(0.006f, 0.016f), rng.range(0.35f, 0.75f), chipSeconds);
        }
    }

    clip.sampleRate = kSynthSampleRate;
    makeLoopable(clip, loopFade);
    normalisePeak(clip, 0.45f);
    // A 0.6 ms fade only: this runs across the loop seam every pass, and a
    // longer one would be heard as a periodic click of its own.
    conditionClip(clip, 0.0006f);
    return clip;
}

PcmClip synthAmbientWind(std::uint64_t seed, float seconds)
{
    const auto        rate     = static_cast<float>(kSynthSampleRate);
    const float       loopFade = 0.9f;
    const std::size_t target   = synthFrameCount(seconds);

    PcmClip clip;
    if (target == 0) {
        return clip;
    }
    clip.samples.assign(target + static_cast<std::size_t>(loopFade * rate), 0.0f);

    SynthRng rng{seed ^ 0x1D2C3B4Aull};

    // Three bands, each with its own slow gain LFO and cutoff wander. Layering
    // independently modulated bands is what makes noise read as *wind* rather
    // than as hiss: real wind is broadband but its spectral tilt moves.
    struct Band {
        float centreHz;
        float q;
        float gain;
        float lfoHz;
        float wanderHz;
    };
    constexpr std::array<Band, 3> kBands{
        Band{110.0f, 0.5f, 0.55f, 0.07f, 0.9f},   // body
        Band{620.0f, 0.6f, 0.34f, 0.11f, 1.4f},   // mid rush
        Band{2400.0f, 0.7f, 0.14f, 0.17f, 2.2f},  // treetop hiss
    };

    for (const Band& band : kBands) {
        Svf     filter;
        OnePole wander;
        wander.configure(band.wanderHz, rate);
        const float lfoPhase = rng.range(0.0f, kTwoPi);
        // The LFO frequency is snapped to an integer number of cycles over the
        // final loop length; otherwise the gust pattern jumps at the seam.
        const float loopSeconds = static_cast<float>(target) / rate;
        const float cycles      = std::max(1.0f, std::round(band.lfoHz * loopSeconds));
        const float lfoHz       = cycles / loopSeconds;

        for (std::size_t i = 0; i < clip.samples.size(); ++i) {
            const float t    = static_cast<float>(i) / rate;
            const float gust = 0.55f + 0.45f * std::sin(kTwoPi * lfoHz * t + lfoPhase);
            const float move = wander.process(rng.nextBipolar());
            filter.configure(band.centreHz * (1.0f + 0.35f * move), band.q, rate);
            filter.process(rng.nextBipolar());
            clip.samples[i] += filter.band() * band.gain * gust;
        }
    }

    clip.sampleRate = kSynthSampleRate;
    makeLoopable(clip, loopFade);
    normalisePeak(clip, 0.55f);
    conditionClip(clip, 0.0006f);
    return clip;
}

PcmClip synthUiClick(std::uint64_t seed, bool back)
{
    const auto rate = static_cast<float>(kSynthSampleRate);

    PcmClip clip;
    clip.samples = silentBuffer(kUiClickSeconds, rate);
    if (clip.samples.empty()) {
        return clip;
    }

    SynthRng rng{seed ^ (back ? 0xBACC0DEull : 0xC11CCull)};

    // Two partials a fifth apart, sweeping up to confirm and down to cancel.
    // Wooden rather than beepy: the decay is short and the second partial is
    // detuned slightly, which removes the "phone keypad" quality.
    const float root  = back ? 720.0f : 980.0f;
    const float endHz = back ? root * 0.62f : root * 1.5f;
    addDroplet(clip.samples, 0, rate, root, endHz, 0.028f, 0.55f);
    addDroplet(clip.samples, static_cast<std::size_t>(0.006f * rate), rate, root * 1.49f,
               endHz * 1.51f, 0.020f, 0.22f);
    // A whisper of noise on the attack gives it a physical onset.
    addNoiseBurst(clip.samples, 0, rate, rng, 3200.0f, 1.2f, 0.004f, 0.18f, 0.02f);

    normalisePeak(clip, 0.5f);
    conditionClip(clip, 0.0015f);
    return clip;
}

}  // namespace voxl::audio
