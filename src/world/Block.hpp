#pragma once

// Block identity and material properties.
//
// THIS HEADER IS A CONTRACT. Meshing reads the render layer and texture layers,
// physics reads the collision shape, lighting reads opacity and emission, and
// persistence writes numeric ids. Block ids are stable and are written to disk;
// see docs/TECHNICAL_DESIGN.md for the id-remapping rules on format upgrades.

#include "world/VoxelTypes.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace voxl {

/// 16 bits leaves room for far more block types than this game will ever have
/// while keeping a palette entry small. Air is guaranteed to be 0 so that a
/// zero-filled chunk is a valid empty chunk.
using BlockId = std::uint16_t;

/// Which pass a block's geometry is drawn in. The order matters: the renderer
/// draws Opaque front-to-back, then Cutout, then Translucent back-to-front.
enum class RenderLayer : std::uint8_t {
    /// Fully opaque, depth-written, occludes neighbours.
    Opaque = 0,
    /// Binary alpha (leaves, grass tufts). Depth-written, alpha-tested, and
    /// never culls the face behind it.
    Cutout = 1,
    /// Blended (water, glass). Depth-tested but not depth-written.
    Translucent = 2,
};

inline constexpr std::size_t kRenderLayerCount = 3;

/// How a block interacts with the player's collision volume.
enum class CollisionShape : std::uint8_t {
    /// Occupies no space; the player walks straight through.
    None = 0,
    /// Full 1x1x1 solid cube.
    Cube = 1,
    /// Occupies space but does not stop movement (water). Physics may still
    /// apply drag or buoyancy.
    Fluid = 2,
};

/// Static, immutable description of one block type.
///
/// Everything here is read from many threads during meshing and lighting, so
/// the registry hands out const references and never mutates a type after
/// registration.
struct BlockType {
    std::string name = "air";

    /// Texture-array layer index per face, indexed by `Direction`. Using six
    /// entries rather than a single index is what lets grass have a distinct
    /// top, side and bottom without a special case in the mesher.
    std::array<std::uint16_t, kDirectionCount> textureLayers{};

    RenderLayer    renderLayer    = RenderLayer::Opaque;
    CollisionShape collisionShape = CollisionShape::None;

    /// Blocks all light and hides the neighbouring face. False for air, glass,
    /// water and leaves. Distinct from `renderLayer`: a block can be drawn in
    /// the opaque pass yet not be light-opaque.
    bool opaque = false;

    /// Participates in fluid simulation and is rendered with the water shader.
    bool liquid = false;

    /// How much sunlight this block removes as light passes through it. 0 means
    /// perfectly clear; water uses 1-2 so caves under lakes darken naturally.
    /// Ignored when `opaque` is true (which attenuates fully).
    std::uint8_t lightAttenuation = 0;

    /// Light emitted by this block, 0-15. Non-zero makes it a light source that
    /// seeds block-light propagation.
    std::uint8_t lightEmission = 0;

    /// Relative hardness used for break timing. 0 breaks instantly.
    float hardness = 0.0f;

    /// Identifier of the sound set played when the block is broken, placed or
    /// walked on. Resolved by the audio system at load time.
    std::string soundGroup = "stone";

    /// True when this block should never be selected by the interaction
    /// raycast (currently only air).
    bool replaceable = false;

    /// Convenience: a face between two blocks of the same non-opaque type
    /// (water against water) is not drawn.
    [[nodiscard]] bool selfCulling() const noexcept { return renderLayer == RenderLayer::Translucent; }

    [[nodiscard]] bool solid() const noexcept { return collisionShape == CollisionShape::Cube; }
};

/// The built-in block ids.
///
/// These values are written into save files. Never renumber an existing entry;
/// append new blocks at the end and bump the world format version.
namespace blocks {
inline constexpr BlockId Air         = 0;
inline constexpr BlockId Stone       = 1;
inline constexpr BlockId Dirt        = 2;
inline constexpr BlockId Grass       = 3;
inline constexpr BlockId Sand        = 4;
inline constexpr BlockId Gravel      = 5;
inline constexpr BlockId Water       = 6;
inline constexpr BlockId Wood        = 7;
inline constexpr BlockId Leaves      = 8;
inline constexpr BlockId Planks      = 9;
inline constexpr BlockId Glass       = 10;
inline constexpr BlockId Snow        = 11;
inline constexpr BlockId Sandstone   = 12;
inline constexpr BlockId Cobblestone = 13;
inline constexpr BlockId Bedrock     = 14;
inline constexpr BlockId Glowstone   = 15;
inline constexpr BlockId Clay        = 16;
inline constexpr BlockId Ice         = 17;

inline constexpr BlockId Count = 18;
}  // namespace blocks

/// Immutable lookup table of every block type.
///
/// Construct exactly one, populate it during start-up, then treat it as read
/// only: worker threads query it concurrently without synchronisation, which is
/// only safe because nothing mutates it after `finalise()`.
class BlockRegistry {
public:
    BlockRegistry();

    /// Registers a block at a specific id. Ids must be registered exactly once.
    /// Only legal before `finalise()`.
    void registerBlock(BlockId id, BlockType type);

    /// Seals the registry. After this call the registry is safe to read from
    /// any thread and further registration is a fatal error.
    void finalise();

    /// Unknown ids resolve to air rather than being an error, so a save file
    /// written by a newer build degrades gracefully instead of crashing.
    [[nodiscard]] const BlockType& get(BlockId id) const noexcept
    {
        return id < m_types.size() ? m_types[id] : m_types[blocks::Air];
    }

    [[nodiscard]] const BlockType& operator[](BlockId id) const noexcept { return get(id); }

    /// Linear search by name; intended for tooling and config parsing, not for
    /// per-frame use. Returns `blocks::Air` when the name is unknown.
    [[nodiscard]] BlockId findByName(std::string_view name) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_types.size(); }

    // ---- Hot-path predicates, kept inline and branch-light ----

    [[nodiscard]] bool isAir(BlockId id) const noexcept { return id == blocks::Air; }
    [[nodiscard]] bool isOpaque(BlockId id) const noexcept { return get(id).opaque; }
    [[nodiscard]] bool isSolid(BlockId id) const noexcept { return get(id).solid(); }
    [[nodiscard]] bool isLiquid(BlockId id) const noexcept { return get(id).liquid; }
    [[nodiscard]] RenderLayer renderLayer(BlockId id) const noexcept { return get(id).renderLayer; }

    /// The single rule that governs hidden-face removal.
    ///
    /// A face of `block` pointing at `neighbourBlock` is drawn unless the
    /// neighbour fully hides it. An opaque neighbour always hides it; a
    /// translucent block hides the matching face of an identical block so that
    /// a body of water has no internal surfaces.
    [[nodiscard]] bool facesHidden(BlockId block, BlockId neighbourBlock) const noexcept
    {
        if (neighbourBlock == blocks::Air) {
            return false;
        }
        const BlockType& neighbourType = get(neighbourBlock);
        if (neighbourType.opaque) {
            return true;
        }
        return block == neighbourBlock && neighbourType.selfCulling();
    }

private:
    std::vector<BlockType> m_types;
    bool m_finalised = false;
};

/// Builds the registry containing every block in `namespace blocks`, already
/// finalised. This is the only construction path the game uses; tests may build
/// their own registry to exercise edge cases.
[[nodiscard]] BlockRegistry createDefaultBlockRegistry();

}  // namespace voxl
