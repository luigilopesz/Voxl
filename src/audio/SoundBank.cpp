#include "audio/SoundBank.hpp"

#include "core/Log.hpp"
#include "core/Time.hpp"

#include <utility>

namespace voxl::audio {
namespace {

/// Cue naming scheme. Block cues are also reachable by name so a config file or
/// a debug command can address one directly:
///     block.stone.break   block.wood.step   block.glass.mine
/// Non-block cues use the same dotted convention: ambient.wind, ui.click.
[[nodiscard]] std::string blockCueName(SoundMaterial material, BlockSoundEvent event)
{
    std::string name = "block.";
    name += toString(material);
    name += '.';
    name += toString(event);
    return name;
}

/// Derives a distinct, stable seed per (material, event, variation) from the
/// bank seed. Mixing with odd constants rather than adding keeps two adjacent
/// variations from sharing a prefix of the PCG stream, which would make them
/// sound like near-duplicates.
[[nodiscard]] constexpr std::uint64_t deriveSeed(std::uint64_t base, std::uint64_t material,
                                                 std::uint64_t event,
                                                 std::uint64_t variation) noexcept
{
    std::uint64_t mixed = base;
    mixed ^= (material + 1u) * 0x9E3779B97F4A7C15ull;
    mixed ^= (event + 1u) * 0xC2B2AE3D27D4EB4Full;
    mixed ^= (variation + 1u) * 0x165667B19E3779F9ull;
    // One finalising round of splitmix64 so nearby inputs decorrelate.
    mixed ^= mixed >> 30;
    mixed *= 0xBF58476D1CE4E5B9ull;
    mixed ^= mixed >> 27;
    mixed *= 0x94D049BB133111EBull;
    mixed ^= mixed >> 31;
    return mixed;
}

/// Per-event mixing defaults. Break is the loudest thing a block can do; a
/// footstep must sit well under the player's own attention threshold.
[[nodiscard]] CueStyle styleFor(BlockSoundEvent event) noexcept
{
    CueStyle style;
    style.category = SoundCategory::World;
    style.spatial  = true;
    switch (event) {
        case BlockSoundEvent::Break:
            style.gain        = 0.85f;
            style.pitchJitter = 0.07f;
            style.gainJitter  = 0.10f;
            style.maxDistance = 64.0f;
            break;
        case BlockSoundEvent::Place:
            style.gain        = 0.60f;
            style.pitchJitter = 0.08f;
            style.gainJitter  = 0.10f;
            style.maxDistance = 48.0f;
            break;
        case BlockSoundEvent::Step:
            // The widest jitter in the bank. Footsteps fire two or three times
            // a second from a source one metre from the listener's ears, which
            // is exactly the condition under which repetition is obvious.
            style.gain        = 0.40f;
            style.pitchJitter = 0.14f;
            style.gainJitter  = 0.22f;
            style.minDistance = 1.0f;
            style.maxDistance = 24.0f;
            break;
        case BlockSoundEvent::Mine:
            style.gain        = 0.55f;
            style.pitchJitter = 0.05f;
            style.gainJitter  = 0.0f;  // a looping bed must not jump in level
            style.looping     = true;
            style.maxDistance = 32.0f;
            break;
    }
    return style;
}

[[nodiscard]] PcmClip synthesise(SoundMaterial material, BlockSoundEvent event,
                                 std::uint64_t seed)
{
    switch (event) {
        case BlockSoundEvent::Break: return synthBlockBreak(material, seed);
        case BlockSoundEvent::Place: return synthBlockPlace(material, seed);
        case BlockSoundEvent::Step:  return synthFootstep(material, seed);
        case BlockSoundEvent::Mine:  return synthMiningLoop(material, seed);
    }
    return PcmClip{};
}

[[nodiscard]] std::uint32_t variationCount(const BankRecipe& recipe, BlockSoundEvent event) noexcept
{
    switch (event) {
        case BlockSoundEvent::Break: return recipe.breakVariations;
        case BlockSoundEvent::Place: return recipe.placeVariations;
        case BlockSoundEvent::Step:  return recipe.stepVariations;
        case BlockSoundEvent::Mine:  return recipe.miningVariations;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Enum naming
// ---------------------------------------------------------------------------

std::string_view toString(SoundCategory category) noexcept
{
    switch (category) {
        case SoundCategory::World: return "world";
        case SoundCategory::Ui:    return "ui";
    }
    return "world";
}

std::string_view toString(BlockSoundEvent event) noexcept
{
    switch (event) {
        case BlockSoundEvent::Break: return "break";
        case BlockSoundEvent::Place: return "place";
        case BlockSoundEvent::Step:  return "step";
        case BlockSoundEvent::Mine:  return "mine";
    }
    return "break";
}

// ---------------------------------------------------------------------------
//  SoundBank
// ---------------------------------------------------------------------------

CueId SoundBank::add(Cue cue)
{
    if (cue.variations.empty()) {
        VOXL_LOG_WARN("sound cue '{}' has no variations and was dropped", cue.name);
        return kInvalidCue;
    }
    if (!cue.name.empty() && find(cue.name) != kInvalidCue) {
        VOXL_LOG_WARN("sound cue '{}' is already registered; the second one was dropped", cue.name);
        return kInvalidCue;
    }

    // Drop any variation that ended up empty rather than letting the mixer meet
    // a zero-length clip on the audio thread.
    std::erase_if(cue.variations, [](const PcmClip& clip) { return clip.empty(); });
    if (cue.variations.empty()) {
        VOXL_LOG_WARN("sound cue '{}' synthesised to silence and was dropped", cue.name);
        return kInvalidCue;
    }

    const auto id = static_cast<CueId>(m_cues.size());
    m_cues.push_back(std::move(cue));
    return id;
}

CueId SoundBank::addBlockCue(SoundMaterial material, BlockSoundEvent event, Cue cue)
{
    const CueId id = add(std::move(cue));
    if (id == kInvalidCue) {
        return kInvalidCue;
    }
    const auto materialIndex = static_cast<std::size_t>(material);
    const auto eventIndex    = static_cast<std::size_t>(event);
    if (materialIndex < m_blockCues.size() && eventIndex < kBlockSoundEventCount) {
        m_blockCues[materialIndex][eventIndex] = id;
    }
    return id;
}

CueId SoundBank::find(std::string_view name) const noexcept
{
    // Linear over a few dozen cues. A hash map would be faster in the abstract
    // and slower here, and every hot-path caller resolves its id once anyway.
    for (std::size_t i = 0; i < m_cues.size(); ++i) {
        if (m_cues[i].name == name) {
            return static_cast<CueId>(i);
        }
    }
    return kInvalidCue;
}

const Cue* SoundBank::get(CueId id) const noexcept
{
    return id < m_cues.size() ? &m_cues[id] : nullptr;
}

CueId SoundBank::blockCue(SoundMaterial material, BlockSoundEvent event) const noexcept
{
    const auto materialIndex = static_cast<std::size_t>(material);
    const auto eventIndex    = static_cast<std::size_t>(event);
    if (materialIndex >= m_blockCues.size() || eventIndex >= kBlockSoundEventCount) {
        return kInvalidCue;
    }
    return m_blockCues[materialIndex][eventIndex];
}

CueId SoundBank::blockCue(std::string_view soundGroup, BlockSoundEvent event) const noexcept
{
    // Air's group. Silence here is a feature: breaking air is not an event, and
    // resolving it to stone would give every failed interaction a noise.
    if (soundGroup.empty() || soundGroup == "none") {
        return kInvalidCue;
    }
    return blockCue(materialFromSoundGroup(soundGroup), event);
}

const PcmClip* SoundBank::variation(CueId id, std::uint32_t roll) const noexcept
{
    const Cue* cue = get(id);
    if (cue == nullptr || cue->variations.empty()) {
        return nullptr;
    }
    const std::size_t index = roll % static_cast<std::uint32_t>(cue->variations.size());
    return &cue->variations[index];
}

std::size_t SoundBank::clipCount() const noexcept
{
    std::size_t count = 0;
    for (const Cue& cue : m_cues) {
        count += cue.variations.size();
    }
    return count;
}

std::size_t SoundBank::sampleBytes() const noexcept
{
    std::size_t bytes = 0;
    for (const Cue& cue : m_cues) {
        for (const PcmClip& clip : cue.variations) {
            bytes += clip.byteSize();
        }
    }
    return bytes;
}

// ---------------------------------------------------------------------------
//  Construction
// ---------------------------------------------------------------------------

SoundBank SoundBank::create(const BankRecipe& recipe)
{
    Stopwatch watch;
    SoundBank bank;

    for (std::size_t materialIndex = 0; materialIndex < kSoundMaterialCount; ++materialIndex) {
        const auto material = static_cast<SoundMaterial>(materialIndex);
        for (std::size_t eventIndex = 0; eventIndex < kBlockSoundEventCount; ++eventIndex) {
            const auto  event = static_cast<BlockSoundEvent>(eventIndex);
            const auto  count = variationCount(recipe, event);
            if (count == 0) {
                continue;
            }

            Cue cue;
            cue.name  = blockCueName(material, event);
            cue.style = styleFor(event);
            cue.variations.reserve(count);
            for (std::uint32_t v = 0; v < count; ++v) {
                cue.variations.push_back(synthesise(
                    material, event,
                    deriveSeed(recipe.seed, materialIndex, eventIndex, v)));
            }
            bank.addBlockCue(material, event, std::move(cue));
        }
    }

    if (recipe.includeAmbient) {
        Cue wind;
        wind.name              = "ambient.wind";
        wind.style.gain        = 0.18f;
        wind.style.pitchJitter = 0.0f;
        wind.style.gainJitter  = 0.0f;
        wind.style.looping     = true;
        // Non-spatial: the wind bed is everywhere, and panning it would make the
        // whole world appear to swing when the player turns their head.
        wind.style.spatial = false;
        wind.variations.push_back(
            synthAmbientWind(deriveSeed(recipe.seed, 100, 0, 0), recipe.ambientSeconds));
        bank.add(std::move(wind));
    }

    if (recipe.includeUi) {
        for (int back = 0; back < 2; ++back) {
            Cue click;
            click.name              = back != 0 ? "ui.back" : "ui.click";
            click.style.category    = SoundCategory::Ui;
            click.style.spatial     = false;
            click.style.gain        = 0.55f;
            click.style.pitchJitter = 0.02f;
            click.style.gainJitter  = 0.0f;
            click.variations.push_back(synthUiClick(
                deriveSeed(recipe.seed, 200, static_cast<std::uint64_t>(back), 0), back != 0));
            bank.add(std::move(click));
        }
    }

    VOXL_LOG_INFO("SoundBank: {} cue(s), {} clip(s), {} KiB of PCM, synthesised in {:.0f} ms",
                  bank.cueCount(), bank.clipCount(), bank.sampleBytes() / 1024,
                  watch.elapsedMilliseconds());
    return bank;
}

SoundBank SoundBank::createDefault(std::uint64_t seed)
{
    BankRecipe recipe;
    recipe.seed = seed;
    return create(recipe);
}

}  // namespace voxl::audio
