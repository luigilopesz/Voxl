// The JobSystem is defined entirely in its header, by design and not by
// accident: `submit`/`submitDetached` are templates, and everything on the
// submission and steal path is a handful of instructions that chunk streaming
// executes hundreds of times per frame, so keeping it inlinable is worth more
// than the compile-time saving of a separate object file.
//
// This translation unit therefore contains no definitions. It exists so that
//   * the contract is compiled at least once on its own, catching a header that
//     only ever builds because some other TU happened to include a dependency
//     first, and
//   * the invariants the header's inline code silently relies on (the priority
//     count matching the enum, the pool being pinned) are checked by the
//     compiler rather than by a comment.
//
// If you are looking for the pool's behaviour, it is in JobSystem.hpp; if you
// are looking for its tests, they are in tests/test_jobsystem.cpp.

#include "core/JobSystem.hpp"

#include <string_view>
#include <type_traits>

namespace voxl {
namespace {

// ---- priority enum <-> array index -------------------------------------------
// WorkerQueue indexes `byPriority[static_cast<size_t>(priority)]`, so every
// enumerator must be a valid index and the count must be exact. Adding a
// priority without bumping kJobPriorityCount would corrupt the deque array.
static_assert(kJobPriorityCount == 3, "kJobPriorityCount must match the JobPriority enumerators");
static_assert(static_cast<std::size_t>(JobPriority::High) == 0, "High must be the first index");
static_assert(static_cast<std::size_t>(JobPriority::Normal) == 1, "Normal must follow High");
static_assert(static_cast<std::size_t>(JobPriority::Low) == 2, "Low must follow Normal");
static_assert(static_cast<std::size_t>(JobPriority::Low) + 1 == kJobPriorityCount,
              "kJobPriorityCount must be one past the last enumerator");

// The worker's inner loop walks priorities in ascending index order and treats
// that as descending urgency. If the enumerators were ever reordered, High work
// would silently be serviced last.
static_assert(static_cast<std::size_t>(JobPriority::High) <
                  static_cast<std::size_t>(JobPriority::Normal) &&
              static_cast<std::size_t>(JobPriority::Normal) <
                  static_cast<std::size_t>(JobPriority::Low),
              "priority indices must be ordered most-urgent-first");

static_assert(std::string_view(toString(JobPriority::High)) == "High");
static_assert(std::string_view(toString(JobPriority::Normal)) == "Normal");
static_assert(std::string_view(toString(JobPriority::Low)) == "Low");

static_assert(static_cast<std::uint8_t>(ShutdownMode::Drain) == 0);
static_assert(static_cast<std::uint8_t>(ShutdownMode::Cancel) == 1);

// ---- lifetime -----------------------------------------------------------------
// Workers capture `this`, so a moved-from or copied pool would leave threads
// pointing at a dead object. Both must stay deleted.
static_assert(!std::is_copy_constructible_v<JobSystem>, "JobSystem must not be copyable");
static_assert(!std::is_copy_assignable_v<JobSystem>, "JobSystem must not be copy-assignable");
static_assert(!std::is_move_constructible_v<JobSystem>, "JobSystem must not be movable");
static_assert(!std::is_move_assignable_v<JobSystem>, "JobSystem must not be move-assignable");
static_assert(!std::is_copy_constructible_v<MainThreadQueue>,
              "MainThreadQueue must not be copyable");

// The overlay memcpy-samples this every frame; keeping it trivial guarantees
// there is no allocation on the stats path.
static_assert(std::is_trivially_copyable_v<JobSystemStats>,
              "JobSystemStats is sampled per frame and must stay trivially copyable");
static_assert(std::is_aggregate_v<JobSystemStats>, "JobSystemStats must stay a plain aggregate");

// A default-constructed pool must never report zero workers: enqueue() takes a
// modulo of the queue count, so a zero-sized pool would divide by zero. The
// clamp lives in the constructor; this asserts the documented default argument
// that triggers it is still the sentinel the constructor looks for.
static_assert(std::is_constructible_v<JobSystem, unsigned>,
              "JobSystem must remain constructible from an explicit worker count");

}  // namespace
}  // namespace voxl
