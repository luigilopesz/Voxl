// Tests for the worker pool, the main-thread hand-back queue and the CPU
// profiler that hangs off them.
//
// Rule for everything in this file: never wait forever. A test that hangs on a
// build agent looks exactly like a flaky agent and gets ignored, so every wait
// on another thread goes through waitFor() and fails loudly on timeout.

#include <catch2/catch_test_macros.hpp>

#include "core/JobSystem.hpp"
#include "core/Profiler.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// At file scope rather than inside the unnamed namespace below: the literal
// operators are used throughout the test bodies, not just by the helpers.
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
[[nodiscard]] bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(200us);
    }
    return true;
}

/// Blocks a worker until it is explicitly released, so a test can queue work
/// behind it and observe the queue rather than a race.
struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};

    void block()
    {
        entered.store(true, std::memory_order_release);
        while (!released.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(200us);
        }
    }
};

}  // namespace

// ---------------------------------------------------------------- scheduling --

TEST_CASE("every submitted job runs exactly once", "[core][jobsystem]")
{
    constexpr std::size_t kJobCount = 1024;

    voxl::JobSystem jobs(4);
    std::vector<std::atomic<unsigned>> counters(kJobCount);

    for (std::size_t i = 0; i < kJobCount; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&counters, i] {
            counters[i].fetch_add(1u, std::memory_order_relaxed);
        });
    }

    jobs.waitIdle();
    CHECK(jobs.outstanding() == 0);

    std::size_t wrongCount = 0;
    for (const auto& counter : counters) {
        if (counter.load(std::memory_order_relaxed) != 1u) {
            ++wrongCount;
        }
    }
    CHECK(wrongCount == 0);
}

TEST_CASE("futures deliver job results", "[core][jobsystem]")
{
    constexpr int kJobCount = 64;

    voxl::JobSystem jobs(3);
    std::vector<std::future<int>> results;
    results.reserve(kJobCount);

    for (int i = 0; i < kJobCount; ++i) {
        results.push_back(jobs.submit(voxl::JobPriority::Normal, [i] { return i * i; }));
    }

    for (int i = 0; i < kJobCount; ++i) {
        CHECK(results[static_cast<std::size_t>(i)].get() == i * i);
    }

    // A packaged_task captures the exception itself, so the pool's own
    // catch-and-log must not swallow it before the future sees it.
    std::future<int> thrower =
        jobs.submit(voxl::JobPriority::High, []() -> int { throw std::runtime_error("boom"); });
    CHECK_THROWS_AS(thrower.get(), std::runtime_error);

    // A void result still has to synchronise.
    std::atomic<bool> ran{false};
    std::future<void> done =
        jobs.submit(voxl::JobPriority::High, [&ran] { ran.store(true, std::memory_order_release); });
    done.get();
    CHECK(ran.load(std::memory_order_acquire));
}

TEST_CASE("high priority work is serviced before normal and low", "[core][jobsystem]")
{
    // One worker, so the three jobs below are all queued before any of them can
    // start and the pop order is the only thing under test.
    voxl::JobSystem jobs(1);

    Gate gate;
    jobs.submitDetached(voxl::JobPriority::High, [&gate] { gate.block(); });
    REQUIRE(waitFor([&gate] { return gate.entered.load(std::memory_order_acquire); }));

    std::mutex                      orderMutex;
    std::vector<voxl::JobPriority>  order;
    const auto record = [&orderMutex, &order](voxl::JobPriority priority) {
        std::lock_guard<std::mutex> lock(orderMutex);
        order.push_back(priority);
    };

    jobs.submitDetached(voxl::JobPriority::Low, [&record] { record(voxl::JobPriority::Low); });
    jobs.submitDetached(voxl::JobPriority::Normal, [&record] { record(voxl::JobPriority::Normal); });
    jobs.submitDetached(voxl::JobPriority::High, [&record] { record(voxl::JobPriority::High); });

    gate.released.store(true, std::memory_order_release);
    jobs.waitIdle();

    std::lock_guard<std::mutex> lock(orderMutex);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == voxl::JobPriority::High);
    CHECK(order[1] == voxl::JobPriority::Normal);
    CHECK(order[2] == voxl::JobPriority::Low);
}

TEST_CASE("a job may submit more work and waitIdle covers the children",
          "[core][jobsystem]")
{
    constexpr unsigned kParents  = 32;
    constexpr unsigned kChildren = 8;

    voxl::JobSystem       jobs(4);
    std::atomic<unsigned> leaves{0};

    for (unsigned i = 0; i < kParents; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&jobs, &leaves] {
            for (unsigned k = 0; k < kChildren; ++k) {
                jobs.submitDetached(voxl::JobPriority::Low, [&leaves] {
                    leaves.fetch_add(1u, std::memory_order_relaxed);
                });
            }
        });
    }

    // This is the assertion that pins the "increment active before decrementing
    // queued" ordering in acquireJob: if outstanding() could dip to zero between
    // the two, waitIdle would return with children still pending.
    jobs.waitIdle();
    CHECK(leaves.load(std::memory_order_relaxed) == kParents * kChildren);
}

TEST_CASE("onWorkerThread identifies the owning pool, not any pool", "[core][jobsystem]")
{
    voxl::JobSystem jobs(2);
    CHECK_FALSE(jobs.onWorkerThread());

    std::future<bool> inside = jobs.submit(voxl::JobPriority::High, [&jobs] {
        return jobs.onWorkerThread();
    });
    CHECK(inside.get());

    voxl::JobSystem  other(1);
    std::future<bool> foreign = other.submit(voxl::JobPriority::High, [&jobs] {
        return jobs.onWorkerThread();
    });
    CHECK_FALSE(foreign.get());
}

TEST_CASE("stats describe an idle pool", "[core][jobsystem]")
{
    voxl::JobSystem jobs(2);

    const voxl::JobSystemStats idle = jobs.stats();
    CHECK(idle.workerCount == 2);
    CHECK(idle.queued == 0);
    CHECK(idle.active == 0);
    CHECK(idle.outstanding == 0);
    CHECK(idle.mainThreadPending == 0);

    jobs.mainThreadQueue().push([] {});
    CHECK(jobs.stats().mainThreadPending == 1);
    jobs.mainThreadQueue().clear();
    CHECK(jobs.mainThreadQueue().empty());
}

TEST_CASE("default worker count leaves a core for the main thread", "[core][jobsystem]")
{
    voxl::JobSystem  jobs(0);
    const unsigned   hardware = std::thread::hardware_concurrency();
    const unsigned   expected = hardware > 1u ? hardware - 1u : 1u;
    CHECK(jobs.workerCount() == expected);
    CHECK(jobs.workerCount() >= 1u);
}

// ------------------------------------------------------- main-thread queue --

TEST_CASE("main-thread queue runs callbacks only on the draining thread",
          "[core][jobsystem]")
{
    constexpr std::size_t kTaskCount = 32;

    voxl::JobSystem            jobs(4);
    const std::thread::id      mainThread = std::this_thread::get_id();
    std::mutex                 recordMutex;
    std::vector<std::thread::id> ranOn;

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&jobs, &recordMutex, &ranOn] {
            jobs.mainThreadQueue().push([&recordMutex, &ranOn] {
                std::lock_guard<std::mutex> lock(recordMutex);
                ranOn.push_back(std::this_thread::get_id());
            });
        });
    }

    jobs.waitIdle();
    REQUIRE(jobs.mainThreadQueue().size() == kTaskCount);

    // Nothing may have executed yet: pushing from a worker must not run the
    // closure on that worker, or GL would be touched off-thread.
    {
        std::lock_guard<std::mutex> lock(recordMutex);
        CHECK(ranOn.empty());
    }

    CHECK(jobs.mainThreadQueue().drainAll() == kTaskCount);
    CHECK(jobs.mainThreadQueue().empty());

    std::lock_guard<std::mutex> lock(recordMutex);
    REQUIRE(ranOn.size() == kTaskCount);
    std::size_t offThread = 0;
    for (const std::thread::id& id : ranOn) {
        if (id != mainThread) {
            ++offThread;
        }
    }
    CHECK(offThread == 0);
}

TEST_CASE("main-thread queue is FIFO", "[core][jobsystem]")
{
    constexpr int kTaskCount = 16;

    voxl::JobSystem  jobs(1);
    std::vector<int> order;
    order.reserve(kTaskCount);

    for (int i = 0; i < kTaskCount; ++i) {
        jobs.mainThreadQueue().push([&order, i] { order.push_back(i); });
    }
    jobs.mainThreadQueue().drainAll();

    std::vector<int> expected(kTaskCount);
    std::iota(expected.begin(), expected.end(), 0);
    CHECK(order == expected);
}

TEST_CASE("drain honours its time budget", "[core][jobsystem]")
{
    constexpr std::size_t kTaskCount = 64;

    voxl::JobSystem       jobs(1);
    std::atomic<unsigned> ran{0};

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        jobs.mainThreadQueue().push([&ran] {
            ran.fetch_add(1u, std::memory_order_relaxed);
            std::this_thread::sleep_for(2ms);
        });
    }

    // 64 tasks x 2ms would be a 128ms hitch. A 4ms budget must stop early.
    const std::size_t executed = jobs.mainThreadQueue().drain(4ms);
    CHECK(executed >= 1);
    CHECK(executed < kTaskCount);
    CHECK(jobs.mainThreadQueue().size() == kTaskCount - executed);

    // A budget smaller than a single task still makes progress; otherwise a
    // stream of uploads could never start.
    const std::size_t forced = jobs.mainThreadQueue().drain(std::chrono::microseconds{1});
    CHECK(forced == 1);

    // Non-positive means unlimited.
    const std::size_t rest = jobs.mainThreadQueue().drain(std::chrono::microseconds{0});
    CHECK(rest == kTaskCount - executed - 1);
    CHECK(jobs.mainThreadQueue().empty());
    CHECK(ran.load(std::memory_order_relaxed) == kTaskCount);
}

TEST_CASE("a throwing main-thread task does not abort the drain", "[core][jobsystem]")
{
    voxl::JobSystem       jobs(1);
    std::atomic<unsigned> ran{0};

    jobs.mainThreadQueue().push([&ran] { ran.fetch_add(1u, std::memory_order_relaxed); });
    jobs.mainThreadQueue().push([] { throw std::runtime_error("upload failed"); });
    jobs.mainThreadQueue().push([&ran] { ran.fetch_add(1u, std::memory_order_relaxed); });

    CHECK(jobs.mainThreadQueue().drainAll() == 3);
    CHECK(ran.load(std::memory_order_relaxed) == 2);
}

// ---------------------------------------------------------------- shutdown --

TEST_CASE("shutdown drains queued work without hanging", "[core][jobsystem]")
{
    constexpr unsigned kJobCount = 2000;

    // Declared outside the pool's scope so the destructor path can be checked
    // after the workers are gone.
    std::atomic<unsigned> ran{0};
    {
        voxl::JobSystem jobs(4);
        for (unsigned i = 0; i < kJobCount; ++i) {
            jobs.submitDetached(voxl::JobPriority::Low, [&ran] {
                ran.fetch_add(1u, std::memory_order_relaxed);
            });
        }

        jobs.shutdown(voxl::ShutdownMode::Drain);

        CHECK(ran.load(std::memory_order_relaxed) == kJobCount);
        CHECK(jobs.outstanding() == 0);
        CHECK(jobs.workerCount() == 0);  // threads joined, not detached

        jobs.shutdown(voxl::ShutdownMode::Drain);  // idempotent
        jobs.shutdown(voxl::ShutdownMode::Cancel);
    }
    CHECK(ran.load(std::memory_order_relaxed) == kJobCount);
}

TEST_CASE("the destructor drains work nobody waited for", "[core][jobsystem]")
{
    constexpr unsigned kJobCount = 512;

    std::atomic<unsigned> ran{0};
    {
        voxl::JobSystem jobs(3);
        for (unsigned i = 0; i < kJobCount; ++i) {
            jobs.submitDetached(voxl::JobPriority::Normal, [&ran] {
                ran.fetch_add(1u, std::memory_order_relaxed);
            });
        }
    }
    CHECK(ran.load(std::memory_order_relaxed) == kJobCount);
}

TEST_CASE("shutdown drains work that a running job submitted", "[core][jobsystem]")
{
    constexpr unsigned kParents  = 64;
    constexpr unsigned kChildren = 4;

    std::atomic<unsigned> leaves{0};
    {
        voxl::JobSystem jobs(4);
        for (unsigned i = 0; i < kParents; ++i) {
            jobs.submitDetached(voxl::JobPriority::Normal, [&jobs, &leaves] {
                for (unsigned k = 0; k < kChildren; ++k) {
                    jobs.submitDetached(voxl::JobPriority::Low, [&leaves] {
                        leaves.fetch_add(1u, std::memory_order_relaxed);
                    });
                }
            });
        }
        jobs.shutdown(voxl::ShutdownMode::Drain);
    }
    CHECK(leaves.load(std::memory_order_relaxed) == kParents * kChildren);
}

TEST_CASE("cancelling shutdown discards queued work and breaks its futures",
          "[core][jobsystem]")
{
    voxl::JobSystem       jobs(1);
    std::atomic<unsigned> ran{0};

    Gate gate;
    jobs.submitDetached(voxl::JobPriority::High, [&gate] { gate.block(); });
    REQUIRE(waitFor([&gate] { return gate.entered.load(std::memory_order_acquire); }));

    // The single worker is parked in the gate, so everything below is still in a
    // deque when the cancel lands.
    std::future<int> orphan = jobs.submit(voxl::JobPriority::Normal, [] { return 7; });
    for (unsigned i = 0; i < 64; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&ran] {
            ran.fetch_add(1u, std::memory_order_relaxed);
        });
    }
    REQUIRE(jobs.queuedCount() == 65);

    // shutdown() blocks in join(), so the gate has to be opened from elsewhere.
    // It is released well after the cancel has already emptied the deques.
    std::thread opener([&gate] {
        std::this_thread::sleep_for(50ms);
        gate.released.store(true, std::memory_order_release);
    });
    jobs.shutdown(voxl::ShutdownMode::Cancel);
    opener.join();

    CHECK(ran.load(std::memory_order_relaxed) == 0);
    CHECK(jobs.outstanding() == 0);
    CHECK(jobs.workerCount() == 0);

    REQUIRE(orphan.valid());
    CHECK_THROWS_AS(orphan.get(), std::future_error);
}

TEST_CASE("cancelling shutdown leaves the main-thread queue for the caller",
          "[core][jobsystem]")
{
    voxl::JobSystem jobs(2);
    jobs.mainThreadQueue().push([] {});
    jobs.mainThreadQueue().push([] {});

    jobs.shutdown(voxl::ShutdownMode::Cancel);

    // The pool must not decide on the caller's behalf whether pending uploads
    // are still meaningful.
    CHECK(jobs.mainThreadQueue().size() == 2);
    CHECK(jobs.mainThreadQueue().drainAll() == 2);
}

// ------------------------------------------------------------------ stress --

TEST_CASE("10k jobs submitted from many threads all run", "[core][jobsystem][stress]")
{
    constexpr unsigned kProducers   = 4;
    constexpr unsigned kPerProducer = 2500;

    voxl::JobSystem            jobs(0);
    std::atomic<std::uint64_t> sum{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (unsigned p = 0; p < kProducers; ++p) {
        producers.emplace_back([&jobs, &sum, p] {
            const auto priority =
                static_cast<voxl::JobPriority>(p % static_cast<unsigned>(voxl::kJobPriorityCount));
            for (unsigned i = 0; i < kPerProducer; ++i) {
                jobs.submitDetached(priority, [&sum] {
                    sum.fetch_add(1u, std::memory_order_relaxed);
                });
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }

    jobs.waitIdle();
    CHECK(sum.load(std::memory_order_relaxed) ==
          static_cast<std::uint64_t>(kProducers) * kPerProducer);
    CHECK(jobs.outstanding() == 0);
}

TEST_CASE("workers hand 10k results back through the main-thread queue",
          "[core][jobsystem][stress]")
{
    constexpr unsigned kJobCount = 10000;

    voxl::JobSystem       jobs(0);
    std::atomic<unsigned> handedBack{0};

    for (unsigned i = 0; i < kJobCount; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&jobs, &handedBack] {
            jobs.mainThreadQueue().push([&handedBack] {
                handedBack.fetch_add(1u, std::memory_order_relaxed);
            });
        });
    }

    jobs.waitIdle();
    REQUIRE(jobs.mainThreadQueue().size() == kJobCount);

    // Budgeted drains, the way the frame loop does it: this must terminate.
    std::size_t drained = 0;
    while (!jobs.mainThreadQueue().empty()) {
        drained += jobs.mainThreadQueue().drain(2ms);
    }
    CHECK(drained == kJobCount);
    CHECK(handedBack.load(std::memory_order_relaxed) == kJobCount);
}

// ---------------------------------------------------------------- profiler --

namespace {
/// Separate function so the macro's per-line id cache is exercised with two
/// different Profiler instances.
void timeMacroScope(voxl::Profiler& profiler)
{
    VOXL_PROFILE_SCOPE(profiler, "Macro.Scope");
    std::this_thread::sleep_for(100us);
}
}  // namespace

TEST_CASE("profiler accumulates a named scope per frame", "[core][profiler]")
{
    voxl::Profiler profiler;

    const voxl::ProfileId id = profiler.scopeId("Test.Scope");
    REQUIRE(id != voxl::kInvalidProfileId);
    CHECK(profiler.scopeId("Test.Scope") == id);  // interning is idempotent
    CHECK(profiler.scopeCount() == 1);
    CHECK(profiler.scopeName(id) == "Test.Scope");

    profiler.addSampleMs(id, 2.0f);
    profiler.addSampleMs(id, 3.0f);
    profiler.endFrame();

    const voxl::ProfileScopeStats first = profiler.scopeStats(id);
    CHECK(first.name == "Test.Scope");
    CHECK(first.lastCalls == 2);
    CHECK(first.totalCalls == 2);
    CHECK(first.lastMs > 4.9f);
    CHECK(first.lastMs < 5.1f);

    // A frame in which the scope was never entered reports zero for the frame
    // but must not drag the average or pin the minimum at zero.
    profiler.endFrame();
    const voxl::ProfileScopeStats second = profiler.scopeStats(id);
    CHECK(second.lastCalls == 0);
    CHECK(second.lastMs == 0.0f);
    CHECK(second.totalCalls == 2);
    CHECK(second.averageMs > 4.9f);
    CHECK(second.minMs > 4.9f);
    CHECK(profiler.frameIndex() == 2);
}

TEST_CASE("profiler snapshot lists every interned scope", "[core][profiler]")
{
    voxl::Profiler profiler;
    const voxl::ProfileId a = profiler.scopeId("A");
    const voxl::ProfileId b = profiler.scopeId("B");
    profiler.addSampleMs(a, 1.0f);
    profiler.addSampleMs(b, 2.0f);
    profiler.endFrame();

    std::vector<voxl::ProfileScopeStats> rows;
    profiler.snapshot(rows);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].name == "A");
    CHECK(rows[1].name == "B");
    CHECK(rows[1].lastMs > rows[0].lastMs);
    CHECK(profiler.frameTotalMs() > 2.9f);

    CHECK(profiler.snapshot().size() == 2);

    profiler.reset();
    CHECK(profiler.scopeCount() == 2);  // ids stay valid across a reset
    CHECK(profiler.scopeStats(a).totalCalls == 0);
    CHECK(profiler.frameIndex() == 0);
}

TEST_CASE("profiler ignores invalid ids and honours the enabled flag",
          "[core][profiler]")
{
    voxl::Profiler profiler;
    const voxl::ProfileId id = profiler.scopeId("Gated");

    {
        // Sampling an invalid id must be a silent no-op so call sites never have
        // to branch on the result of scopeId().
        voxl::ProfileScope scope(profiler, voxl::kInvalidProfileId);
    }
    profiler.endFrame();
    CHECK(profiler.scopeCount() == 1);
    CHECK(profiler.scopeStats(id).totalCalls == 0);

    profiler.setEnabled(false);
    CHECK_FALSE(profiler.enabled());
    profiler.addSampleMs(id, 5.0f);
    profiler.endFrame();
    CHECK(profiler.scopeStats(id).totalCalls == 0);

    profiler.setEnabled(true);
    profiler.addSampleMs(id, 5.0f);
    profiler.endFrame();
    CHECK(profiler.scopeStats(id).totalCalls == 1);
}

TEST_CASE("profiler refuses to grow past its slot count", "[core][profiler]")
{
    // Names are stored as views, so they must outlive the profiler: declared
    // first here, destroyed last.
    std::vector<std::string> names;
    names.reserve(voxl::Profiler::kMaxScopes + 1);
    for (std::size_t i = 0; i <= voxl::Profiler::kMaxScopes; ++i) {
        names.push_back("scope" + std::to_string(i));
    }

    voxl::Profiler profiler;
    for (std::size_t i = 0; i < voxl::Profiler::kMaxScopes; ++i) {
        CHECK(profiler.scopeId(names[i]) != voxl::kInvalidProfileId);
    }
    CHECK(profiler.scopeCount() == voxl::Profiler::kMaxScopes);
    CHECK(profiler.scopeId(names[voxl::Profiler::kMaxScopes]) == voxl::kInvalidProfileId);
    CHECK(profiler.scopeCount() == voxl::Profiler::kMaxScopes);
}

TEST_CASE("the profile scope macro caches per profiler", "[core][profiler]")
{
    voxl::Profiler first;
    voxl::Profiler second;

    timeMacroScope(first);
    timeMacroScope(second);
    timeMacroScope(first);

    first.endFrame();
    second.endFrame();

    REQUIRE(first.scopeCount() == 1);
    REQUIRE(second.scopeCount() == 1);
    CHECK(first.scopeStats(voxl::ProfileId{0}).lastCalls == 2);
    CHECK(second.scopeStats(voxl::ProfileId{0}).lastCalls == 1);
    CHECK(first.scopeStats(voxl::ProfileId{0}).name == "Macro.Scope");
}

TEST_CASE("profiler scopes can be timed from worker threads",
          "[core][profiler][jobsystem]")
{
    constexpr unsigned kJobCount = 256;

    voxl::Profiler  profiler;
    voxl::JobSystem jobs(4);

    const voxl::ProfileId id = profiler.scopeId("Worker.Job");
    for (unsigned i = 0; i < kJobCount; ++i) {
        jobs.submitDetached(voxl::JobPriority::Normal, [&profiler, id] {
            VOXL_PROFILE_SCOPE_ID(profiler, id);
            std::this_thread::sleep_for(50us);
        });
    }
    jobs.waitIdle();
    profiler.endFrame();

    const voxl::ProfileScopeStats stats = profiler.scopeStats(id);
    CHECK(stats.lastCalls == kJobCount);
    CHECK(stats.totalCalls == kJobCount);
    CHECK(stats.lastMs > 0.0f);

    profiler.resetExtremes();
    CHECK(profiler.scopeStats(id).minMs == 0.0f);
}
