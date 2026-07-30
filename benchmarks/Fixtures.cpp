#include "Fixtures.hpp"

#include "core/Log.hpp"
#include "world/ChunkStorage.hpp"

#include <algorithm>
#include <memory>

namespace voxl::bench {

namespace {

std::uint64_t                     g_seed        = TerrainSettings{}.seed;
std::unique_ptr<TerrainGenerator> g_generator;

/// Chunk-local index of the column centre, used by the biome scan.
constexpr std::int32_t kColumnProbeOffset = kChunkSize / 2;

}  // namespace

const BlockRegistry& registry()
{
    static const BlockRegistry instance = createDefaultBlockRegistry();
    return instance;
}

void setSeed(std::uint64_t value)
{
    VOXL_CHECK(g_generator == nullptr, "setSeed() after the generator was already built");
    g_seed = value;
}

std::uint64_t seed() noexcept
{
    return g_seed;
}

const TerrainGenerator& generator()
{
    if (!g_generator) {
        TerrainSettings settings;
        settings.seed = g_seed;
        g_generator   = std::make_unique<TerrainGenerator>(settings);
    }
    return *g_generator;
}

// ------------------------------------------------------------ biome search --

BiomeSite findBiomeSite(BiomeId biome, std::int32_t maxRadiusChunks)
{
    BiomeSite site;
    site.biome = biome;

    // Expanding square rings rather than a raster scan: the nearest match to the
    // origin is the one found, which keeps the chosen site stable when the radius
    // limit is changed.
    for (std::int32_t radius = 0; radius <= maxRadiusChunks; ++radius) {
        for (std::int32_t cz = -radius; cz <= radius; ++cz) {
            for (std::int32_t cx = -radius; cx <= radius; ++cx) {
                const bool onRing = std::max(std::abs(cx), std::abs(cz)) == radius;
                if (!onRing) {
                    continue;
                }

                const std::int32_t worldX = cx * kChunkSize + kColumnProbeOffset;
                const std::int32_t worldZ = cz * kChunkSize + kColumnProbeOffset;
                const ColumnSample sample = generator().sampleColumn(worldX, worldZ);
                if (sample.biome != biome) {
                    continue;
                }

                site.found          = true;
                site.worldX         = worldX;
                site.worldZ         = worldZ;
                site.surfaceY       = sample.surfaceY;
                site.surfaceSection = ChunkPos{cx, blockToChunkAxis(sample.surfaceY), cz};
                return site;
            }
        }
    }
    return site;
}

// ------------------------------------------------------- chunk construction --

ChunkPtr generateChunk(const ChunkPos& position, LodLevel level)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->setLod(level);
    generator().generate(*chunk, level);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

ChunkPtr uniformChunk(const ChunkPos& position, BlockId fill)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(fill);
    chunk->fillLight(ChunkStorage::kMaxLightLevel, 0);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

ChunkPtr checkerboardChunk(const ChunkPos& position)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(blocks::Air);
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                if (((x + y + z) & 1) == 0) {
                    chunk->storage().set(localIndex(x, y, z), blocks::Stone);
                }
            }
        }
    }
    chunk->fillLight(ChunkStorage::kMaxLightLevel, 0);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

std::vector<std::size_t> spreadSolidBlocks(const Chunk& chunk, std::size_t count)
{
    std::vector<std::size_t> picked;
    if (count == 0) {
        return picked;
    }
    picked.reserve(count);

    // Walk the whole chunk with a stride so the damage is spread rather than
    // clustered in the first hundred indices; clustered damage would give the
    // sorted SubVoxelStore an unrealistically cheap insert pattern.
    const std::size_t stride = std::max<std::size_t>(kChunkVolume / count, 1);
    for (std::size_t index = 0; index < kChunkVolume && picked.size() < count; index += stride) {
        if (chunk.getBlock(index) != blocks::Air) {
            picked.push_back(index);
        }
    }
    // Second pass fills the shortfall when the strided walk landed on air.
    for (std::size_t index = 0; index < kChunkVolume && picked.size() < count; ++index) {
        if (chunk.getBlock(index) != blocks::Air &&
            std::find(picked.begin(), picked.end(), index) == picked.end()) {
            picked.push_back(index);
        }
    }
    std::sort(picked.begin(), picked.end());
    return picked;
}

// ----------------------------------------------------------- neighbourhoods --

Scene generatedScene(const ChunkPos& centre, LodLevel level)
{
    Scene scene;
    scene.centre = generateChunk(centre, level);
    scene.neighbourhood.setCentre(centre);

    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t sectionY = centre.y + dy;
        if (sectionY < 0 || sectionY >= kWorldSectionCount) {
            continue;  // answered by the out-of-world rules
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                scene.neighbourhood.setChunk(
                    dx, dy, dz,
                    generateChunk(ChunkPos{centre.x + dx, sectionY, centre.z + dz}, level));
            }
        }
    }
    scene.neighbourhood.setChunk(0, 0, 0, scene.centre);
    return scene;
}

ChunkNeighbourhood generatedNeighbourhood(const ChunkPos& centre, LodLevel level)
{
    return generatedScene(centre, level).neighbourhood;
}

ChunkNeighbourhood isolated(const ChunkPtr& centre)
{
    ChunkNeighbourhood neighbourhood(centre->position());
    neighbourhood.setChunk(0, 0, 0, centre);
    return neighbourhood;
}

ChunkNeighbourhood surrounded(const ChunkPtr& centre, BlockId material)
{
    ChunkNeighbourhood neighbourhood(centre->position());
    const ChunkPos&    base = centre->position();
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t sectionY = base.y + dy;
        if (sectionY < 0 || sectionY >= kWorldSectionCount) {
            continue;
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                neighbourhood.setChunk(
                    dx, dy, dz,
                    uniformChunk(ChunkPos{base.x + dx, sectionY, base.z + dz}, material));
            }
        }
    }
    neighbourhood.setChunk(0, 0, 0, centre);
    return neighbourhood;
}

// ------------------------------------------------------------ scene metrics --

std::size_t countNonAir(const Chunk& chunk)
{
    std::size_t total = 0;
    for (std::size_t index = 0; index < kChunkVolume; ++index) {
        total += chunk.getBlock(index) != blocks::Air ? 1u : 0u;
    }
    return total;
}

std::size_t countExposedFaces(const Chunk& chunk)
{
    std::size_t faces = 0;
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                if (chunk.getBlock(x, y, z) == blocks::Air) {
                    continue;
                }
                for (std::size_t direction = 0; direction < kDirectionCount; ++direction) {
                    const glm::ivec3&  offset = kDirectionOffsets[direction];
                    const std::int32_t nx     = x + offset.x;
                    const std::int32_t ny     = y + offset.y;
                    const std::int32_t nz     = z + offset.z;
                    // Outside the chunk counts as air: this is a description of
                    // the chunk in isolation, matching how the synthetic scenes
                    // are meshed.
                    const bool exposed = !isLocalPos(nx, ny, nz) ||
                                         chunk.getBlock(nx, ny, nz) == blocks::Air;
                    faces += exposed ? 1u : 0u;
                }
            }
        }
    }
    return faces;
}

CaveSite findDensestCaveSection(std::int32_t searchRadiusChunks, std::int32_t sectionY)
{
    CaveSite best;
    for (std::int32_t cz = -searchRadiusChunks; cz <= searchRadiusChunks; ++cz) {
        for (std::int32_t cx = -searchRadiusChunks; cx <= searchRadiusChunks; ++cx) {
            const ChunkPos    position{cx, sectionY, cz};
            const ChunkPtr    chunk = generateChunk(position, kLodFull);
            const std::size_t faces = countExposedFaces(*chunk);
            if (!best.found || faces > best.exposedFaces) {
                best.found        = true;
                best.position     = position;
                best.exposedFaces = faces;
                best.solidBlocks  = countNonAir(*chunk);
            }
        }
    }
    return best;
}

}  // namespace voxl::bench
