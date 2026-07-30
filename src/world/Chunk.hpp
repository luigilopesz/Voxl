#pragma once

// One 32^3 world section: voxel storage, light, and the streaming state machine.
//
// THIS HEADER IS A CONTRACT. Streaming, generation, meshing, lighting, physics
// and persistence all agree on the lifecycle and the threading rules below.

#include "core/Log.hpp"
#include "world/Block.hpp"
#include "world/ChunkStorage.hpp"
#include "world/VoxelTypes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace voxl {

/// Streaming lifecycle of a chunk.
///
/// LEGAL TRANSITIONS (anything else is a bug and trips an assert):
///
///     Empty      -> Generating   main thread, when a generation job is queued
///     Generating -> Generated    worker,      terrain written, light seeded
///     Generated  -> Meshing      main thread, when a mesh job is queued
///     Meshing    -> Meshed       worker,      ChunkMeshData built on the CPU
///     Meshed     -> Ready        MAIN THREAD, after the GPU upload lands
///     Ready      -> Meshing      main thread, remesh after an edit
///     Generated  -> Unloading    \
///     Ready      -> Unloading     |  main thread, chunk left the view distance
///     Empty      -> Unloading     |
///     Meshed     -> Unloading    /
///
/// `Generating` and `Meshing` are the two states in which a worker owns the
/// chunk. The main thread must not mutate a chunk in those states, and the
/// transition *out* of them is performed by the worker that owns it.
///
/// Unloading is deliberately NOT reachable from Generating or Meshing: a worker
/// holds a shared_ptr and is mid-flight, so the unload path waits for the job
/// to publish its result and only then retires the chunk. Trying to unload a
/// busy chunk is what produces use-after-free in engines like this.
enum class ChunkState : std::uint8_t {
    /// Allocated, no voxels yet. All-air, safe to read.
    Empty = 0,
    /// A worker is writing terrain. NOT safe to read.
    Generating = 1,
    /// Voxels are final; no mesh yet.
    Generated = 2,
    /// A worker is reading a snapshot to build geometry. Safe to read, must not
    /// be written.
    Meshing = 3,
    /// CPU-side mesh exists and is waiting on the main thread for upload.
    Meshed = 4,
    /// Voxels and GPU buffers are both live. The steady state.
    Ready = 5,
    /// Scheduled for retirement; no new work may be queued against it.
    Unloading = 6,
};

inline constexpr std::size_t kChunkStateCount = 7;

[[nodiscard]] constexpr const char* toString(ChunkState state) noexcept
{
    switch (state) {
        case ChunkState::Empty:      return "Empty";
        case ChunkState::Generating: return "Generating";
        case ChunkState::Generated:  return "Generated";
        case ChunkState::Meshing:    return "Meshing";
        case ChunkState::Meshed:     return "Meshed";
        case ChunkState::Ready:      return "Ready";
        case ChunkState::Unloading:  return "Unloading";
    }
    return "Unknown";
}

/// Single source of truth for the diagram above. Kept constexpr so tests can
/// exhaustively verify the table.
[[nodiscard]] constexpr bool isLegalChunkTransition(ChunkState from, ChunkState to) noexcept
{
    if (from == to) {
        return false;  // a re-entered state is always a duplicated transition
    }
    switch (from) {
        case ChunkState::Empty:
            return to == ChunkState::Generating || to == ChunkState::Unloading;
        case ChunkState::Generating:
            return to == ChunkState::Generated;
        case ChunkState::Generated:
            return to == ChunkState::Meshing || to == ChunkState::Unloading;
        case ChunkState::Meshing:
            return to == ChunkState::Meshed;
        case ChunkState::Meshed:
            return to == ChunkState::Ready || to == ChunkState::Unloading;
        case ChunkState::Ready:
            return to == ChunkState::Meshing || to == ChunkState::Unloading;
        case ChunkState::Unloading:
            return false;
    }
    return false;
}

/// True while a worker thread owns the chunk's contents.
[[nodiscard]] constexpr bool isChunkBusy(ChunkState state) noexcept
{
    return state == ChunkState::Generating || state == ChunkState::Meshing;
}

/// One vertical section of the world.
///
/// PINNED: non-copyable and non-movable. Two reasons, both load-bearing.
///  1. It holds std::atomic members, which are not movable in the first place.
///  2. Workers hold `std::shared_ptr<const Chunk>` snapshots of neighbours (see
///     src/world/BlockAccess.hpp) and compare chunk identity by address, so the
///     object must never relocate. Chunks are always heap-allocated behind a
///     shared_ptr; use `Chunk::create()`.
///
/// THREADING CONTRACT
/// ------------------
/// Main thread only:
///   create/destroy, setBlock, setLight (outside generation), markDirty,
///   clearRemeshFlag, markSaved, and every transition into Meshing/Unloading.
///
/// Worker threads may:
///   - write voxels/light while the state is Generating AND they are the worker
///     that performed Empty -> Generating (exclusive ownership by construction);
///   - read voxels/light while the state is Meshing or Ready, provided they hold
///     a shared_ptr keeping the chunk alive;
///   - call state(), tryTransition(), contentVersion(), position(), the memory
///     accessors and the dirty-flag readers at any time.
///
/// Never legal from a worker: setBlock on a Ready chunk, any GL call, and
/// transitioning a chunk it does not own.
class Chunk {
public:
    explicit Chunk(const ChunkPos& position) noexcept : m_position(position) {}

    Chunk(const Chunk&)            = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&)                 = delete;
    Chunk& operator=(Chunk&&)      = delete;

    [[nodiscard]] static std::shared_ptr<Chunk> create(const ChunkPos& position)
    {
        return std::make_shared<Chunk>(position);
    }

    [[nodiscard]] const ChunkPos& position() const noexcept { return m_position; }
    [[nodiscard]] BlockPos originBlock() const noexcept { return m_position.originBlock(); }

    // ------------------------------------------------------------ voxels --

    /// Local coordinates, each component in [0, kChunkSize).
    [[nodiscard]] BlockId getBlock(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        return m_storage.get(x, y, z);
    }
    [[nodiscard]] BlockId getBlock(std::size_t index) const noexcept { return m_storage.get(index); }
    [[nodiscard]] BlockId getBlock(const BlockPos& local) const noexcept
    {
        return m_storage.get(local.x, local.y, local.z);
    }

    /// Writes a voxel and bumps the content version. See the threading contract.
    void setBlock(std::int32_t x, std::int32_t y, std::int32_t z, BlockId id)
    {
        m_storage.set(x, y, z, id);
        touch();
    }
    void setBlock(std::size_t index, BlockId id)
    {
        m_storage.set(index, id);
        touch();
    }
    void setBlock(const BlockPos& local, BlockId id) { setBlock(local.x, local.y, local.z, id); }

    // ------------------------------------------------------------- light --

    /// Packed byte: high nibble sunlight, low nibble block light. Layout
    /// constants live on ChunkStorage.
    [[nodiscard]] std::uint8_t getLight(std::size_t index) const noexcept
    {
        return m_storage.light(index);
    }
    [[nodiscard]] std::uint8_t getLight(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        return m_storage.light(x, y, z);
    }
    [[nodiscard]] std::uint8_t getSunlight(std::size_t index) const noexcept
    {
        return m_storage.sunlight(index);
    }
    [[nodiscard]] std::uint8_t getBlockLight(std::size_t index) const noexcept
    {
        return m_storage.blockLight(index);
    }

    /// Light writes do NOT bump the content version - lighting runs after
    /// meshing is scheduled and re-dirties the chunk explicitly via markDirty().
    void setLight(std::size_t index, std::uint8_t packed) { m_storage.setLight(index, packed); }
    void setSunlight(std::size_t index, std::uint8_t level) { m_storage.setSunlight(index, level); }
    void setBlockLight(std::size_t index, std::uint8_t level) { m_storage.setBlockLight(index, level); }
    void fillLight(std::uint8_t sunlight, std::uint8_t blockLight)
    {
        m_storage.fillLight(sunlight, blockLight);
    }

    // ----------------------------------------------------------- storage --

    /// Direct storage access for the generator and the save/load path, which
    /// need the bulk operations (fill, optimise, raw palette). Subject to the
    /// same threading contract as setBlock.
    [[nodiscard]] ChunkStorage& storage() noexcept { return m_storage; }
    [[nodiscard]] const ChunkStorage& storage() const noexcept { return m_storage; }

    [[nodiscard]] bool isEmpty() const noexcept { return m_storage.isEmpty(); }

    // ------------------------------------------------------------- state --

    /// Acquire so that a thread observing Generated also sees every voxel the
    /// generating worker wrote before it released the state.
    [[nodiscard]] ChunkState state() const noexcept
    {
        return m_state.load(std::memory_order_acquire);
    }

    /// Compare-and-swap the state. Returns false when another thread got there
    /// first, which is the whole point: two schedulers both deciding to remesh
    /// the same chunk must result in exactly one mesh job.
    ///
    /// Release ordering on success publishes the writes made before the call.
    bool tryTransition(ChunkState expected, ChunkState next) noexcept
    {
        VOXL_ASSERT(isLegalChunkTransition(expected, next), "illegal chunk state transition");
        return m_state.compare_exchange_strong(expected, next, std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }

    /// Unconditional transition. Only for the unload path, which must succeed
    /// from whatever non-busy state the chunk happens to be in, and for tests.
    void forceState(ChunkState next) noexcept { m_state.store(next, std::memory_order_release); }

    [[nodiscard]] bool isBusy() const noexcept { return isChunkBusy(state()); }
    /// True once the chunk has voxels that are safe to read.
    [[nodiscard]] bool hasVoxels() const noexcept
    {
        const ChunkState current = state();
        return current == ChunkState::Generated || current == ChunkState::Meshing ||
               current == ChunkState::Meshed || current == ChunkState::Ready;
    }

    // ------------------------------------------------------- dirty flags --

    /// Set when the voxels or light changed since the last mesh was built.
    [[nodiscard]] bool needsRemesh() const noexcept
    {
        return m_needsRemesh.load(std::memory_order_acquire);
    }
    void markDirty() noexcept { m_needsRemesh.store(true, std::memory_order_release); }
    void clearRemeshFlag() noexcept { m_needsRemesh.store(false, std::memory_order_release); }

    /// Set when the chunk diverges from what is on disk. Cleared by the saver.
    [[nodiscard]] bool needsSave() const noexcept
    {
        return m_needsSave.load(std::memory_order_acquire);
    }
    void markModified() noexcept { m_needsSave.store(true, std::memory_order_release); }
    void markSaved() noexcept { m_needsSave.store(false, std::memory_order_release); }

    /// Monotonic counter bumped by every voxel write.
    ///
    /// A mesh job records the version it read; when its result comes back to the
    /// main thread the version is compared again and a stale mesh is discarded
    /// rather than uploaded. Without this, an edit made while a mesh job is in
    /// flight is silently reverted on screen until something else dirties the
    /// chunk.
    [[nodiscard]] std::uint32_t contentVersion() const noexcept
    {
        return m_contentVersion.load(std::memory_order_acquire);
    }
    /// Version the currently uploaded mesh was built from.
    [[nodiscard]] std::uint32_t meshedVersion() const noexcept
    {
        return m_meshedVersion.load(std::memory_order_acquire);
    }
    void setMeshedVersion(std::uint32_t version) noexcept
    {
        m_meshedVersion.store(version, std::memory_order_release);
    }

    /// Frame index of the last time streaming touched this chunk; the unload
    /// heuristic uses it to avoid thrashing a chunk on the distance boundary.
    [[nodiscard]] std::uint64_t lastTouchedFrame() const noexcept
    {
        return m_lastTouchedFrame.load(std::memory_order_relaxed);
    }
    void setLastTouchedFrame(std::uint64_t frame) noexcept
    {
        m_lastTouchedFrame.store(frame, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------ memory --

    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept
    {
        return sizeof(Chunk) + m_storage.heapBytes();
    }

private:
    /// Marks the chunk changed: new content version, needs remesh, needs save.
    void touch() noexcept
    {
        m_contentVersion.fetch_add(1, std::memory_order_release);
        m_needsRemesh.store(true, std::memory_order_release);
        m_needsSave.store(true, std::memory_order_release);
    }

    const ChunkPos m_position;
    ChunkStorage   m_storage;

    std::atomic<ChunkState>    m_state{ChunkState::Empty};
    std::atomic<bool>          m_needsRemesh{false};
    std::atomic<bool>          m_needsSave{false};
    std::atomic<std::uint32_t> m_contentVersion{0};
    std::atomic<std::uint32_t> m_meshedVersion{0};
    std::atomic<std::uint64_t> m_lastTouchedFrame{0};
};

using ChunkPtr      = std::shared_ptr<Chunk>;
using ConstChunkPtr = std::shared_ptr<const Chunk>;

}  // namespace voxl
