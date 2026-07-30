#include <catch2/catch_test_macros.hpp>

#include "audio/AudioEngine.hpp"
#include "audio/SoundBank.hpp"
#include "audio/SynthSounds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// NOTHING IN THIS FILE MAY REQUIRE AN AUDIO DEVICE.
//
// The suite runs on a build agent with no sound card, so every engine test uses
// `AudioConfig::enabled = false`, which lands in the exact same no-op state a
// failed `ma_device_init` produces. The synthesis tests never touch the engine
// at all - that separation is why the interesting half of the audio system is
// testable headless.

using namespace voxl::audio;

namespace {

/// The invariants every clip must satisfy before it is allowed near the mixer.
/// Checked as a count rather than one CHECK per sample so a broken generator
/// produces one readable failure instead of fifty thousand.
struct ClipReport {
    std::size_t nonFinite = 0;
    std::size_t outOfRange = 0;
    float       peak       = 0.0f;
    float       first      = 0.0f;
    float       last       = 0.0f;
};

[[nodiscard]] ClipReport inspect(const PcmClip& clip)
{
    ClipReport report;
    for (const float sample : clip.samples) {
        if (!std::isfinite(sample)) {
            ++report.nonFinite;
            continue;
        }
        if (sample < -1.0f || sample > 1.0f) {
            ++report.outOfRange;
        }
        report.peak = std::max(report.peak, std::fabs(sample));
    }
    if (!clip.samples.empty()) {
        report.first = clip.samples.front();
        report.last  = clip.samples.back();
    }
    return report;
}

/// Every generated cue must be finite, in range, audible, and silent at both
/// edges. The edge condition is the anti-click contract: a voice that starts or
/// stops on a non-zero sample produces a step discontinuity, which is heard as
/// a pop regardless of how good the rest of the sound is.
void checkClipContract(const PcmClip& clip, const std::string& label)
{
    INFO("clip: " << label << " (" << clip.frameCount() << " frames)");
    REQUIRE_FALSE(clip.empty());
    const ClipReport report = inspect(clip);
    CHECK(report.nonFinite == 0);
    CHECK(report.outOfRange == 0);
    CHECK(report.peak > 0.01f);          // not accidentally silent
    CHECK(report.peak <= 1.0f);
    CHECK(std::fabs(report.first) < 1.0e-6f);
    CHECK(std::fabs(report.last) < 1.0e-6f);
    CHECK(clip.sampleRate == kSynthSampleRate);
}

/// Compared through a helper rather than inline, so a failing CHECK does not
/// ask Catch2 to stringify two buffers of fifty thousand floats.
[[nodiscard]] bool sameSamples(const PcmClip& a, const PcmClip& b)
{
    return a.samples == b.samples;
}

constexpr std::array<SoundMaterial, kSoundMaterialCount> kAllMaterials{
    SoundMaterial::Stone, SoundMaterial::Dirt,  SoundMaterial::Grass,
    SoundMaterial::Sand,  SoundMaterial::Gravel, SoundMaterial::Wood,
    SoundMaterial::Glass, SoundMaterial::Snow,  SoundMaterial::Water,
};

/// A bank small enough to build inside a test: one variation per event and no
/// ambient bed, which is the expensive one.
[[nodiscard]] BankRecipe tinyRecipe()
{
    BankRecipe recipe;
    recipe.breakVariations  = 2;
    recipe.placeVariations  = 1;
    recipe.stepVariations   = 2;
    recipe.miningVariations = 1;
    recipe.includeAmbient   = false;
    recipe.includeUi        = true;
    return recipe;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Deterministic RNG
// ---------------------------------------------------------------------------

TEST_CASE("SynthRng is reproducible and decorrelated across seeds", "[audio][synth]")
{
    SynthRng a{12345};
    SynthRng b{12345};
    SynthRng c{12346};

    bool differed = false;
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t left = a.nextUInt();
        CHECK(left == b.nextUInt());
        differed = differed || left != c.nextUInt();
    }
    CHECK(differed);

    SynthRng bounded{7};
    for (int i = 0; i < 256; ++i) {
        const float value = bounded.nextFloat();
        CHECK(value >= 0.0f);
        CHECK(value < 1.0f);
        const float bipolar = bounded.nextBipolar();
        CHECK(bipolar >= -1.0f);
        CHECK(bipolar < 1.0f);
        const float ranged = bounded.range(-3.0f, 5.0f);
        CHECK(ranged >= -3.0f);
        CHECK(ranged <= 5.0f);
    }
}

// ---------------------------------------------------------------------------
//  Clip conditioning
// ---------------------------------------------------------------------------

TEST_CASE("conditionClip scrubs non-finite samples and enforces the range", "[audio][synth]")
{
    PcmClip clip;
    clip.samples.assign(4800, 0.5f);
    clip.samples[10] = std::numeric_limits<float>::quiet_NaN();
    clip.samples[11] = std::numeric_limits<float>::infinity();
    clip.samples[12] = -std::numeric_limits<float>::infinity();

    // The hot samples must sit clear of the edge fade, which is
    // 0.003 s * 48 kHz = 144 samples at each end. Probing the knee at index 100
    // measured the knee AND the fade-in ramp at once (gain 0.79 there), so the
    // assertion below failed for a reason that had nothing to do with the knee.
    constexpr std::size_t kHot = 2000;
    clip.samples[kHot]     = 42.0f;
    clip.samples[kHot + 1] = -42.0f;

    conditionClip(clip);

    const ClipReport report = inspect(clip);
    CHECK(report.nonFinite == 0);
    CHECK(report.outOfRange == 0);
    CHECK(clip.samples.front() == 0.0f);
    CHECK(clip.samples.back() == 0.0f);
    // The soft knee keeps a hot sample loud rather than crushing it to nothing.
    CHECK(clip.samples[kHot] > 0.85f);
    CHECK(clip.samples[kHot + 1] < -0.85f);
}

TEST_CASE("conditionClip still zeroes both edges when there is no room to fade",
          "[audio][synth]")
{
    PcmClip clip;
    clip.samples.assign(3, 0.5f);
    conditionClip(clip, 1.0f);  // absurd fade request
    CHECK(clip.samples.front() == 0.0f);
    CHECK(clip.samples.back() == 0.0f);
    CHECK(clip.samples.size() == 3);
}

TEST_CASE("makeLoopable shortens the buffer and matches the seam", "[audio][synth]")
{
    constexpr float kFade = 0.05f;

    PcmClip clip;
    clip.samples.resize(kSynthSampleRate);  // one second
    SynthRng rng{99};
    for (float& sample : clip.samples) {
        sample = 0.4f * rng.nextBipolar();
    }
    const std::size_t before = clip.frameCount();

    makeLoopable(clip, kFade);

    CHECK(clip.loopable);
    CHECK(clip.frameCount() == before - static_cast<std::size_t>(kFade * kSynthSampleRate));
    // The crossfade must not have introduced a level jump; the equal-power
    // curve keeps the energy of two noise sources roughly constant.
    const ClipReport report = inspect(clip);
    CHECK(report.nonFinite == 0);
    CHECK(report.peak > 0.05f);
}

TEST_CASE("normalisePeak leaves silence alone", "[audio][synth]")
{
    PcmClip silent;
    silent.samples.assign(128, 0.0f);
    normalisePeak(silent, 0.9f);
    CHECK(inspect(silent).peak == 0.0f);

    PcmClip quiet;
    quiet.samples.assign(128, 0.01f);
    normalisePeak(quiet, 0.5f);
    CHECK(inspect(quiet).peak > 0.49f);
    CHECK(inspect(quiet).peak <= 0.5f + 1.0e-5f);
}

// ---------------------------------------------------------------------------
//  Generators: length, contract, determinism
// ---------------------------------------------------------------------------

TEST_CASE("every block cue is the length its material profile advertises",
          "[audio][synth]")
{
    for (const SoundMaterial material : kAllMaterials) {
        const MaterialTimings& timings = materialTimings(material);
        INFO("material " << toString(material));

        CHECK(synthBlockBreak(material, 1).frameCount() ==
              synthFrameCount(timings.breakSeconds));
        CHECK(synthBlockPlace(material, 1).frameCount() ==
              synthFrameCount(timings.placeSeconds));
        CHECK(synthFootstep(material, 1).frameCount() ==
              synthFrameCount(timings.footstepSeconds));
        // The mining bed is generated long and crossfaded back down; the point
        // of the test is that the trim lands on exactly the advertised length.
        CHECK(synthMiningLoop(material, 1).frameCount() ==
              synthFrameCount(timings.miningSeconds));
    }
}

TEST_CASE("every block cue satisfies the clip contract", "[audio][synth]")
{
    for (const SoundMaterial material : kAllMaterials) {
        const std::string name{toString(material)};
        checkClipContract(synthBlockBreak(material, 0xABCDEF), name + ".break");
        checkClipContract(synthBlockPlace(material, 0xABCDEF), name + ".place");
        checkClipContract(synthFootstep(material, 0xABCDEF), name + ".step");
        checkClipContract(synthMiningLoop(material, 0xABCDEF), name + ".mine");
    }
}

TEST_CASE("the ambient bed and the UI blips satisfy the clip contract", "[audio][synth]")
{
    const PcmClip wind = synthAmbientWind(4242, 1.0f);
    checkClipContract(wind, "ambient.wind");
    CHECK(wind.loopable);
    CHECK(wind.frameCount() == synthFrameCount(1.0f));

    const PcmClip click = synthUiClick(7, false);
    const PcmClip back  = synthUiClick(7, true);
    checkClipContract(click, "ui.click");
    checkClipContract(back, "ui.back");
    CHECK(click.frameCount() == synthFrameCount(kUiClickSeconds));
    CHECK(back.frameCount() == synthFrameCount(kUiClickSeconds));
    // Confirm and cancel must not be the same buffer.
    CHECK_FALSE(sameSamples(click, back));
}

TEST_CASE("synthesis is deterministic for a seed and varies between seeds",
          "[audio][synth]")
{
    for (const SoundMaterial material : kAllMaterials) {
        INFO("material " << toString(material));

        const PcmClip first  = synthBlockBreak(material, 0x1234'5678'9ABC'DEF0ull);
        const PcmClip repeat = synthBlockBreak(material, 0x1234'5678'9ABC'DEF0ull);
        const PcmClip other  = synthBlockBreak(material, 0x1234'5678'9ABC'DEF1ull);

        CHECK(sameSamples(first, repeat));
        CHECK(first.frameCount() == other.frameCount());
        CHECK_FALSE(sameSamples(first, other));
    }

    // Same for the other event types, on one material - the generators share a
    // seeding path, so one spot check per type is enough to catch a regression.
    CHECK(sameSamples(synthBlockPlace(SoundMaterial::Wood, 5),
                      synthBlockPlace(SoundMaterial::Wood, 5)));
    CHECK(sameSamples(synthFootstep(SoundMaterial::Sand, 5),
                      synthFootstep(SoundMaterial::Sand, 5)));
    CHECK(sameSamples(synthMiningLoop(SoundMaterial::Glass, 5),
                      synthMiningLoop(SoundMaterial::Glass, 5)));
    CHECK(sameSamples(synthAmbientWind(5, 0.5f), synthAmbientWind(5, 0.5f)));
    CHECK(sameSamples(synthUiClick(5), synthUiClick(5)));
}

TEST_CASE("different materials produce different break sounds", "[audio][synth]")
{
    // A generator that ignored its material argument would pass every test
    // above; this is the one that catches it.
    const PcmClip stone = synthBlockBreak(SoundMaterial::Stone, 11);
    const PcmClip glass = synthBlockBreak(SoundMaterial::Glass, 11);
    const PcmClip dirt  = synthBlockBreak(SoundMaterial::Dirt, 11);
    CHECK_FALSE(sameSamples(stone, glass));
    CHECK_FALSE(sameSamples(stone, dirt));
    CHECK(glass.frameCount() != dirt.frameCount());
}

// ---------------------------------------------------------------------------
//  Material / sound-group resolution
// ---------------------------------------------------------------------------

TEST_CASE("sound groups map onto materials", "[audio][bank]")
{
    // These strings are the ones world/Block.cpp actually registers.
    CHECK(materialFromSoundGroup("stone") == SoundMaterial::Stone);
    CHECK(materialFromSoundGroup("dirt") == SoundMaterial::Dirt);
    CHECK(materialFromSoundGroup("grass") == SoundMaterial::Grass);
    CHECK(materialFromSoundGroup("sand") == SoundMaterial::Sand);
    CHECK(materialFromSoundGroup("gravel") == SoundMaterial::Gravel);
    CHECK(materialFromSoundGroup("wood") == SoundMaterial::Wood);
    CHECK(materialFromSoundGroup("glass") == SoundMaterial::Glass);
    CHECK(materialFromSoundGroup("snow") == SoundMaterial::Snow);
    CHECK(materialFromSoundGroup("water") == SoundMaterial::Water);
    // Unknown groups must degrade, not fail.
    CHECK(materialFromSoundGroup("obsidian") == SoundMaterial::Stone);
    CHECK(materialFromSoundGroup("") == SoundMaterial::Stone);

    for (const SoundMaterial material : kAllMaterials) {
        CHECK(materialFromSoundGroup(toString(material)) == material);
    }
}

// ---------------------------------------------------------------------------
//  SoundBank
// ---------------------------------------------------------------------------

TEST_CASE("a bank resolves every block group and keeps air silent", "[audio][bank]")
{
    const SoundBank bank = SoundBank::create(tinyRecipe());

    CHECK(bank.cueCount() == kSoundMaterialCount * kBlockSoundEventCount + 2);
    CHECK(bank.sampleBytes() > 0);
    CHECK(bank.clipCount() >= bank.cueCount());

    for (const SoundMaterial material : kAllMaterials) {
        const std::string group{toString(material)};
        INFO("group " << group);
        for (std::size_t e = 0; e < kBlockSoundEventCount; ++e) {
            const auto  event = static_cast<BlockSoundEvent>(e);
            const CueId byString = bank.blockCue(group, event);
            const CueId byEnum   = bank.blockCue(material, event);
            CHECK(byString != kInvalidCue);
            CHECK(byString == byEnum);

            const Cue* cue = bank.get(byString);
            REQUIRE(cue != nullptr);
            CHECK_FALSE(cue->empty());
            CHECK(cue->name == "block." + group + "." + std::string{toString(event)});
        }
    }

    // Air, and anything else with no sound, must resolve to nothing at all.
    CHECK(bank.blockCue("none", BlockSoundEvent::Break) == kInvalidCue);
    CHECK(bank.blockCue("", BlockSoundEvent::Break) == kInvalidCue);
    // An unrecognised group falls back to stone rather than going silent.
    CHECK(bank.blockCue("obsidian", BlockSoundEvent::Break) ==
          bank.blockCue(SoundMaterial::Stone, BlockSoundEvent::Break));

    // The mining bed must be flagged looping, or a break timer would fire one
    // burst and then run in silence.
    const Cue* mine = bank.get(bank.blockCue(SoundMaterial::Stone, BlockSoundEvent::Mine));
    REQUIRE(mine != nullptr);
    CHECK(mine->style.looping);
    CHECK(mine->variations.front().loopable);

    // UI cues are on their own bus and are not spatialised.
    const CueId click = bank.find("ui.click");
    REQUIRE(click != kInvalidCue);
    const Cue* clickCue = bank.get(click);
    REQUIRE(clickCue != nullptr);
    CHECK(clickCue->style.category == SoundCategory::Ui);
    CHECK_FALSE(clickCue->style.spatial);

    CHECK(bank.find("no.such.cue") == kInvalidCue);
    CHECK(bank.get(kInvalidCue) == nullptr);
    CHECK(bank.variation(kInvalidCue, 0) == nullptr);
}

TEST_CASE("variation selection cycles and never goes out of bounds", "[audio][bank]")
{
    BankRecipe recipe   = tinyRecipe();
    recipe.stepVariations = 4;
    const SoundBank bank = SoundBank::create(recipe);

    const CueId step = bank.blockCue(SoundMaterial::Stone, BlockSoundEvent::Step);
    REQUIRE(step != kInvalidCue);
    const Cue* cue = bank.get(step);
    REQUIRE(cue != nullptr);
    REQUIRE(cue->variations.size() == 4);

    // Repeated footsteps must not all be the same buffer, which is the entire
    // point of having variations.
    CHECK_FALSE(sameSamples(cue->variations[0], cue->variations[1]));
    CHECK_FALSE(sameSamples(cue->variations[1], cue->variations[2]));

    for (std::uint32_t roll = 0; roll < 32; ++roll) {
        const PcmClip* clip = bank.variation(step, roll);
        REQUIRE(clip != nullptr);
        CHECK(clip == &cue->variations[roll % 4]);
    }
    // A huge roll must still land in range.
    CHECK(bank.variation(step, 0xFFFFFFFFu) != nullptr);
}

TEST_CASE("a bank is reproducible from its seed", "[audio][bank]")
{
    BankRecipe recipe = tinyRecipe();
    recipe.seed       = 0xFEEDFACEull;

    const SoundBank first  = SoundBank::create(recipe);
    const SoundBank second = SoundBank::create(recipe);
    REQUIRE(first.cueCount() == second.cueCount());

    for (std::size_t i = 0; i < first.cueCount(); ++i) {
        const Cue* a = first.get(static_cast<CueId>(i));
        const Cue* b = second.get(static_cast<CueId>(i));
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        INFO("cue " << a->name);
        CHECK(a->name == b->name);
        REQUIRE(a->variations.size() == b->variations.size());
        for (std::size_t v = 0; v < a->variations.size(); ++v) {
            CHECK(sameSamples(a->variations[v], b->variations[v]));
        }
    }

    recipe.seed              = 0xFEEDFACFull;
    const SoundBank different = SoundBank::create(recipe);
    const CueId     cueId     = different.blockCue(SoundMaterial::Stone, BlockSoundEvent::Break);
    REQUIRE(cueId != kInvalidCue);
    const Cue* fromFirst     = first.get(cueId);
    const Cue* fromDifferent = different.get(cueId);
    REQUIRE(fromFirst != nullptr);
    REQUIRE(fromDifferent != nullptr);
    CHECK(fromFirst->name == fromDifferent->name);
    CHECK_FALSE(sameSamples(fromFirst->variations[0], fromDifferent->variations[0]));
}

TEST_CASE("a bank rejects empty and duplicate cues", "[audio][bank]")
{
    SoundBank bank;

    Cue empty;
    empty.name = "empty";
    CHECK(bank.add(std::move(empty)) == kInvalidCue);

    Cue silent;
    silent.name = "silent";
    silent.variations.emplace_back();  // a clip with no samples
    CHECK(bank.add(std::move(silent)) == kInvalidCue);

    Cue good;
    good.name = "good";
    good.variations.push_back(synthUiClick(1));
    const CueId id = bank.add(std::move(good));
    CHECK(id != kInvalidCue);
    CHECK(bank.find("good") == id);

    Cue duplicate;
    duplicate.name = "good";
    duplicate.variations.push_back(synthUiClick(2));
    CHECK(bank.add(std::move(duplicate)) == kInvalidCue);
    CHECK(bank.cueCount() == 1);
}

// ---------------------------------------------------------------------------
//  AudioEngine without a device
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] AudioConfig headlessConfig()
{
    AudioConfig config;
    // The same state a failed ma_device_init leaves behind, reachable on a
    // machine that happens to *have* a sound card.
    config.enabled = false;
    return config;
}

}  // namespace

TEST_CASE("an engine with no device reports unavailable and never plays", "[audio][engine]")
{
    AudioEngine engine{headlessConfig()};

    CHECK_FALSE(engine.available());
    CHECK_FALSE(engine.unavailableReason().empty());

    const AudioStats stats = engine.stats();
    CHECK_FALSE(stats.available);
    CHECK(stats.activeVoices == 0);
    CHECK(stats.voicesStarted == 0);
    CHECK(stats.voicesDropped == 0);
    CHECK(stats.voiceCapacity == engine.config().voiceCount);

    auto bank = std::make_shared<const SoundBank>(SoundBank::create(tinyRecipe()));
    engine.setSoundBank(bank);
    CHECK(engine.soundBank() == bank);

    // Cue lookup still works with no device; only playback is inert.
    const CueId breakCue = engine.blockCue("stone", BlockSoundEvent::Break);
    CHECK(breakCue != kInvalidCue);
    CHECK(engine.findCue("ui.click") != kInvalidCue);
    CHECK(engine.blockCue("none", BlockSoundEvent::Break) == kInvalidCue);

    // Every play path must return an invalid handle and change nothing.
    CHECK_FALSE(engine.play(breakCue).valid());
    CHECK_FALSE(engine.playAt(breakCue, glm::vec3{1.0f, 2.0f, 3.0f}).valid());
    CHECK_FALSE(engine.playUi(engine.findCue("ui.click")).valid());
    CHECK_FALSE(engine.playBlockBreak("stone", glm::vec3{0.0f}).valid());
    CHECK_FALSE(engine.playBlockPlace("wood", glm::vec3{0.0f}).valid());
    CHECK_FALSE(engine.playFootstep("grass", glm::vec3{0.0f}).valid());
    CHECK_FALSE(engine.playBlockBreak("none", glm::vec3{0.0f}).valid());
    CHECK_FALSE(engine.playBlockBreak("obsidian", glm::vec3{0.0f}).valid());

    CHECK(engine.stats().voicesStarted == 0);
    CHECK(engine.stats().voicesDropped == 0);
}

TEST_CASE("voice operations on an unavailable engine are safe no-ops", "[audio][engine]")
{
    AudioEngine engine{headlessConfig()};
    engine.setSoundBank(std::make_shared<const SoundBank>(SoundBank::create(tinyRecipe())));

    const VoiceHandle invalid{};
    CHECK_FALSE(invalid.valid());
    CHECK_FALSE(engine.voicePlaying(invalid));

    // Deliberately fabricated handles, including out-of-range ones: none of
    // these may index anything.
    const VoiceHandle fabricated{7, 3};
    const VoiceHandle wild{0xFFFFFFFFu, 0xFFFFFFFFu};
    CHECK_FALSE(engine.voicePlaying(fabricated));
    CHECK_FALSE(engine.voicePlaying(wild));

    engine.stopVoice(invalid);
    engine.stopVoice(fabricated, 0.0f);
    engine.stopVoice(wild, 10.0f);
    engine.updateVoice(invalid, PlayParams{});
    engine.updateVoice(wild, PlayParams{});
    engine.stopAll();

    const CueId mine = engine.blockCue("stone", BlockSoundEvent::Mine);
    REQUIRE(mine != kInvalidCue);
    CHECK_FALSE(engine.sustain(invalid, mine, PlayParams{}).valid());
    CHECK_FALSE(engine.sustain(wild, kInvalidCue, PlayParams{}).valid());

    SUCCEED("no crash");
}

TEST_CASE("volume and listener state round-trip without a device", "[audio][engine]")
{
    AudioEngine engine{headlessConfig()};

    engine.setMasterVolume(0.25f);
    CHECK(engine.masterVolume() == 0.25f);
    engine.setMasterVolume(-1.0f);  // clamped, not rejected
    CHECK(engine.masterVolume() == 0.0f);
    engine.setMasterVolume(1.0f);

    engine.setCategoryVolume(SoundCategory::World, 0.4f);
    engine.setCategoryVolume(SoundCategory::Ui, 0.9f);
    CHECK(engine.categoryVolume(SoundCategory::World) == 0.4f);
    CHECK(engine.categoryVolume(SoundCategory::Ui) == 0.9f);

    CHECK_FALSE(engine.muted());
    engine.setMuted(true);
    CHECK(engine.muted());
    // Muting must not disturb the sliders the menu will restore.
    CHECK(engine.categoryVolume(SoundCategory::Ui) == 0.9f);
    engine.setMuted(false);

    ListenerState state;
    state.position = glm::vec3{10.0f, -4.0f, 96.5f};
    state.forward  = glm::vec3{0.0f, 0.0f, 1.0f};
    state.up       = glm::vec3{0.0f, 1.0f, 0.0f};
    engine.setListener(state);

    const ListenerState read = engine.listener();
    CHECK(read.position == state.position);
    CHECK(read.forward == state.forward);
    CHECK(read.up == state.up);
}

TEST_CASE("the sustained and retrigger helpers tolerate a dead engine", "[audio][engine]")
{
    AudioEngine engine{headlessConfig()};
    engine.setSoundBank(std::make_shared<const SoundBank>(SoundBank::create(tinyRecipe())));

    const CueId mine = engine.blockCue("stone", BlockSoundEvent::Mine);
    REQUIRE(mine != kInvalidCue);

    SustainedVoice sustained;
    CHECK_FALSE(sustained.active());
    for (int frame = 0; frame < 10; ++frame) {
        PlayParams params;
        params.position = glm::vec3{static_cast<float>(frame), 64.0f, 0.0f};
        sustained.update(engine, mine, params);
    }
    // With no device nothing plays, so the helper must report inactive rather
    // than pretending it holds a voice.
    CHECK_FALSE(sustained.active());
    sustained.stop(engine);
    CHECK_FALSE(sustained.active());

    // An invalid cue must stop rather than start anything.
    sustained.update(engine, kInvalidCue, PlayParams{});
    CHECK_FALSE(sustained.active());

    RetriggerVoice retrigger;
    // The first update always fires; after that the interval governs, and the
    // whole thing must be safe when nothing can actually be heard.
    CHECK(retrigger.update(engine, mine, PlayParams{}, 0.0f, 0.25f));
    CHECK_FALSE(retrigger.update(engine, mine, PlayParams{}, 0.1f, 0.25f));
    CHECK_FALSE(retrigger.update(engine, mine, PlayParams{}, 0.1f, 0.25f));
    CHECK(retrigger.update(engine, mine, PlayParams{}, 0.1f, 0.25f));
    retrigger.reset();
    CHECK(retrigger.update(engine, mine, PlayParams{}, 0.0f, 0.25f));
}

TEST_CASE("an engine constructed with the shipping config always starts", "[audio][engine]")
{
    // On a developer machine this opens a real device; on a build agent it does
    // not. Either outcome is a pass - what is asserted is that construction
    // succeeds and that the reported state is self-consistent, which is the
    // property that keeps the game bootable everywhere.
    AudioEngine engine{AudioConfig{}};

    const AudioStats stats = engine.stats();
    CHECK(stats.available == engine.available());
    CHECK(engine.available() == engine.unavailableReason().empty());

    if (!engine.available()) {
        CHECK_FALSE(engine.play(0).valid());
    } else {
        CHECK(stats.deviceSampleRate > 0);
        CHECK(stats.deviceChannels > 0);
    }

    engine.stopAll();
    SUCCEED("construction and teardown are safe in both states");
}
