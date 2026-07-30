#pragma once

// Work-stealing thread pool plus the main-thread hand-back queue.
//
// THIS HEADER IS A CONTRACT. Terrain generation, meshing, lighting and chunk
// I/O all schedule through the single JobSystem owned by the application.
//
// ============================================================================
//  OPENGL IS FORBIDDEN ON WORKER THREADS.
// ============================================================================
//  The GL context created by voxl::Window is current on the main thread only.
//  Touching any gl* entry point from a worker is undefined behaviour and in
//  practice either crashes the driver or silently corrupts state on a later
//  frame, which is nearly impossible to bisect. A worker that produces data the
//  GPU needs (a ChunkMeshData, a decoded texture) must finish on the CPU and
//  then post a closure to `mainThreadQueue()`; the main thread drains that
//  queue once per frame with a time budget so a burst of freshly meshed chunks
//  cannot blow the frame time.
//
// Threading: every public member of JobSystem and MainThreadQueue is safe to
// call from any thread EXCEPT `MainThreadQueue::drain*` and
// `JobSystem::shutdown`, which are main-thread only. See the per-method notes.

#include "core/Log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace voxl {

/// Scheduling class. Workers always drain High before Normal before Low, so a
/// starved Low job is possible by design: background streaming must never delay
/// the chunk the player is staring at.
enum class JobPriority : std::uint8_t {
    /// Blocking the visible frame: remeshing a chunk the player just edited or
    /// is looking at, lighting updates inside the near ring.
    High = 0,
    /// Ordinary streaming: terrain generation and meshing for chunks entering
    /// the view distance.
    Normal = 1,
    /// Opportunistic work with no deadline: prefetching, saving, decompression
    /// of chunks the player is moving away from.
    Low = 2,
};

inline constexpr std::size_t kJobPriorityCount = 3;

[[nodiscard]] constexpr const char* toString(JobPriority priority) noexcept
{
    switch (priority) {
        case JobPriority::High:   return "High";
        case JobPriority::Normal: return "Normal";
        case JobPriority::Low:    return "Low";
    }
    return "Unknown";
}

/// What to do with work that is still queued when the pool is torn down.
enum class ShutdownMode : std::uint8_t {
    /// Finish everything already submitted, then join. Use at normal exit so
    /// pending chunk saves actually reach disk.
    Drain = 0,
    /// Discard queued jobs and join once the in-flight ones return. Futures for
    /// discarded jobs are broken (waiting on one throws std::future_error).
    Cancel = 1,
};

// ---------------------------------------------------------------------------
//  MainThreadQueue
// ---------------------------------------------------------------------------

/// Closures that must run on the thread owning the GL context.
///
/// This is the only sanctioned bridge from a worker back to the renderer.
/// Thread safety: `push` is safe from any thread; `drain`/`drainAll`/`clear`
/// are main-thread only (they execute the closures, which touch GL).
class MainThreadQueue {
public:
    using Clock = std::chrono::steady_clock;

    MainThreadQueue() = default;

    MainThreadQueue(const MainThreadQueue&)            = delete;
    MainThreadQueue& operator=(const MainThreadQueue&) = delete;

    /// Enqueues work for the next drain. Order is FIFO, which matters because
    /// a chunk's "upload mesh" closure must not overtake its "delete old mesh".
    void push(std::function<void()> task)
    {
        VOXL_ASSERT(static_cast<bool>(task), "null main-thread task");
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_tasks.push_back(std::move(task));
        }
        m_size.fetch_add(1, std::memory_order_release);
    }

    /// Runs queued closures until the budget is spent. MAIN THREAD ONLY.
    ///
    /// The budget is checked *after* each closure, so exactly one task always
    /// runs per call: a budget smaller than a single upload would otherwise
    /// deadlock streaming forever. A non-positive budget means "no limit".
    /// Returns the number of closures executed.
    std::size_t drain(std::chrono::microseconds budget)
    {
        const bool             unlimited = budget.count() <= 0;
        const Clock::time_point deadline = Clock::now() + budget;
        std::size_t            executed  = 0;

        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_tasks.empty()) {
                    break;
                }
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            m_size.fetch_sub(1, std::memory_order_release);

            runGuarded(task);
            ++executed;

            if (!unlimited && Clock::now() >= deadline) {
                break;
            }
        }
        return executed;
    }

    /// Runs everything currently queued, ignoring frame time. Used at shutdown
    /// and after a world load, never in the steady-state frame loop.
    std::size_t drainAll() { return drain(std::chrono::microseconds{0}); }

    /// Drops queued closures without running them. Only legal during teardown,
    /// after the GL resources they would have touched are already gone.
    void clear() noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.clear();
        m_size.store(0, std::memory_order_release);
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_size.load(std::memory_order_acquire); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

private:
    static void runGuarded(const std::function<void()>& task) noexcept
    {
        // A throwing GPU upload must not unwind through the frame loop; log and
        // keep the remaining uploads going.
        try {
            task();
        } catch (const std::exception& error) {
            VOXL_LOG_ERROR("main-thread task threw: {}", error.what());
        } catch (...) {
            VOXL_LOG_ERROR("main-thread task threw a non-std exception");
        }
    }

    mutable std::mutex                m_mutex;
    std::deque<std::function<void()>> m_tasks;
    /// Duplicated outside the mutex so the debug overlay can sample the depth
    /// every frame without contending with worker pushes.
    std::atomic<std::size_t> m_size{0};
};

// ---------------------------------------------------------------------------
//  JobSystem
// ---------------------------------------------------------------------------

/// Snapshot of pool occupancy for the debug overlay. Sampled without locks, so
/// the fields are individually accurate but not mutually consistent.
struct JobSystemStats {
    std::size_t queued            = 0;  ///< submitted, not yet started
    std::size_t active            = 0;  ///< currently running on a worker
    std::size_t outstanding       = 0;  ///< queued + active
    std::size_t mainThreadPending = 0;  ///< closures awaiting the next drain
    unsigned    workerCount       = 0;
};

/// Fixed-size pool of worker threads with per-worker deques and stealing.
///
/// Why per-worker deques rather than one global queue: chunk meshing submits in
/// bursts of hundreds of tiny jobs, and a single mutex-guarded queue turns the
/// submitting thread into the bottleneck. Each worker owns a deque, pushes to
/// its own when it submits (cache-warm continuation), pops from the front, and
/// steals from the back of a random-ish victim when it runs dry. Only the
/// sleep/wake handshake touches a shared mutex.
///
/// Thread safety: `submit`, `submitDetached`, `outstanding`, `stats`, `waitIdle`
/// are safe from any thread including workers. `shutdown` and the destructor
/// are main-thread only and must not race with submission.
class JobSystem {
public:
    /// `threadCount == 0` selects hardware_concurrency() - 1, leaving one core
    /// for the main thread, clamped to at least one worker so single-core
    /// machines still make progress.
    explicit JobSystem(unsigned threadCount = 0)
    {
        unsigned count = threadCount;
        if (count == 0) {
            const unsigned hardware = std::thread::hardware_concurrency();
            count = hardware > 1u ? hardware - 1u : 1u;
        }

        m_queues.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            m_queues.push_back(std::make_unique<WorkerQueue>());
        }

        m_threads.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            m_threads.emplace_back([this, i] { workerLoop(i); });
        }
        VOXL_LOG_INFO("JobSystem started with {} worker thread(s)", count);
    }

    ~JobSystem() { shutdown(ShutdownMode::Drain); }

    JobSystem(const JobSystem&)            = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&)                 = delete;
    JobSystem& operator=(JobSystem&&)      = delete;

    // ---- submission ----

    /// Schedules `fn` and returns a future for its result.
    ///
    /// The future costs a heap allocation and a shared state; prefer
    /// `submitDetached` for fire-and-forget work such as "generate this chunk
    /// and post the upload to the main-thread queue".
    template <typename Fn>
    [[nodiscard]] auto submit(JobPriority priority, Fn&& fn)
        -> std::future<std::invoke_result_t<std::decay_t<Fn>>>
    {
        using Result = std::invoke_result_t<std::decay_t<Fn>>;
        auto task    = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
        std::future<Result> future = task->get_future();
        enqueue(priority, [task]() { (*task)(); });
        return future;
    }

    /// Schedules `fn` with no result channel. Exceptions escaping `fn` are
    /// logged and swallowed - there is nobody to rethrow them to.
    template <typename Fn>
    void submitDetached(JobPriority priority, Fn&& fn)
    {
        enqueue(priority, std::function<void()>(std::forward<Fn>(fn)));
    }

    // ---- main-thread hand-back ----

    /// The queue every GPU upload must go through. See the file header.
    [[nodiscard]] MainThreadQueue& mainThreadQueue() noexcept { return m_mainThread; }
    [[nodiscard]] const MainThreadQueue& mainThreadQueue() const noexcept { return m_mainThread; }

    // ---- observation ----

    [[nodiscard]] std::size_t queuedCount() const noexcept
    {
        return m_queued.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t activeCount() const noexcept
    {
        return m_active.load(std::memory_order_acquire);
    }
    /// Jobs submitted but not yet finished. This is the number the debug
    /// overlay shows; it reaching zero is the "streaming has settled" signal.
    [[nodiscard]] std::size_t outstanding() const noexcept { return queuedCount() + activeCount(); }

    [[nodiscard]] unsigned workerCount() const noexcept
    {
        return static_cast<unsigned>(m_threads.size());
    }

    [[nodiscard]] JobSystemStats stats() const noexcept
    {
        JobSystemStats out;
        out.queued            = queuedCount();
        out.active            = activeCount();
        out.outstanding       = out.queued + out.active;
        out.mainThreadPending = m_mainThread.size();
        out.workerCount       = workerCount();
        return out;
    }

    /// True when called from one of this pool's workers. Used by assertions
    /// that guard main-thread-only code paths.
    [[nodiscard]] bool onWorkerThread() const noexcept { return t_owner == this; }

    /// Blocks until nothing is queued or running. Never call this from a worker
    /// (it would deadlock waiting on itself); only the main thread should.
    void waitIdle()
    {
        VOXL_CHECK(!onWorkerThread(), "waitIdle() called from a worker thread");
        std::unique_lock<std::mutex> lock(m_sync);
        m_idleCv.wait(lock, [this] { return outstanding() == 0; });
    }

    // ---- teardown ----

    /// Stops the pool and joins every worker. Idempotent. MAIN THREAD ONLY, and
    /// no submission may be in flight concurrently.
    ///
    /// Drain mode waits for the backlog first; Cancel mode discards it. Either
    /// way the main-thread queue is left alone - the caller decides whether the
    /// pending uploads are still meaningful.
    void shutdown(ShutdownMode mode = ShutdownMode::Drain)
    {
        if (m_stopping.load(std::memory_order_acquire)) {
            return;
        }

        if (mode == ShutdownMode::Drain) {
            waitIdle();
        } else {
            discardQueuedWork();
        }

        {
            std::lock_guard<std::mutex> lock(m_sync);
            m_stopping.store(true, std::memory_order_release);
        }
        m_workCv.notify_all();

        for (std::thread& thread : m_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        m_threads.clear();

        // Anything still queued in Cancel mode is dropped here; the futures for
        // those jobs become broken promises, which is the documented contract.
        discardQueuedWork();
    }

private:
    using Job = std::function<void()>;

    /// One deque per priority per worker. Cache-line aligned so two workers
    /// stealing from different victims do not ping-pong the same line.
    ///
    /// The trailing padding MSVC warns about (C4324) is exactly the point: a
    /// worker's mutex must not share a cache line with the next worker's.
#pragma warning(push)
#pragma warning(disable : 4324)
    struct alignas(64) WorkerQueue {
        std::mutex       mutex;
        std::deque<Job>  byPriority[kJobPriorityCount];

        /// Caller must hold `mutex`.
        [[nodiscard]] bool emptyLocked() const noexcept
        {
            for (std::size_t p = 0; p < kJobPriorityCount; ++p) {
                if (!byPriority[p].empty()) {
                    return false;
                }
            }
            return true;
        }
    };
#pragma warning(pop)

    void enqueue(JobPriority priority, Job job)
    {
        VOXL_CHECK(static_cast<bool>(job), "null job submitted");
        VOXL_CHECK(!m_stopping.load(std::memory_order_acquire),
                   "job submitted to a JobSystem that is shutting down");

        // Submitting from a worker keeps the continuation on the same core,
        // where the data it will touch is already hot.
        const std::size_t target =
            onWorkerThread() ? t_workerIndex
                             : (m_roundRobin.fetch_add(1, std::memory_order_relaxed) % m_queues.size());

        {
            WorkerQueue& queue = *m_queues[target];
            std::lock_guard<std::mutex> lock(queue.mutex);
            queue.byPriority[static_cast<std::size_t>(priority)].push_back(std::move(job));
        }
        m_queued.fetch_add(1, std::memory_order_release);

        // The empty critical section is not redundant: it orders this increment
        // against a worker that is between its predicate check and its sleep,
        // which is the classic lost-wakeup in a pool like this.
        {
            std::lock_guard<std::mutex> lock(m_sync);
        }
        m_workCv.notify_one();
    }

    /// Pops from `index`'s own deque, honouring priority order. Caller owns the
    /// accounting side effects performed here.
    bool tryPopLocal(std::size_t index, Job& out)
    {
        WorkerQueue& queue = *m_queues[index];
        std::lock_guard<std::mutex> lock(queue.mutex);
        for (std::size_t p = 0; p < kJobPriorityCount; ++p) {
            std::deque<Job>& deque = queue.byPriority[p];
            if (!deque.empty()) {
                out = std::move(deque.front());
                deque.pop_front();
                return true;
            }
        }
        return false;
    }

    /// Steals from the *back* of a victim's deque so the victim keeps working
    /// on its own cache-hot front without contending on the same element.
    bool trySteal(std::size_t victim, Job& out)
    {
        WorkerQueue& queue = *m_queues[victim];
        std::unique_lock<std::mutex> lock(queue.mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            return false;  // busy victim: move on rather than block
        }
        for (std::size_t p = 0; p < kJobPriorityCount; ++p) {
            std::deque<Job>& deque = queue.byPriority[p];
            if (!deque.empty()) {
                out = std::move(deque.back());
                deque.pop_back();
                return true;
            }
        }
        return false;
    }

    bool acquireJob(std::size_t index, Job& out)
    {
        bool found = tryPopLocal(index, out);
        if (!found) {
            const std::size_t count = m_queues.size();
            for (std::size_t offset = 1; offset < count && !found; ++offset) {
                found = trySteal((index + offset) % count, out);
            }
        }
        if (!found) {
            return false;
        }
        // Increment active before decrementing queued: if the order were
        // reversed, `outstanding()` would dip to zero between the two and
        // waitIdle() could return while a job is about to run.
        m_active.fetch_add(1, std::memory_order_release);
        m_queued.fetch_sub(1, std::memory_order_release);
        return true;
    }

    void workerLoop(unsigned index)
    {
        t_owner       = this;
        t_workerIndex = index;

        for (;;) {
            Job job;
            if (acquireJob(index, job)) {
                runJob(job);
                continue;
            }

            std::unique_lock<std::mutex> lock(m_sync);
            if (m_stopping.load(std::memory_order_acquire)) {
                break;
            }
            m_workCv.wait(lock, [this] {
                return m_stopping.load(std::memory_order_acquire) ||
                       m_queued.load(std::memory_order_acquire) > 0;
            });
            if (m_stopping.load(std::memory_order_acquire) &&
                m_queued.load(std::memory_order_acquire) == 0) {
                break;
            }
        }

        t_owner       = nullptr;
        t_workerIndex = 0;
    }

    void runJob(const Job& job) noexcept
    {
        // packaged_task already captures exceptions into its future; this catch
        // exists for detached jobs, where an escaping exception would call
        // std::terminate and take the whole game down.
        try {
            job();
        } catch (const std::exception& error) {
            VOXL_LOG_ERROR("worker job threw: {}", error.what());
        } catch (...) {
            VOXL_LOG_ERROR("worker job threw a non-std exception");
        }

        m_active.fetch_sub(1, std::memory_order_release);
        if (outstanding() == 0) {
            // Taken briefly so a waiter that just evaluated the predicate as
            // false cannot miss this notification.
            {
                std::lock_guard<std::mutex> lock(m_sync);
            }
            m_idleCv.notify_all();
        }
    }

    void discardQueuedWork()
    {
        std::size_t dropped = 0;
        for (std::unique_ptr<WorkerQueue>& queue : m_queues) {
            std::lock_guard<std::mutex> lock(queue->mutex);
            for (std::size_t p = 0; p < kJobPriorityCount; ++p) {
                dropped += queue->byPriority[p].size();
                queue->byPriority[p].clear();
            }
        }
        if (dropped > 0) {
            m_queued.fetch_sub(dropped, std::memory_order_release);
            VOXL_LOG_WARN("JobSystem cancelled {} queued job(s)", dropped);
        }
    }

    std::vector<std::unique_ptr<WorkerQueue>> m_queues;
    std::vector<std::thread>                  m_threads;

    /// Guards the two condition variables only; the work deques have their own
    /// locks so stealing never serialises on this.
    mutable std::mutex      m_sync;
    std::condition_variable m_workCv;
    std::condition_variable m_idleCv;

    std::atomic<bool>        m_stopping{false};
    std::atomic<std::size_t> m_queued{0};
    std::atomic<std::size_t> m_active{0};
    std::atomic<std::size_t> m_roundRobin{0};

    MainThreadQueue m_mainThread;

    /// Identifies the pool a thread belongs to. A pointer rather than a bool so
    /// that two JobSystems (game + tests) cannot confuse each other.
    static inline thread_local const JobSystem* t_owner       = nullptr;
    static inline thread_local unsigned         t_workerIndex = 0;
};

}  // namespace voxl
