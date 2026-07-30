// Compile-time verification of the Chunk lifecycle contract.
//
// WHY THIS FILE HAS NO FUNCTION DEFINITIONS
// -----------------------------------------
// Chunk is defined entirely in-class in world/Chunk.hpp. Its members are either
// atomic loads/stores that must inline (the streaming scheduler queries state()
// and needsRemesh() for every resident chunk, every frame) or one-line forwards
// into ChunkStorage. There is no body left to move out of line.
//
// What lives here is the check that the transition diagram in the header comment
// still describes the code underneath it. isLegalChunkTransition() is a
// hand-written switch, and the only runtime guard on it is the VOXL_ASSERT in
// tryTransition(), which is compiled out of every non-Debug build. Below, the
// diagram is re-encoded as data - deliberately written out independently rather
// than derived from the switch - and the two are cross-checked exhaustively for
// all 49 state pairs. If someone edits one and not the other, the build breaks.

#include "world/Chunk.hpp"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace voxl {
namespace {

/// The transition diagram from the header comment, transcribed as an edge list.
/// This is a second, independent statement of the same rule: it exists to
/// disagree with isLegalChunkTransition() if either one is edited alone.
struct StateEdge {
    ChunkState from;
    ChunkState to;
};

constexpr StateEdge kDocumentedTransitions[] = {
    // The forward streaming pipeline.
    {ChunkState::Empty, ChunkState::Generating},
    {ChunkState::Generating, ChunkState::Generated},
    {ChunkState::Generated, ChunkState::Meshing},
    {ChunkState::Meshing, ChunkState::Meshed},
    {ChunkState::Meshed, ChunkState::Ready},
    // Remesh after an edit or a light update.
    {ChunkState::Ready, ChunkState::Meshing},
    // Retirement, legal only from the states in which no worker owns the chunk.
    {ChunkState::Empty, ChunkState::Unloading},
    {ChunkState::Generated, ChunkState::Unloading},
    {ChunkState::Meshed, ChunkState::Unloading},
    {ChunkState::Ready, ChunkState::Unloading},
};

[[nodiscard]] constexpr bool isDocumented(ChunkState from, ChunkState to) noexcept
{
    for (const StateEdge& edge : kDocumentedTransitions) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr ChunkState stateAt(std::size_t ordinal) noexcept
{
    return static_cast<ChunkState>(static_cast<std::uint8_t>(ordinal));
}

constexpr bool transitionTableMatchesTheDiagram() noexcept
{
    for (std::size_t from = 0; from < kChunkStateCount; ++from) {
        for (std::size_t to = 0; to < kChunkStateCount; ++to) {
            if (isLegalChunkTransition(stateAt(from), stateAt(to)) !=
                isDocumented(stateAt(from), stateAt(to))) {
                return false;
            }
        }
    }
    return true;
}
static_assert(transitionTableMatchesTheDiagram(),
              "isLegalChunkTransition() and the documented diagram disagree");

/// A state may never transition to itself: re-entering a state means some
/// scheduler queued the same work twice, and swallowing that silently is how a
/// chunk ends up with two generation jobs writing it at once.
constexpr bool noSelfTransitions() noexcept
{
    for (std::size_t i = 0; i < kChunkStateCount; ++i) {
        if (isLegalChunkTransition(stateAt(i), stateAt(i))) {
            return false;
        }
    }
    return true;
}
static_assert(noSelfTransitions(), "a state must not be a legal transition to itself");

/// THE load-bearing safety property. A busy chunk is owned by a worker that
/// holds a shared_ptr and is mid-flight; letting the unload path retire it is
/// precisely the use-after-free this state machine exists to prevent. The unload
/// path must instead wait for the worker to publish and leave the busy state.
constexpr bool busyChunksCannotBeUnloaded() noexcept
{
    for (std::size_t i = 0; i < kChunkStateCount; ++i) {
        const ChunkState state = stateAt(i);
        if (isChunkBusy(state) && isLegalChunkTransition(state, ChunkState::Unloading)) {
            return false;
        }
    }
    return true;
}
static_assert(busyChunksCannotBeUnloaded(),
              "Unloading must not be reachable from Generating or Meshing");

/// A busy state has exactly one exit, and it is not busy. That is what makes
/// "the worker that entered the state is the one that leaves it" a total
/// description of ownership rather than a convention.
constexpr bool busyStatesHaveExactlyOneNonBusyExit() noexcept
{
    for (std::size_t i = 0; i < kChunkStateCount; ++i) {
        const ChunkState state = stateAt(i);
        if (!isChunkBusy(state)) {
            continue;
        }
        std::size_t exits = 0;
        for (std::size_t j = 0; j < kChunkStateCount; ++j) {
            if (isLegalChunkTransition(state, stateAt(j))) {
                ++exits;
                if (isChunkBusy(stateAt(j))) {
                    return false;
                }
            }
        }
        if (exits != 1) {
            return false;
        }
    }
    return true;
}
static_assert(busyStatesHaveExactlyOneNonBusyExit(),
              "a busy state must have exactly one exit and it must not be busy");

/// Unloading is terminal: nothing may be queued against a retiring chunk.
constexpr bool unloadingIsTerminal() noexcept
{
    for (std::size_t i = 0; i < kChunkStateCount; ++i) {
        if (isLegalChunkTransition(ChunkState::Unloading, stateAt(i))) {
            return false;
        }
    }
    return true;
}
static_assert(unloadingIsTerminal(), "Unloading must be a terminal state");

/// Every state except the initial one must be reachable, otherwise a state in
/// the enum is dead and the pipeline has a gap.
constexpr bool everyStateIsReachable() noexcept
{
    for (std::size_t to = 1; to < kChunkStateCount; ++to) {
        bool reachable = false;
        for (std::size_t from = 0; from < kChunkStateCount; ++from) {
            if (isLegalChunkTransition(stateAt(from), stateAt(to))) {
                reachable = true;
            }
        }
        if (!reachable) {
            return false;
        }
    }
    return true;
}
static_assert(everyStateIsReachable(), "a ChunkState is unreachable and therefore dead");

static_assert(stateAt(kChunkStateCount - 1u) == ChunkState::Unloading,
              "kChunkStateCount does not match the ChunkState enumerators");
static_assert(static_cast<std::uint8_t>(ChunkState::Empty) == 0,
              "Empty must be the zero state so a default-constructed atomic is Empty");

// -------------------------------------------------------------- toString --

/// toString() falls through to "Unknown" when an enumerator is missing from its
/// switch. A missing case is otherwise invisible until it shows up in a log or
/// the debug overlay, so pin it down here.
constexpr bool everyStateHasAName() noexcept
{
    for (std::size_t i = 0; i < kChunkStateCount; ++i) {
        const std::string_view name = toString(stateAt(i));
        if (name.empty() || name == std::string_view{"Unknown"}) {
            return false;
        }
    }
    return true;
}
static_assert(everyStateHasAName(), "toString(ChunkState) is missing a case");

// ----------------------------------------------------- threading premises --

/// Chunk's threading contract assumes its atomics are genuine machine
/// instructions. A lock-based std::atomic would put a mutex acquire inside
/// state(), which the streaming scheduler calls for every resident chunk every
/// frame, and would make tryTransition() unsafe to call from a signal-like
/// context. On every platform this engine targets these are all lock-free; if
/// that ever stops being true, the design needs revisiting, not a silent
/// slowdown.
static_assert(std::atomic<ChunkState>::is_always_lock_free,
              "ChunkState must be lock-free for the streaming hot path");
static_assert(std::atomic<bool>::is_always_lock_free, "dirty flags must be lock-free");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "content/mesh versions must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "the frame counter must be lock-free");

/// Chunk must stay pinned. Workers hold shared_ptr snapshots of neighbouring
/// chunks and ChunkNeighbourhood compares chunk identity by address, so a
/// relocating Chunk would silently break neighbour lookups rather than fail to
/// compile at the point of the mistake.
static_assert(!std::is_copy_constructible_v<Chunk>, "Chunk must not be copyable");
static_assert(!std::is_copy_assignable_v<Chunk>, "Chunk must not be copy-assignable");
static_assert(!std::is_move_constructible_v<Chunk>, "Chunk must not be movable");
static_assert(!std::is_move_assignable_v<Chunk>, "Chunk must not be move-assignable");

}  // namespace

namespace detail {

/// Anchors this translation unit. See the note on
/// kChunkStorageContractVerified in world/ChunkStorage.cpp - Chunk is inline by
/// contract, so without an external symbol this object file contributes nothing
/// to the archive and the librarian says so (LNK4221).
extern const bool kChunkContractVerified;
const bool        kChunkContractVerified = true;

}  // namespace detail
}  // namespace voxl
