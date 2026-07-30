#include "world/LightEngine.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <utility>

namespace voxl::detail {

/// Flat, padded working set for one 32^3 section.
///
/// One CELL of padding on every side. That is all the flood needs: light only
/// ever moves along an axis, so a diagonal skirt cell can never inject anything
/// into the interior in one step, and the six face slabs carry the whole of what
/// the neighbours contribute. The shell is loaded once from the neighbour chunks
/// and then acts purely as a source - the flood never writes it, which is what
/// lets `collectSpill` compare the finished interior against the neighbours'
/// ORIGINAL values and emit only the cells that genuinely brightened.
struct LightScratch {
    static constexpr std::int32_t kDim    = kChunkSize + 2;  // 34
    static constexpr std::size_t  kVolume = static_cast<std::size_t>(kDim) * kDim * kDim;

    static constexpr std::ptrdiff_t kStrideX = 1;
    static constexpr std::ptrdiff_t kStrideZ = kDim;
    static constexpr std::ptrdiff_t kStrideY = static_cast<std::ptrdiff_t>(kDim) * kDim;

    /// Mirrors localIndex()'s ordering (x fastest, then z, then y) so an interior
    /// row of 32 voxels is contiguous in both the chunk and here.
    [[nodiscard]] static constexpr std::size_t index(std::int32_t x, std::int32_t y,
                                                     std::int32_t z) noexcept
    {
        return static_cast<std::size_t>((x + 1) * kStrideX + (z + 1) * kStrideZ +
                                        (y + 1) * kStrideY);
    }

    /// Bit 0: light may not enter this cell. Bit 1: the flood may write it.
    static constexpr std::uint8_t kBlocked  = 1u << 0;
    static constexpr std::uint8_t kInterior = 1u << 1;

    std::array<std::uint8_t, kVolume> flags{};
    std::array<std::uint8_t, kVolume> attenuation{};
    std::array<std::uint8_t, kVolume> emission{};
    std::array<std::uint8_t, kVolume> sun{};
    std::array<std::uint8_t, kVolume> block{};

    /// Index-based FIFO, not std::queue<BlockPos>. Reserved once to twice the
    /// volume so a cell being raised more than once still cannot reallocate.
    std::vector<std::uint16_t> queue;

    /// True when at least one interior cell emits light, so the block-light pass
    /// can be skipped outright for the overwhelming majority of chunks.
    bool anyEmission = false;
    /// True once any interior cell holds block light, whether from its own
    /// emission or from a neighbour's spill. Guards a full-volume rescan.
    bool anyBlockLight = false;
};

}  // namespace voxl::detail

namespace voxl {
namespace {

using detail::LightScratch;

/// Neighbour offsets in scratch index space, indexed by Direction.
constexpr std::ptrdiff_t kScratchStep[kDirectionCount] = {
    -LightScratch::kStrideX, +LightScratch::kStrideX, -LightScratch::kStrideY,
    +LightScratch::kStrideY, -LightScratch::kStrideZ, +LightScratch::kStrideZ,
};

/// Direction::NegY is the only one the sunlight free-fall rule applies to.
constexpr std::size_t kDownIndex = static_cast<std::size_t>(Direction::NegY);

/// Coordinates of one padded face slab cell. `a` and `b` sweep the two axes the
/// face spans; the third is pinned just outside the chunk.
void slabPosition(std::size_t direction, std::int32_t a, std::int32_t b, std::int32_t& x,
                  std::int32_t& y, std::int32_t& z) noexcept
{
    switch (static_cast<Direction>(direction)) {
        case Direction::NegX: x = -1;         y = a;          z = b;          break;
        case Direction::PosX: x = kChunkSize; y = a;          z = b;          break;
        case Direction::NegY: x = a;          y = -1;         z = b;          break;
        case Direction::PosY: x = a;          y = kChunkSize; z = b;          break;
        case Direction::NegZ: x = a;          y = b;          z = -1;         break;
        case Direction::PosZ: x = a;          y = b;          z = kChunkSize; break;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  LightWorld
// ---------------------------------------------------------------------------

LightWorld::LightWorld(LookupFn lookup, WritableFn writable)
    : m_lookup(std::move(lookup)), m_writable(std::move(writable))
{
    VOXL_CHECK(static_cast<bool>(m_lookup), "LightWorld needs a chunk lookup");
    VOXL_CHECK(static_cast<bool>(m_writable), "LightWorld needs a writability predicate");
    m_touched.reserve(32);
}

void LightWorld::invalidate() const noexcept
{
    m_cacheValid  = false;
    m_cachedPos   = ChunkPos{0, -1, 0};
    m_cachedOwner.reset();
    m_cached = Resolved{};
}

const LightWorld::Resolved& LightWorld::resolve(const ChunkPos& position) const
{
    if (m_cacheValid && m_cachedPos == position) {
        return m_cached;
    }
    m_cachedOwner     = m_lookup(position);
    m_cached.chunk    = m_cachedOwner.get();
    m_cached.writable = m_cached.chunk != nullptr && m_writable(position);
    m_cachedPos       = position;
    // The memo is only ever readable inside a Pass. Outside one this assignment
    // is what makes the next query re-ask the world: residency and writability
    // both go stale the instant the frame that observed them ends, and a memo
    // that survives that boundary hands the flood a chunk a worker now owns.
    // The owner shared_ptr is still stored, because the raw pointer we just
    // returned has to stay valid for the caller that is about to dereference it.
    m_cacheValid = m_passDepth != 0;
    return m_cached;
}

BlockId LightWorld::blockAt(const BlockPos& pos) const
{
    if (pos.y > kWorldMaxY) {
        return kAboveWorldBlock;
    }
    if (pos.y < kWorldMinY) {
        return kBelowWorldBlock;
    }
    const Resolved& slot = resolve(toChunkPos(pos));
    if (slot.chunk == nullptr) {
        return kMissingChunkBlock;
    }
    return slot.chunk->getBlock(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                blockToLocalAxis(pos.z));
}

std::uint8_t LightWorld::lightAt(const BlockPos& pos) const
{
    if (pos.y > kWorldMaxY) {
        return kAboveWorldLight;
    }
    if (pos.y < kWorldMinY) {
        return kBelowWorldLight;
    }
    const Resolved& slot = resolve(toChunkPos(pos));
    if (slot.chunk == nullptr) {
        // DARK, not kMissingChunkLight. The meshing convention treats an absent
        // chunk as full sunlight so a seam looks bright rather than black for a
        // frame; here the same guess would tell the removal pass it has found a
        // boundary of still-valid light and stop clearing, leaving a permanent
        // bright patch. An unknown neighbour must never be a light source.
        return 0;
    }
    return slot.chunk->getLight(localIndex(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                           blockToLocalAxis(pos.z)));
}

bool LightWorld::wholeAt(const BlockPos& pos) const
{
    if (!isInsideWorld(pos)) {
        return true;
    }
    const Resolved& slot = resolve(toChunkPos(pos));
    if (slot.chunk == nullptr) {
        return true;
    }
    return slot.chunk->isBlockWhole(localIndex(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                               blockToLocalAxis(pos.z)));
}

bool LightWorld::writableAt(const BlockPos& pos) const
{
    if (!isInsideWorld(pos)) {
        return false;
    }
    return resolve(toChunkPos(pos)).writable;
}

bool LightWorld::setSunlightAt(const BlockPos& pos, std::uint8_t level)
{
    return setNibble(pos, level, true);
}

bool LightWorld::setBlockLightAt(const BlockPos& pos, std::uint8_t level)
{
    return setNibble(pos, level, false);
}

bool LightWorld::setNibble(const BlockPos& pos, std::uint8_t level, bool sun)
{
    if (!isInsideWorld(pos)) {
        return false;
    }
    const ChunkPos  chunkPos = toChunkPos(pos);
    const Resolved& slot     = resolve(chunkPos);
    if (slot.chunk == nullptr) {
        return false;
    }
    if (!slot.writable) {
        ++m_refused;
        return false;
    }

    const std::int32_t lx    = blockToLocalAxis(pos.x);
    const std::int32_t ly    = blockToLocalAxis(pos.y);
    const std::int32_t lz    = blockToLocalAxis(pos.z);
    const std::size_t  index = localIndex(lx, ly, lz);

    const std::uint8_t packed = slot.chunk->getLight(index);
    const std::uint8_t next   = sun ? ChunkStorage::packLight(level,
                                                              ChunkStorage::unpackBlockLight(packed))
                                    : ChunkStorage::packLight(ChunkStorage::unpackSunlight(packed),
                                                              level);
    if (next == packed) {
        return true;
    }
    slot.chunk->setLight(index, next);
    noteTouched(chunkPos, lx, ly, lz);
    return true;
}

void LightWorld::noteTouched(const ChunkPos& position, std::int32_t lx, std::int32_t ly,
                             std::int32_t lz)
{
    std::uint8_t faces = 0;
    if (lx == 0) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegX));
    }
    if (lx == kChunkSize - 1) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosX));
    }
    if (ly == 0) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegY));
    }
    if (ly == kChunkSize - 1) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosY));
    }
    if (lz == 0) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegZ));
    }
    if (lz == kChunkSize - 1) {
        faces |= static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosZ));
    }

    // A flood walks one chunk at a time, so the entry it wants is nearly always
    // the last one appended; a linear scan over a handful of chunks beats a set.
    for (Touched& entry : m_touched) {
        if (entry.position == position) {
            entry.faceMask = static_cast<std::uint8_t>(entry.faceMask | faces);
            return;
        }
    }
    m_touched.push_back(Touched{position, faces});
}

// ---------------------------------------------------------------------------
//  LightEngine - construction
// ---------------------------------------------------------------------------

LightEngine::LightEngine(const BlockRegistry& registry)
    : m_scratch(std::make_unique<detail::LightScratch>())
{
    m_opaque.resize(registry.size());
    m_attenuation.resize(registry.size());
    m_emission.resize(registry.size());
    for (std::size_t id = 0; id < registry.size(); ++id) {
        const BlockType& type = registry.get(static_cast<BlockId>(id));
        m_opaque[id]          = type.opaque ? 1u : 0u;
        // An opaque block's attenuation is meaningless (nothing gets in), and
        // BlockType says so; clamping here keeps stepCost from ever producing a
        // cost that depends on a field the registry does not guarantee.
        m_attenuation[id] = type.opaque
                                ? std::uint8_t{0}
                                : static_cast<std::uint8_t>(std::min<std::uint8_t>(
                                      type.lightAttenuation, kFullLight));
        m_emission[id]    = static_cast<std::uint8_t>(std::min<std::uint8_t>(type.lightEmission,
                                                                             kFullLight));
    }

    m_scratch->queue.reserve(2 * LightScratch::kVolume);
    m_removeQueue.reserve(4096);
    m_lightQueue.reserve(4096);
}

LightEngine::~LightEngine()                                  = default;
LightEngine::LightEngine(LightEngine&&) noexcept             = default;
LightEngine& LightEngine::operator=(LightEngine&&) noexcept  = default;

void LightEngine::cellMaterial(const Chunk* chunk, std::size_t blockIndex,
                               std::uint8_t& outAttenuation, std::uint8_t& outOpaque,
                               std::uint8_t& outEmission) const noexcept
{
    if (chunk == nullptr) {
        // A neighbour we cannot see is a neutral wall: it neither gives light
        // nor takes any. Guessing "air" would let our own light leak into a cell
        // whose real contents may be solid rock.
        outAttenuation = 0;
        outOpaque      = 1;
        outEmission    = 0;
        return;
    }
    const std::size_t id = index(chunk->getBlock(blockIndex));
    outEmission          = m_emission[id];
    if (!chunk->isBlockWhole(blockIndex)) {
        // See the sub-voxel note at the top of LightEngine.hpp: a damaged block
        // transmits light freely, exactly as the mesher draws it.
        outAttenuation = 0;
        outOpaque      = 0;
        return;
    }
    outAttenuation = m_attenuation[id];
    outOpaque      = m_opaque[id];
}

// ---------------------------------------------------------------------------
//  LightEngine - worker side
// ---------------------------------------------------------------------------

void LightEngine::loadSection(const Chunk& target, const LightColumnWork* work,
                              const ChunkNeighbourhood* around, std::int32_t sectionY)
{
    LightScratch& scratch = *m_scratch;

    // The whole shell starts blocked. Only the six face slabs are then filled
    // in, so the twelve edges and eight corners stay walls and no light can take
    // a diagonal shortcut through a chunk that was never loaded.
    //
    // Only `flags` and the interior light are actually reset. `attenuation` and
    // `emission` are rewritten wholesale for the interior below and are never
    // read for a blocked cell, and a slab cell that is left blocked is never
    // read at all - so clearing all five arrays would be 200 KB of memset per
    // section, half of it for bytes nothing can observe.
    scratch.flags.fill(LightScratch::kBlocked);
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            const auto at = static_cast<std::ptrdiff_t>(LightScratch::index(0, y, z));
            std::fill_n(scratch.sun.begin() + at, kChunkSize, std::uint8_t{0});
            std::fill_n(scratch.block.begin() + at, kChunkSize, std::uint8_t{0});
        }
    }
    scratch.anyEmission   = false;
    scratch.anyBlockLight = false;

    // ---- interior ----
    const ChunkStorage& storage  = target.storage();
    const bool          damaged  = target.hasSubVoxelDamage();

    if (storage.isUniform() && !damaged) {
        const std::size_t  id      = index(storage.uniformValue());
        const std::uint8_t opaque  = m_opaque[id];
        const std::uint8_t attenuation = m_attenuation[id];
        const std::uint8_t emission    = m_emission[id];
        const std::uint8_t flags =
            static_cast<std::uint8_t>(LightScratch::kInterior | (opaque ? LightScratch::kBlocked : 0u));
        scratch.anyEmission = emission != 0;
        for (std::int32_t y = 0; y < kChunkSize; ++y) {
            for (std::int32_t z = 0; z < kChunkSize; ++z) {
                const auto at = static_cast<std::ptrdiff_t>(LightScratch::index(0, y, z));
                std::fill_n(scratch.flags.begin() + at, kChunkSize, flags);
                std::fill_n(scratch.attenuation.begin() + at, kChunkSize, attenuation);
                std::fill_n(scratch.emission.begin() + at, kChunkSize, emission);
            }
        }
    } else {
        for (std::int32_t y = 0; y < kChunkSize; ++y) {
            for (std::int32_t z = 0; z < kChunkSize; ++z) {
                const std::size_t source      = localIndex(0, y, z);
                const std::size_t destination = LightScratch::index(0, y, z);
                for (std::int32_t x = 0; x < kChunkSize; ++x) {
                    const std::size_t src = source + static_cast<std::size_t>(x);
                    const std::size_t dst = destination + static_cast<std::size_t>(x);

                    const std::size_t id       = index(storage.get(src));
                    std::uint8_t      opaque   = m_opaque[id];
                    std::uint8_t      attenuation = m_attenuation[id];
                    const std::uint8_t emission = m_emission[id];
                    if (damaged && target.subVoxels().isPartial(src)) {
                        opaque      = 0;
                        attenuation = 0;
                    }

                    scratch.flags[dst] = static_cast<std::uint8_t>(
                        LightScratch::kInterior | (opaque ? LightScratch::kBlocked : 0u));
                    scratch.attenuation[dst] = attenuation;
                    scratch.emission[dst]    = emission;
                    scratch.anyEmission      = scratch.anyEmission || emission != 0;
                }
            }
        }
    }

    // ---- the six face slabs ----
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const glm::ivec3& offset = kDirectionOffsets[d];
        const bool aboveWorld = sectionY + offset.y >= kWorldSectionCount;
        const bool belowWorld = sectionY + offset.y < 0;

        const Chunk* neighbourChunk = nullptr;
        if (!aboveWorld && !belowWorld) {
            neighbourChunk = work != nullptr ? work->at(offset.x, offset.z, sectionY + offset.y)
                                             : around->chunkAt(offset.x, offset.y, offset.z);
        }

        const bool openSky = aboveWorld;
        if (belowWorld || (!openSky && neighbourChunk == nullptr)) {
            // Bedrock below the world, or a neighbour we cannot see: the slab
            // stays a wall. Its light still has to be cleared, because the
            // seeding pass reads a slab's stored value whether or not the cell
            // is opaque - an opaque neighbour may be a glowstone - and only the
            // interior light is reset per section.
            for (std::int32_t a = 0; a < kChunkSize; ++a) {
                for (std::int32_t b = 0; b < kChunkSize; ++b) {
                    std::int32_t x = 0;
                    std::int32_t y = 0;
                    std::int32_t z = 0;
                    slabPosition(d, a, b, x, y, z);
                    const std::size_t dst = LightScratch::index(x, y, z);
                    scratch.sun[dst]      = 0;
                    scratch.block[dst]    = 0;
                }
            }
            continue;
        }

        const bool neighbourDamaged = neighbourChunk != nullptr &&
                                      neighbourChunk->hasSubVoxelDamage();

        for (std::int32_t a = 0; a < kChunkSize; ++a) {
            for (std::int32_t b = 0; b < kChunkSize; ++b) {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                slabPosition(d, a, b, x, y, z);
                const std::size_t dst = LightScratch::index(x, y, z);

                if (openSky) {
                    // The sky itself: clear air at full sunlight, matching
                    // BlockAccess::kAboveWorldLight.
                    scratch.flags[dst]       = 0;
                    scratch.attenuation[dst] = 0;
                    scratch.sun[dst]         = kFullLight;
                    scratch.block[dst]       = 0;
                    continue;
                }

                const std::size_t neighbourIndex = localIndex(x & kChunkSizeMask,
                                                              y & kChunkSizeMask,
                                                              z & kChunkSizeMask);
                const std::size_t id     = index(neighbourChunk->getBlock(neighbourIndex));
                std::uint8_t      opaque = m_opaque[id];
                std::uint8_t      attenuation = m_attenuation[id];
                if (neighbourDamaged && neighbourChunk->subVoxels().isPartial(neighbourIndex)) {
                    opaque      = 0;
                    attenuation = 0;
                }

                const std::uint8_t packed = neighbourChunk->getLight(neighbourIndex);
                scratch.flags[dst]        = opaque ? LightScratch::kBlocked : std::uint8_t{0};
                scratch.attenuation[dst]  = attenuation;
                scratch.sun[dst]          = ChunkStorage::unpackSunlight(packed);
                scratch.block[dst]        = ChunkStorage::unpackBlockLight(packed);
            }
        }
    }
}

void LightEngine::seedSection(bool propagated)
{
    LightScratch& scratch = *m_scratch;
    scratch.queue.clear();

    // ---- emissive blocks own their own cell, opaque or not ----
    if (scratch.anyEmission) {
        for (std::int32_t y = 0; y < kChunkSize; ++y) {
            for (std::int32_t z = 0; z < kChunkSize; ++z) {
                const std::size_t row = LightScratch::index(0, y, z);
                for (std::int32_t x = 0; x < kChunkSize; ++x) {
                    const std::size_t at = row + static_cast<std::size_t>(x);
                    if (scratch.emission[at] != 0) {
                        scratch.block[at]     = scratch.emission[at];
                        scratch.anyBlockLight = true;
                        scratch.queue.push_back(static_cast<std::uint16_t>(at));
                    }
                }
            }
        }
    }
    if (!propagated) {
        return;  // the coarse path floods block light only, from the queue above
    }

    // ---- one step in from every face slab ----
    //
    // The slab cells are never enqueued themselves: they are read-only sources,
    // which is what keeps their loaded values available for collectSpill().
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const std::ptrdiff_t inward  = -kScratchStep[d];
        const bool           falling = static_cast<Direction>(d) == Direction::PosY;

        for (std::int32_t a = 0; a < kChunkSize; ++a) {
            for (std::int32_t b = 0; b < kChunkSize; ++b) {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                slabPosition(d, a, b, x, y, z);
                const std::size_t source = LightScratch::index(x, y, z);
                // Deliberately NO opacity test on the source. An opaque cell can
                // still be a light source - glowstone is a full solid block - and
                // skipping it here is what leaves a lamp one block inside a
                // neighbour's border dimmer on our side of the seam than it is on
                // theirs. Opaque cells carry no sunlight and no received block
                // light, so reading them costs nothing when they are ordinary rock.
                const auto target1 = static_cast<std::size_t>(
                    static_cast<std::ptrdiff_t>(source) + inward);
                if ((scratch.flags[target1] & LightScratch::kBlocked) != 0) {
                    continue;
                }
                const std::uint8_t attenuation = scratch.attenuation[target1];

                const std::uint8_t sunLevel = scratch.sun[source];
                const std::uint8_t sunCost  = stepCost(sunLevel, true, falling, attenuation);
                if (sunLevel > sunCost) {
                    const auto next = static_cast<std::uint8_t>(sunLevel - sunCost);
                    if (next > scratch.sun[target1]) {
                        scratch.sun[target1] = next;
                        scratch.queue.push_back(static_cast<std::uint16_t>(target1));
                    }
                }

                const std::uint8_t blockLevel = scratch.block[source];
                const std::uint8_t blockCost  = stepCost(blockLevel, false, falling, attenuation);
                if (blockLevel > blockCost) {
                    const auto next = static_cast<std::uint8_t>(blockLevel - blockCost);
                    if (next > scratch.block[target1]) {
                        scratch.block[target1] = next;
                        scratch.anyBlockLight  = true;
                        scratch.queue.push_back(static_cast<std::uint16_t>(target1));
                    }
                }
            }
        }
    }
}

void LightEngine::floodVolume(bool sun)
{
    LightScratch& scratch = *m_scratch;
    std::array<std::uint8_t, LightScratch::kVolume>& values = sun ? scratch.sun : scratch.block;

    // The queue was filled by seedSection with cells whose value is already set;
    // it holds indices only, and the level is re-read on pop so a cell raised
    // again after being queued expands at its final value rather than twice.
    for (std::size_t head = 0; head < scratch.queue.size(); ++head) {
        const std::size_t  at    = scratch.queue[head];
        const std::uint8_t level = values[at];
        if (level <= 1) {
            continue;  // even a free step down would deliver nothing above zero
        }

        for (std::size_t d = 0; d < kDirectionCount; ++d) {
            const auto next = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(at) +
                                                       kScratchStep[d]);
            const std::uint8_t flags = scratch.flags[next];
            // Skirt cells are deliberately excluded: they are sources, not
            // destinations. collectSpill() re-reads them to work out what has to
            // cross the border, and a flood that overwrote them would compare
            // the neighbour against our own answer and find nothing to send.
            if ((flags & LightScratch::kInterior) == 0 || (flags & LightScratch::kBlocked) != 0) {
                continue;
            }

            const std::uint8_t cost =
                stepCost(level, sun, d == kDownIndex, scratch.attenuation[next]);
            if (cost >= level) {
                continue;
            }
            const auto value = static_cast<std::uint8_t>(level - cost);
            if (value <= values[next]) {
                continue;
            }
            values[next] = value;
            scratch.queue.push_back(static_cast<std::uint16_t>(next));
        }
    }
    scratch.queue.clear();
}

void LightEngine::sweepSkyColumns()
{
    // The coarse-LOD sunlight pass: one walk down each column, no queue. See
    // kMaxPropagatedLod for why this is indistinguishable at the distances the
    // coarse levels are used at.
    LightScratch& scratch = *m_scratch;
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            const std::size_t sky = LightScratch::index(x, kChunkSize, z);
            // A blocked sky slab means the section above is unknown; its stored
            // bytes are stale and must not be read.
            std::uint8_t carry = (scratch.flags[sky] & LightScratch::kBlocked) != 0
                                     ? std::uint8_t{0}
                                     : scratch.sun[sky];
            for (std::int32_t y = kChunkSize - 1; y >= 0; --y) {
                const std::size_t at = LightScratch::index(x, y, z);
                if ((scratch.flags[at] & LightScratch::kBlocked) != 0) {
                    scratch.sun[at] = 0;
                    carry           = 0;
                    continue;
                }
                const std::uint8_t cost = stepCost(carry, true, true, scratch.attenuation[at]);
                carry = carry > cost ? static_cast<std::uint8_t>(carry - cost) : std::uint8_t{0};
                scratch.sun[at] = carry;
            }
        }
    }
}

void LightEngine::writeSection(Chunk& target) const
{
    const LightScratch& scratch = *m_scratch;

    // An all-air sky section and a buried stone section are both uniform, and
    // between them they are most of the world. Collapsing them costs one pass
    // and saves ChunkStorage a 32 KB allocation each.
    const std::uint8_t first = ChunkStorage::packLight(scratch.sun[LightScratch::index(0, 0, 0)],
                                                       scratch.block[LightScratch::index(0, 0, 0)]);
    bool uniform = true;
    for (std::int32_t y = 0; y < kChunkSize && uniform; ++y) {
        for (std::int32_t z = 0; z < kChunkSize && uniform; ++z) {
            const std::size_t row = LightScratch::index(0, y, z);
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                const std::size_t at = row + static_cast<std::size_t>(x);
                if (ChunkStorage::packLight(scratch.sun[at], scratch.block[at]) != first) {
                    uniform = false;
                    break;
                }
            }
        }
    }

    if (uniform) {
        target.fillLight(ChunkStorage::unpackSunlight(first), ChunkStorage::unpackBlockLight(first));
        target.markDirty();
        return;
    }

    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            const std::size_t row         = LightScratch::index(0, y, z);
            const std::size_t destination = localIndex(0, y, z);
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                const std::size_t at = row + static_cast<std::size_t>(x);
                target.setLight(destination + static_cast<std::size_t>(x),
                                ChunkStorage::packLight(scratch.sun[at], scratch.block[at]));
            }
        }
    }
    target.markDirty();
}

void LightEngine::collectSpill(const BlockPos& origin, const LightColumnWork* work,
                               const ChunkNeighbourhood* around, std::int32_t sectionY,
                               LightSpill& spill) const
{
    const LightScratch& scratch = *m_scratch;

    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const glm::ivec3& offset    = kDirectionOffsets[d];
        const std::int32_t sideY    = sectionY + offset.y;
        if (sideY < 0 || sideY >= kWorldSectionCount) {
            continue;  // the sky and the void both take no light from us
        }
        const Chunk* side = work != nullptr ? work->at(offset.x, offset.z, sideY)
                                            : around->chunkAt(offset.x, offset.y, offset.z);
        if (side == nullptr) {
            continue;
        }
        // A section this job has not lit yet reads our light itself when its turn
        // comes; spilling into it now would hand the main thread a thousand seeds
        // per boundary that the very next pass would recompute anyway.
        if (work != nullptr && work->targets[static_cast<std::size_t>(sideY)] != nullptr &&
            work->targets[static_cast<std::size_t>(sideY)].get() == side &&
            sideY < sectionY) {
            continue;
        }

        const std::ptrdiff_t outward = kScratchStep[d];
        const bool           falling = static_cast<Direction>(d) == Direction::NegY;

        for (std::int32_t a = 0; a < kChunkSize; ++a) {
            for (std::int32_t b = 0; b < kChunkSize; ++b) {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                slabPosition(d, a, b, x, y, z);
                const std::size_t slab = LightScratch::index(x, y, z);
                if ((scratch.flags[slab] & LightScratch::kBlocked) != 0) {
                    continue;  // light may not enter the neighbour's cell at all
                }
                // The cell we are sending FROM may be opaque: an emissive block
                // holds its own light and has to shine out of the chunk exactly
                // as it shines within it.
                const auto inside = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(slab) -
                                                             outward);

                const std::uint8_t attenuation = scratch.attenuation[slab];

                const std::uint8_t sunLevel = scratch.sun[inside];
                const std::uint8_t sunCost  = stepCost(sunLevel, true, falling, attenuation);
                const std::uint8_t sunOut =
                    sunLevel > sunCost ? static_cast<std::uint8_t>(sunLevel - sunCost)
                                       : std::uint8_t{0};

                const std::uint8_t blockLevel = scratch.block[inside];
                const std::uint8_t blockCost  = stepCost(blockLevel, false, falling, attenuation);
                const std::uint8_t blockOut =
                    blockLevel > blockCost ? static_cast<std::uint8_t>(blockLevel - blockCost)
                                           : std::uint8_t{0};

                const bool raisesSun   = sunOut > scratch.sun[slab];
                const bool raisesBlock = blockOut > scratch.block[slab];
                if (!raisesSun && !raisesBlock) {
                    continue;
                }
                spill.push_back(LightSeed{BlockPos{origin.x + x, origin.y + y, origin.z + z},
                                          raisesSun ? sunOut : std::uint8_t{0},
                                          raisesBlock ? blockOut : std::uint8_t{0}});
            }
        }
    }
}

bool LightEngine::trySkyFastPath(Chunk& target, const LightColumnWork* work,
                                 const ChunkNeighbourhood* around, std::int32_t sectionY,
                                 LightSpill* spill)
{
    LightScratch&     scratch = *m_scratch;
    const std::size_t id      = index(target.storage().uniformValue());
    if (m_opaque[id] != 0 || m_attenuation[id] != 0 || m_emission[id] != 0) {
        return false;
    }

    // Sky slab must be complete daylight, and no face may carry block light: a
    // lamp next door would light part of this section and the flat answer would
    // be wrong. Sunlight from a side is capped at 14 by the one-per-block cost,
    // so it can never beat the 15 the sky already gives every cell.
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const bool sky = static_cast<Direction>(d) == Direction::PosY;
        for (std::int32_t a = 0; a < kChunkSize; ++a) {
            for (std::int32_t b = 0; b < kChunkSize; ++b) {
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t z = 0;
                slabPosition(d, a, b, x, y, z);
                const std::size_t at = LightScratch::index(x, y, z);
                if (scratch.block[at] > 1) {
                    return false;
                }
                if (sky && ((scratch.flags[at] & LightScratch::kBlocked) != 0 ||
                            scratch.sun[at] != kFullLight)) {
                    return false;
                }
            }
        }
    }

    target.fillLight(kFullLight, 0);
    target.markDirty();

    if (spill != nullptr) {
        // collectSpill reads the interior out of the scratch, so it has to be
        // filled in even though the chunk itself was written with one call.
        for (std::int32_t y = 0; y < kChunkSize; ++y) {
            for (std::int32_t z = 0; z < kChunkSize; ++z) {
                const auto at = static_cast<std::ptrdiff_t>(LightScratch::index(0, y, z));
                std::fill_n(scratch.sun.begin() + at, kChunkSize, kFullLight);
            }
        }
        collectSpill(target.originBlock(), work, around, sectionY, *spill);
    }
    return true;
}

void LightEngine::lightSection(Chunk& target, const LightColumnWork* work,
                               const ChunkNeighbourhood* around, LightSpill* spill)
{
    const std::int32_t sectionY = target.position().y;
    const LodLevel     level    = target.lod();
    const bool         propagated = level <= kMaxPropagatedLod;

    // MOST OF A STREAMED WORLD IS ONE OF TWO TRIVIAL SHAPES, and a flood over
    // 32768 cells to discover that is the single biggest cost in this file.
    //
    // A section of nothing but opaque, non-emissive blocks is black, full stop:
    // light cannot enter an opaque cell, so no neighbour and no sky can change
    // anything and no light can leave. That is every section below the caves.
    const ChunkStorage& storage        = target.storage();
    const bool          uniformSection = storage.isUniform() && !target.hasSubVoxelDamage();
    if (uniformSection) {
        const std::size_t id = index(storage.uniformValue());
        if (m_opaque[id] != 0 && m_emission[id] == 0) {
            target.fillLight(0, 0);
            target.markDirty();
            return;
        }
    }

    loadSection(target, work, around, sectionY);

    // The other shape: nothing but clear, non-emissive blocks under an unbroken
    // sky. Every cell is 15, which is the ceiling - no neighbour can raise it and
    // nothing inside can lower it - so the answer is known without a single queue
    // operation. That is every section above the terrain, which at a sea level of
    // 96 in a 256-block world is most of the column.
    if (uniformSection && trySkyFastPath(target, work, around, sectionY, spill)) {
        return;
    }

    seedSection(propagated);

    LightScratch& scratch = *m_scratch;
    if (propagated) {
        // Sunlight first. The two channels share one queue and one cost rule
        // that branches on which channel it is serving, so running them together
        // would expand a block-light value under the sunlight free-fall rule.
        floodVolume(true);

        // seedSection queued cells for both channels at once and floodVolume
        // consumed the queue, so the block pass needs its frontier rebuilt. Only
        // worth a full-volume scan when something actually carries block light,
        // which for terrain without glowstone is never.
        scratch.queue.clear();
        if (scratch.anyBlockLight) {
            for (std::int32_t y = 0; y < kChunkSize; ++y) {
                for (std::int32_t z = 0; z < kChunkSize; ++z) {
                    const std::size_t row = LightScratch::index(0, y, z);
                    for (std::int32_t x = 0; x < kChunkSize; ++x) {
                        const std::size_t at = row + static_cast<std::size_t>(x);
                        if (scratch.block[at] != 0) {
                            scratch.queue.push_back(static_cast<std::uint16_t>(at));
                        }
                    }
                }
            }
            floodVolume(false);
        }
    } else {
        sweepSkyColumns();
        if (scratch.anyBlockLight) {
            floodVolume(false);
        } else {
            scratch.queue.clear();
        }
    }

    writeSection(target);

    if (spill != nullptr && propagated) {
        collectSpill(target.originBlock(), work, around, sectionY, *spill);
    }
}

LightSpill LightEngine::lightColumn(const LightColumnWork& work)
{
    LightSpill spill;

    // TOP DOWN. Sunlight entering section y is what left the bottom of y + 1, so
    // going the other way would light every section from a sky value that is not
    // known yet and need a second pass over the whole column.
    for (std::int32_t y = kWorldSectionCount - 1; y >= 0; --y) {
        const ChunkPtr& target = work.targets[static_cast<std::size_t>(y)];
        if (target == nullptr) {
            continue;
        }
        VOXL_ASSERT(target->position().x == work.column.x && target->position().z == work.column.z &&
                        target->position().y == y,
                    "light column target is not the section it is filed under");
        lightSection(*target, &work, nullptr, &spill);
    }
    return spill;
}

void LightEngine::lightChunk(Chunk& chunk, const ChunkNeighbourhood& around, LightSpill* spill)
{
    VOXL_ASSERT(around.centrePos() == chunk.position(),
                "lightChunk needs a neighbourhood centred on the chunk being lit");
    lightSection(chunk, nullptr, &around, spill);
}

// ---------------------------------------------------------------------------
//  LightEngine - main thread
// ---------------------------------------------------------------------------

LightUpdateStats LightEngine::propagate(LightWorld& world, bool sun)
{
    LightUpdateStats stats;
    const std::size_t before = world.refusedWrites();

    for (std::size_t head = 0; head < m_lightQueue.size(); ++head) {
        if (head >= kMaxIncrementalCells) {
            stats.exhausted = true;
            break;
        }
        const BlockPos     origin = m_lightQueue[head].position;
        const std::uint8_t level  = sun ? world.sunlightAt(origin) : world.blockLightAt(origin);
        if (level <= 1) {
            continue;
        }
        ++stats.cellsPropagated;

        for (std::size_t d = 0; d < kDirectionCount; ++d) {
            const BlockPos next = neighbour(origin, static_cast<Direction>(d));
            if (!isInsideWorld(next)) {
                continue;
            }

            const std::size_t id = index(world.blockAt(next));
            std::uint8_t opaque      = m_opaque[id];
            std::uint8_t attenuation = m_attenuation[id];
            if (!world.wholeAt(next)) {
                opaque      = 0;
                attenuation = 0;
            }
            if (opaque != 0) {
                continue;
            }

            const std::uint8_t cost = stepCost(level, sun, d == kDownIndex, attenuation);
            if (cost >= level) {
                continue;
            }
            const auto value = static_cast<std::uint8_t>(level - cost);
            const std::uint8_t current = sun ? world.sunlightAt(next) : world.blockLightAt(next);
            if (value <= current) {
                continue;
            }
            const bool written = sun ? world.setSunlightAt(next, value)
                                     : world.setBlockLightAt(next, value);
            if (written) {
                m_lightQueue.push_back(Front{next, value});
            }
        }
    }

    m_lightQueue.clear();
    stats.writesRefused = world.refusedWrites() - before;
    return stats;
}

LightUpdateStats LightEngine::removeAndRefill(LightWorld& world, const BlockPos& origin, bool sun)
{
    LightUpdateStats  stats;
    const std::size_t before = world.refusedWrites();

    m_removeQueue.clear();
    m_lightQueue.clear();

    // WHY A REMOVAL PASS AND NOT SIMPLY A RE-FLOOD.
    //
    // Propagation only ever raises a cell: it writes when the value it carries
    // beats what is already stored. When a light source disappears, or a block
    // is placed that stops light, the stale values left behind are exactly as
    // large as - usually larger than - anything a re-flood would try to write,
    // so every write is rejected and the darkness never arrives. The region
    // keeps glowing with light that has no source, and because nothing dirties
    // it again it keeps glowing forever.
    //
    // The fix is the standard two-queue algorithm. First walk outward clearing
    // every cell whose value is exactly what this cell would have given it - that
    // is the signature of "I lit you" - and while doing so collect every cell
    // that is BRIGHTER than that, because those are lit by something else and
    // form the boundary of still-valid light. Then run the ordinary propagation
    // pass from that boundary, which refills whatever was cleared but should not
    // have been.
    const std::uint8_t stale = sun ? world.sunlightAt(origin) : world.blockLightAt(origin);
    if (stale != 0) {
        const bool cleared = sun ? world.setSunlightAt(origin, 0) : world.setBlockLightAt(origin, 0);
        if (cleared) {
            m_removeQueue.push_back(Front{origin, stale});
        }
    }

    for (std::size_t head = 0; head < m_removeQueue.size(); ++head) {
        if (head >= kMaxIncrementalCells) {
            stats.exhausted = true;
            break;
        }
        const Front front = m_removeQueue[head];
        ++stats.cellsCleared;

        for (std::size_t d = 0; d < kDirectionCount; ++d) {
            const BlockPos next = neighbour(front.position, static_cast<Direction>(d));
            if (!isInsideWorld(next)) {
                continue;
            }
            const std::uint8_t current = sun ? world.sunlightAt(next) : world.blockLightAt(next);
            if (current == 0) {
                continue;
            }

            const std::size_t id = index(world.blockAt(next));
            std::uint8_t attenuation = m_attenuation[id];
            if (!world.wholeAt(next)) {
                attenuation = 0;
            }
            const std::uint8_t cost =
                stepCost(front.level, sun, d == kDownIndex, attenuation);
            const std::uint8_t expected =
                front.level > cost ? static_cast<std::uint8_t>(front.level - cost) : std::uint8_t{0};

            if (expected != 0 && current == expected) {
                // Ours. Clear it and keep walking.
                const bool cleared = sun ? world.setSunlightAt(next, 0)
                                         : world.setBlockLightAt(next, 0);
                if (cleared) {
                    m_removeQueue.push_back(Front{next, current});
                }
                continue;
            }
            // Anything else is either brighter than we could have made it - a
            // real source - or dimmer than it should be, which means our picture
            // of it is stale. Both are handled by re-propagating from it.
            m_lightQueue.push_back(Front{next, current});
        }
    }

    m_removeQueue.clear();

    // Whatever the origin can legitimately hold now, plus its six neighbours, so
    // light flows back into a cell that has just become transparent.
    if (!sun) {
        const std::size_t  id       = index(world.blockAt(origin));
        const std::uint8_t emission = m_emission[id];
        if (emission != 0 && world.setBlockLightAt(origin, emission)) {
            m_lightQueue.push_back(Front{origin, emission});
        }
    }
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const BlockPos next = neighbour(origin, static_cast<Direction>(d));
        if (!isInsideWorld(next)) {
            continue;
        }
        const std::uint8_t current = sun ? world.sunlightAt(next) : world.blockLightAt(next);
        if (current != 0) {
            m_lightQueue.push_back(Front{next, current});
        }
    }

    const LightUpdateStats refill = propagate(world, sun);
    stats.cellsPropagated += refill.cellsPropagated;
    stats.exhausted = stats.exhausted || refill.exhausted;
    stats.writesRefused = world.refusedWrites() - before;
    return stats;
}

LightUpdateStats LightEngine::voxelChanged(LightWorld& world, const BlockPos& pos)
{
    LightUpdateStats stats;
    if (!isInsideWorld(pos)) {
        return stats;
    }

    // Opened here rather than left to the caller: this is one of the two places
    // a main-thread flood starts, so binding the memo's lifetime to it is what
    // makes "the reader never trusts a stale chunk" true by construction instead
    // of by every call site remembering. See LightWorld.
    const LightWorld::Pass pass{world};

    // Block light first. Its removal pass has to run before the cell is given
    // its new emission, or the new value would look like still-valid light and
    // stop the walk at the source.
    stats += removeAndRefill(world, pos, false);
    stats += removeAndRefill(world, pos, true);

    if (stats.exhausted) {
        VOXL_LOG_WARN("light update at ({}, {}, {}) hit the {}-cell bound; the region may stay "
                      "slightly brighter than it should",
                      pos.x, pos.y, pos.z, kMaxIncrementalCells);
    }
    return stats;
}

LightUpdateStats LightEngine::applySeeds(LightWorld& world, const LightSeed* seeds,
                                         std::size_t count)
{
    LightUpdateStats stats;
    if (seeds == nullptr || count == 0) {
        return stats;
    }

    const LightWorld::Pass pass{world};

    // SNAPSHOTTED HERE, NOT INSIDE propagate(). The seeding loop below writes,
    // and every one of those writes can be refused. propagate() takes its own
    // baseline at its own entry, which is already past the seeding loop, so the
    // refusals the seeds incurred were invisible to the returned stats. A batch
    // in which EVERY seed was refused - the common case, because a seed lands in
    // the neighbour column the worker that produced it did not own, and that is
    // exactly the column most likely to be busy - reported zero refusals, and
    // World::applyPendingLight read zero as "the batch landed" and threw it away.
    // The light never arrived and nothing was left to retry it. Same delta over
    // the whole function that removeAndRefill already takes.
    const std::size_t before = world.refusedWrites();

    for (int channel = 0; channel < 2; ++channel) {
        const bool sun = channel == 0;
        m_lightQueue.clear();
        for (std::size_t i = 0; i < count; ++i) {
            const LightSeed&   seed  = seeds[i];
            const std::uint8_t level = sun ? seed.sunlight : seed.blockLight;
            if (level == 0) {
                continue;
            }
            const std::uint8_t current =
                sun ? world.sunlightAt(seed.position) : world.blockLightAt(seed.position);
            if (level <= current) {
                continue;  // a stale seed, or the neighbour got there first
            }
            const bool written = sun ? world.setSunlightAt(seed.position, level)
                                     : world.setBlockLightAt(seed.position, level);
            if (written) {
                m_lightQueue.push_back(Front{seed.position, level});
            }
        }
        stats += propagate(world, sun);
    }

    // Assigned, not accumulated: propagate() already added its own delta, and
    // this delta spans it. Both channels, because a seed refused on either one
    // means the batch has to be replayed.
    stats.writesRefused = world.refusedWrites() - before;
    return stats;
}

}  // namespace voxl
