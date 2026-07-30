#pragma once

// Named, playable sound cues and the block-material cue table.
//
// A SoundBank is pure CPU data: PCM buffers plus the metadata that says how a
// cue should be randomised and mixed. It knows nothing about miniaudio, about a
// device, or about whether audio is even available, which is what lets it be
// built and tested on a machine with no sound card - and built on a JobSystem
// worker while the rest of start-up continues.
//
// LIFETIME CONTRACT WITH AudioEngine
// ----------------------------------
// A playing voice holds a raw pointer into a cue's sample buffer. The engine
// therefore takes a `shared_ptr<const SoundBank>`, which makes two things true:
//   * the bank cannot be mutated once it is attached (the pointer is const), so
//     `add()` can never reallocate a buffer a voice is reading, and
//   * the bank cannot be destroyed while the engine holds it.
// Build the bank fully, then attach it. Never attach and then extend.
//
// Thread safety: const access is safe from any thread once construction has
// finished. `add()` is not synchronised and is only legal during construction.

#include "audio/SynthSounds.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voxl::audio {

/// Mixer bus. Each has its own user-facing volume slider on top of the master.
enum class SoundCategory : std::uint8_t {
    /// Anything diegetic: blocks, footsteps, ambience.
    World = 0,
    /// Menus and HUD. Kept separate so muting the world does not make the menu
    /// feel broken.
    Ui = 1,
};

inline constexpr std::size_t kSoundCategoryCount = 2;

[[nodiscard]] std::string_view toString(SoundCategory category) noexcept;

/// Index into a SoundBank. Stable for the life of the bank.
using CueId = std::uint32_t;

/// Returned by every failed lookup. Passing it to a play call is a silent
/// no-op, which is deliberate: a missing cue must never be a crash or a log
/// spam source in the middle of a frame.
inline constexpr CueId kInvalidCue = 0xFFFFFFFFu;

/// The per-material events a block can produce.
enum class BlockSoundEvent : std::uint8_t {
    /// The block was destroyed.
    Break = 0,
    /// A block was placed.
    Place = 1,
    /// The player's foot landed on it.
    Step = 2,
    /// Looping bed for "a break timer is running against this block".
    Mine = 3,
};

inline constexpr std::size_t kBlockSoundEventCount = 4;

[[nodiscard]] std::string_view toString(BlockSoundEvent event) noexcept;

/// How a cue is randomised and spatialised. Jitter is multiplicative and
/// symmetric: 0.06 means the pitch lands somewhere in [0.94, 1.06].
///
/// Randomising per play is the whole reason repeated footsteps do not sound
/// like a machine gun; the variation list handles timbre, this handles the
/// finer-grained wobble.
struct CueStyle {
    float gain        = 1.0f;
    float pitchJitter = 0.06f;
    float gainJitter  = 0.12f;

    SoundCategory category = SoundCategory::World;

    /// Content is designed to loop; a voice started with it plays until stopped.
    bool looping = false;

    /// Whether `play()` positions this cue in the world by default. Ambience and
    /// UI are flat.
    bool spatial = true;

    /// Full volume within `minDistance` blocks, silent beyond `maxDistance`.
    float minDistance = 2.0f;
    float maxDistance = 56.0f;
};

/// A named set of interchangeable recordings of the same event.
struct Cue {
    std::string          name;
    CueStyle             style{};
    std::vector<PcmClip> variations;

    [[nodiscard]] bool empty() const noexcept { return variations.empty(); }
};

/// Controls which cues `SoundBank::create` generates and how many variations
/// each gets. Tests build a deliberately tiny bank; the game uses the default.
struct BankRecipe {
    std::uint64_t seed = 0x566F786C41756469ull;  ///< "VoxlAudi"

    std::uint32_t breakVariations   = 4;
    std::uint32_t placeVariations   = 3;
    std::uint32_t stepVariations    = 5;  ///< most repeated cue, so the most variants
    std::uint32_t miningVariations  = 1;  ///< looping; play-time pitch jitter is enough

    bool  includeAmbient = true;
    bool  includeUi      = true;
    float ambientSeconds = 6.0f;
};

namespace detail {
/// A block-cue table where every slot means "no cue". Zero-initialising the
/// array would instead make every slot alias cue 0, which is a real cue.
[[nodiscard]] constexpr std::array<std::array<CueId, kBlockSoundEventCount>, kSoundMaterialCount>
emptyBlockCueTable() noexcept
{
    std::array<std::array<CueId, kBlockSoundEventCount>, kSoundMaterialCount> table{};
    for (auto& row : table) {
        for (CueId& id : row) {
            id = kInvalidCue;
        }
    }
    return table;
}
}  // namespace detail

class SoundBank {
public:
    SoundBank() = default;

    SoundBank(const SoundBank&)            = delete;
    SoundBank& operator=(const SoundBank&) = delete;
    SoundBank(SoundBank&&) noexcept        = default;
    SoundBank& operator=(SoundBank&&) noexcept = default;

    /// Synthesises every cue described by `recipe`. Pure CPU work with no
    /// global state, so it is safe on a worker thread; it costs a few hundred
    /// milliseconds at the default recipe and is worth keeping off the main
    /// thread during start-up.
    [[nodiscard]] static SoundBank create(const BankRecipe& recipe);

    /// The shipping bank.
    [[nodiscard]] static SoundBank createDefault(std::uint64_t seed = BankRecipe{}.seed);

    /// Appends a cue and returns its id. Construction-time only - see the
    /// lifetime contract at the top of this file. A cue with no variations, or
    /// with a name that is already taken, is rejected and returns kInvalidCue.
    CueId add(Cue cue);

    /// Registers `cue` as the given material's handler for `event`, in addition
    /// to adding it under its own name. Used by `create`; exposed so a
    /// file-backed bank can be assembled the same way.
    CueId addBlockCue(SoundMaterial material, BlockSoundEvent event, Cue cue);

    [[nodiscard]] CueId       find(std::string_view name) const noexcept;
    [[nodiscard]] const Cue*  get(CueId id) const noexcept;

    /// Resolves a `BlockType::soundGroup` string. Returns kInvalidCue for
    /// "none" and for the empty string so air stays silent; any other unknown
    /// group falls back to stone, matching `materialFromSoundGroup`.
    [[nodiscard]] CueId blockCue(std::string_view soundGroup,
                                 BlockSoundEvent  event) const noexcept;
    [[nodiscard]] CueId blockCue(SoundMaterial material, BlockSoundEvent event) const noexcept;

    /// Picks one of the cue's variations. `roll` is any value; it is reduced
    /// modulo the variation count, so the caller's RNG choice is the only source
    /// of randomness and playback stays reproducible under a fixed seed.
    [[nodiscard]] const PcmClip* variation(CueId id, std::uint32_t roll) const noexcept;

    [[nodiscard]] std::size_t cueCount() const noexcept { return m_cues.size(); }
    [[nodiscard]] std::size_t clipCount() const noexcept;
    /// Total PCM footprint, for the debug overlay and the start-up log.
    [[nodiscard]] std::size_t sampleBytes() const noexcept;

    [[nodiscard]] const std::vector<Cue>& cues() const noexcept { return m_cues; }

private:
    std::vector<Cue> m_cues;

    /// material -> event -> cue. Flat array rather than a map: nine materials
    /// times four events is smaller than one hash bucket, and the lookup is on
    /// the break/place path.
    std::array<std::array<CueId, kBlockSoundEventCount>, kSoundMaterialCount> m_blockCues =
        detail::emptyBlockCueTable();
};

}  // namespace voxl::audio
