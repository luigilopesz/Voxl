#include "world/Block.hpp"

#include "core/Log.hpp"

#include <array>
#include <string_view>
#include <utility>

namespace voxl {
namespace {

// ---------------------------------------------------------------------------
//  TEXTURE ARRAY LAYER ORDER
// ---------------------------------------------------------------------------
//  The renderer builds a GL_TEXTURE_2D_ARRAY whose layer N is the image named
//  at index N below. This order is a contract between this file and the texture
//  loader: it is also written into every packed vertex, so inserting a layer in
//  the middle silently retextures the world. Append only, and mirror any change
//  in docs/TECHNICAL_DESIGN.md.
// ---------------------------------------------------------------------------
enum TextureLayer : std::uint16_t {
    TexStone = 0,
    TexDirt,
    TexGrassTop,
    TexGrassSide,
    TexSand,
    TexGravel,
    TexWater,
    TexLogTop,
    TexLogSide,
    TexLeaves,
    TexPlanks,
    TexGlass,
    TexSnow,
    TexSandstoneTop,
    TexSandstoneSide,
    TexSandstoneBottom,
    TexCobblestone,
    TexBedrock,
    TexGlowstone,
    TexClay,
    TexIce,
    TexLayerCount,
};

/// Names the texture loader resolves to `assets/textures/blocks/<name>.png`.
constexpr std::array<std::string_view, TexLayerCount> kTextureLayerNames = {
    "stone",      "dirt",           "grass_top",       "grass_side",  "sand",
    "gravel",     "water",          "log_top",         "log_side",    "leaves",
    "planks",     "glass",          "snow",            "sandstone_top",
    "sandstone_side", "sandstone_bottom", "cobblestone", "bedrock",   "glowstone",
    "clay",       "ice",
};

/// Same texture on all six faces.
[[nodiscard]] constexpr std::array<std::uint16_t, kDirectionCount> uniformFaces(std::uint16_t layer) noexcept
{
    return {layer, layer, layer, layer, layer, layer};
}

/// Distinct top/side/bottom, in Direction order: NegX, PosX, NegY, PosY, NegZ, PosZ.
[[nodiscard]] constexpr std::array<std::uint16_t, kDirectionCount> columnFaces(
    std::uint16_t top, std::uint16_t side, std::uint16_t bottom) noexcept
{
    return {side, side, bottom, top, side, side};
}

/// Placeholder occupying an id that has not been registered yet. The empty name
/// is what `registerBlock` uses to detect a duplicate registration, and it also
/// makes an unregistered id behave exactly like air if it is ever queried.
[[nodiscard]] BlockType unregisteredSlot()
{
    BlockType slot;
    slot.name        = std::string{};
    slot.replaceable = true;
    return slot;
}

/// Fully solid, light-blocking cube. Every stone-like block starts here.
[[nodiscard]] BlockType solidCube(std::string_view name, std::uint16_t layer, float hardness,
                                  std::string_view soundGroup)
{
    BlockType type;
    type.name           = std::string{name};
    type.textureLayers  = uniformFaces(layer);
    type.renderLayer    = RenderLayer::Opaque;
    type.collisionShape = CollisionShape::Cube;
    type.opaque         = true;
    type.hardness       = hardness;
    type.soundGroup     = std::string{soundGroup};
    return type;
}

}  // namespace

// ---------------------------------------------------------------- registry --

BlockRegistry::BlockRegistry()
{
    // `get()` falls back to index 0 for unknown ids, so the table must never be
    // empty even before anything is registered.
    m_types.assign(1, unregisteredSlot());
}

void BlockRegistry::registerBlock(BlockId id, BlockType type)
{
    VOXL_CHECK(!m_finalised, "registerBlock() called after finalise()");
    VOXL_CHECK(!type.name.empty(), "block types must have a non-empty name");

    if (id >= m_types.size()) {
        m_types.resize(static_cast<std::size_t>(id) + 1u, unregisteredSlot());
    }
    VOXL_CHECK(m_types[id].name.empty(), "block id {} registered twice", id);

    m_types[id] = std::move(type);
}

void BlockRegistry::finalise()
{
    VOXL_CHECK(!m_finalised, "finalise() called twice");
    VOXL_CHECK(!m_types.empty(), "an empty registry has no air block to fall back on");

    std::size_t holes = 0;
    for (std::size_t i = 0; i < m_types.size(); ++i) {
        if (m_types[i].name.empty()) {
            ++holes;
        }
    }
    if (holes > 0) {
        // Not fatal: a hole reads as air, which is the documented degradation
        // path for save files written by a build with more blocks than this one.
        VOXL_LOG_WARN("BlockRegistry has {} unregistered id(s); they resolve to air", holes);
    }

    m_types.shrink_to_fit();
    m_finalised = true;
    VOXL_LOG_INFO("BlockRegistry finalised: {} block type(s)", m_types.size());
}

BlockId BlockRegistry::findByName(std::string_view name) const noexcept
{
    if (name.empty()) {
        return blocks::Air;  // never match the unregistered placeholders
    }
    for (std::size_t i = 0; i < m_types.size(); ++i) {
        if (m_types[i].name == name) {
            return static_cast<BlockId>(i);
        }
    }
    return blocks::Air;
}

// ------------------------------------------------------- default registry --

BlockRegistry createDefaultBlockRegistry()
{
    BlockRegistry registry;

    {
        BlockType air;
        air.name           = "air";
        air.textureLayers  = uniformFaces(0);  // never sampled; air is never meshed
        air.renderLayer    = RenderLayer::Opaque;
        air.collisionShape = CollisionShape::None;
        air.opaque         = false;
        air.replaceable    = true;
        air.hardness       = 0.0f;
        air.soundGroup     = "none";
        registry.registerBlock(blocks::Air, std::move(air));
    }

    registry.registerBlock(blocks::Stone, solidCube("stone", TexStone, 1.5f, "stone"));
    registry.registerBlock(blocks::Dirt, solidCube("dirt", TexDirt, 0.5f, "dirt"));

    {
        BlockType grass = solidCube("grass", TexGrassTop, 0.6f, "grass");
        // Bottom reuses the dirt layer so a broken grass block looks continuous
        // with the soil underneath it.
        grass.textureLayers = columnFaces(TexGrassTop, TexGrassSide, TexDirt);
        registry.registerBlock(blocks::Grass, std::move(grass));
    }

    registry.registerBlock(blocks::Sand, solidCube("sand", TexSand, 0.5f, "sand"));
    registry.registerBlock(blocks::Gravel, solidCube("gravel", TexGravel, 0.6f, "gravel"));

    {
        BlockType water;
        water.name           = "water";
        water.textureLayers  = uniformFaces(TexWater);
        water.renderLayer    = RenderLayer::Translucent;
        water.collisionShape = CollisionShape::Fluid;
        water.opaque         = false;
        water.liquid         = true;
        // 2 per block means light dies about seven blocks down, which darkens
        // the bottom of a lake without making shallow water look muddy.
        water.lightAttenuation = 2;
        water.hardness         = 0.0f;
        water.soundGroup       = "water";
        registry.registerBlock(blocks::Water, std::move(water));
    }

    {
        BlockType wood = solidCube("wood", TexLogSide, 2.0f, "wood");
        wood.textureLayers = columnFaces(TexLogTop, TexLogSide, TexLogTop);
        registry.registerBlock(blocks::Wood, std::move(wood));
    }

    {
        BlockType leaves;
        leaves.name           = "leaves";
        leaves.textureLayers  = uniformFaces(TexLeaves);
        leaves.renderLayer    = RenderLayer::Cutout;
        leaves.collisionShape = CollisionShape::Cube;
        // Not light-opaque: a canopy has to dim the forest floor gradually
        // rather than casting a hard black shadow the shape of the tree.
        leaves.opaque           = false;
        leaves.lightAttenuation = 1;
        leaves.hardness         = 0.2f;
        leaves.soundGroup       = "grass";
        registry.registerBlock(blocks::Leaves, std::move(leaves));
    }

    registry.registerBlock(blocks::Planks, solidCube("planks", TexPlanks, 2.0f, "wood"));

    {
        BlockType glass;
        glass.name           = "glass";
        glass.textureLayers  = uniformFaces(TexGlass);
        glass.renderLayer    = RenderLayer::Translucent;
        glass.collisionShape = CollisionShape::Cube;
        glass.opaque         = false;
        // Perfectly clear: a glass roof must not dim the room under it.
        glass.lightAttenuation = 0;
        glass.hardness         = 0.3f;
        glass.soundGroup       = "glass";
        registry.registerBlock(blocks::Glass, std::move(glass));
    }

    registry.registerBlock(blocks::Snow, solidCube("snow", TexSnow, 0.2f, "snow"));

    {
        BlockType sandstone = solidCube("sandstone", TexSandstoneSide, 0.8f, "stone");
        sandstone.textureLayers =
            columnFaces(TexSandstoneTop, TexSandstoneSide, TexSandstoneBottom);
        registry.registerBlock(blocks::Sandstone, std::move(sandstone));
    }

    registry.registerBlock(blocks::Cobblestone,
                           solidCube("cobblestone", TexCobblestone, 2.0f, "stone"));

    {
        BlockType bedrock = solidCube("bedrock", TexBedrock, -1.0f, "stone");
        // Negative hardness is the agreed sentinel for "cannot be broken";
        // interaction code must test for it before computing a break time.
        registry.registerBlock(blocks::Bedrock, std::move(bedrock));
    }

    {
        BlockType glowstone = solidCube("glowstone", TexGlowstone, 0.3f, "glass");
        glowstone.lightEmission = 15;
        registry.registerBlock(blocks::Glowstone, std::move(glowstone));
    }

    registry.registerBlock(blocks::Clay, solidCube("clay", TexClay, 0.6f, "dirt"));

    {
        BlockType ice;
        ice.name           = "ice";
        ice.textureLayers  = uniformFaces(TexIce);
        ice.renderLayer    = RenderLayer::Translucent;
        ice.collisionShape = CollisionShape::Cube;
        ice.opaque         = false;
        // Slightly cloudy, so a frozen lake still reads as a surface from above.
        ice.lightAttenuation = 1;
        ice.hardness         = 0.5f;
        ice.soundGroup       = "glass";
        registry.registerBlock(blocks::Ice, std::move(ice));
    }

    registry.finalise();

    VOXL_LOG_INFO("block texture array requires {} layer(s), layer 0 = '{}'",
                  kTextureLayerNames.size(), kTextureLayerNames[0]);
    return registry;
}

}  // namespace voxl
