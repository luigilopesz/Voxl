#include "core/Profiler.hpp"

#include "core/Log.hpp"

#include <chrono>

namespace voxl {

namespace {
/// One millisecond in the units the accumulator stores. Kept as a double because
/// a frame's worth of nanoseconds exceeds float's exact integer range within
/// about 17ms, and the overlay would start showing quantised values.
constexpr double kNanosPerMillisecond = 1.0e6;
}  // namespace

ProfileId Profiler::scopeId(std::string_view name)
{
    VOXL_CHECK(!name.empty(), "profiler scope name must not be empty");

    std::lock_guard<std::mutex> lock(m_nameMutex);

    // Relaxed is enough under the lock: every writer of m_count holds it, so
    // this thread's view of it cannot be stale.
    const std::size_t count = m_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        if (m_names[i] == name) {
            return static_cast<ProfileId>(i);
        }
    }

    if (count >= kMaxScopes) {
        if (!m_warnedFull) {
            m_warnedFull = true;
            VOXL_LOG_WARN("Profiler is full at {} scopes; '{}' will not be timed",
                          static_cast<std::size_t>(kMaxScopes), name);
        }
        return kInvalidProfileId;
    }

    m_names[count] = name;
    // Release pairs with the acquire in scopeCount(): a thread that observes the
    // higher count is guaranteed to see the name that goes with it. The
    // accumulator for this slot is already zero from construction, so there is
    // nothing else to publish.
    m_count.store(count + 1, std::memory_order_release);
    return static_cast<ProfileId>(count);
}

void Profiler::addSample(ProfileId id, Clock::duration elapsed) noexcept
{
    if (!enabled() || id == kInvalidProfileId || static_cast<std::size_t>(id) >= kMaxScopes) {
        return;
    }

    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

    // Relaxed: these counters carry no ownership of other data, and the only
    // reader (endFrame, main thread) is happy to attribute a late arrival to the
    // following frame. Anything stronger would put a fence in a meshing loop.
    Accumulator& slot = m_accumulators[id];
    slot.nanos.fetch_add(static_cast<std::int64_t>(nanos), std::memory_order_relaxed);
    slot.calls.fetch_add(1u, std::memory_order_relaxed);
}

void Profiler::addSampleMs(ProfileId id, float milliseconds) noexcept
{
    if (!enabled() || id == kInvalidProfileId || static_cast<std::size_t>(id) >= kMaxScopes) {
        return;
    }
    // Clamp instead of asserting: a caller that computed a negative delta from a
    // clock hiccup should not corrupt the running total.
    const double nanos = static_cast<double>(milliseconds) * kNanosPerMillisecond;
    const std::int64_t whole = nanos > 0.0 ? static_cast<std::int64_t>(nanos) : 0;

    Accumulator& slot = m_accumulators[id];
    slot.nanos.fetch_add(whole, std::memory_order_relaxed);
    slot.calls.fetch_add(1u, std::memory_order_relaxed);
}

void Profiler::endFrame() noexcept
{
    const std::size_t count = scopeCount();
    float             total = 0.0f;

    for (std::size_t i = 0; i < count; ++i) {
        Accumulator& slot = m_accumulators[i];
        // exchange rather than load+store: a worker may still be adding to this
        // slot, and the read-modify-write guarantees its contribution is either
        // fully in this frame or fully in the next one, never dropped.
        const std::int64_t  nanos = slot.nanos.exchange(0, std::memory_order_relaxed);
        const std::uint32_t calls = slot.calls.exchange(0u, std::memory_order_relaxed);

        const float milliseconds = static_cast<float>(static_cast<double>(nanos) / kNanosPerMillisecond);

        History& history   = m_history[i];
        history.lastMs     = milliseconds;
        history.lastCalls  = calls;
        history.totalCalls += calls;

        // A scope that was not entered contributes nothing. Recording a 0ms
        // sample instead would drag the average toward zero and pin the minimum
        // at zero, which makes an occasional scope (world save, chunk load)
        // unreadable in the overlay.
        if (calls > 0) {
            history.sample.add(milliseconds);
        }

        total += milliseconds;
    }

    m_frameTotalMs = total;
    ++m_frameIndex;
}

std::string_view Profiler::scopeName(ProfileId id) const
{
    if (!valid(id)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(m_nameMutex);
    return m_names[id];
}

ProfileScopeStats Profiler::scopeStats(ProfileId id) const
{
    ProfileScopeStats out;
    if (!valid(id)) {
        return out;
    }

    const History& history = m_history[id];
    out.name       = scopeName(id);
    out.lastMs     = history.lastMs;
    out.averageMs  = history.sample.averageMs();
    out.minMs      = history.sample.minMs();
    out.maxMs      = history.sample.maxMs();
    out.lastCalls  = history.lastCalls;
    out.totalCalls = history.totalCalls;
    return out;
}

void Profiler::snapshot(std::vector<ProfileScopeStats>& out) const
{
    const std::size_t count = scopeCount();
    out.clear();
    out.reserve(count);

    // One lock for the whole table rather than one per row: scopeStats() would
    // otherwise take and release m_nameMutex `count` times per frame.
    std::lock_guard<std::mutex> lock(m_nameMutex);
    for (std::size_t i = 0; i < count; ++i) {
        const History& history = m_history[i];

        ProfileScopeStats row;
        row.name       = m_names[i];
        row.lastMs     = history.lastMs;
        row.averageMs  = history.sample.averageMs();
        row.minMs      = history.sample.minMs();
        row.maxMs      = history.sample.maxMs();
        row.lastCalls  = history.lastCalls;
        row.totalCalls = history.totalCalls;
        out.push_back(row);
    }
}

std::vector<ProfileScopeStats> Profiler::snapshot() const
{
    std::vector<ProfileScopeStats> out;
    snapshot(out);
    return out;
}

void Profiler::resetExtremes() noexcept
{
    const std::size_t count = scopeCount();
    for (std::size_t i = 0; i < count; ++i) {
        m_history[i].sample.resetExtremes();
    }
}

void Profiler::reset() noexcept
{
    // Names and ids survive on purpose: a call site that cached an id must not
    // start writing into somebody else's row after a reset.
    for (std::size_t i = 0; i < kMaxScopes; ++i) {
        m_accumulators[i].nanos.store(0, std::memory_order_relaxed);
        m_accumulators[i].calls.store(0u, std::memory_order_relaxed);
        m_history[i] = History{};
    }
    m_frameIndex   = 0;
    m_frameTotalMs = 0.0f;
}

ProfileId ProfileIdCache::id(Profiler& profiler)
{
    // Acquire pairs with the release store below so that seeing the owner
    // guarantees seeing the id that belongs to it.
    if (m_owner.load(std::memory_order_acquire) == &profiler) {
        return m_id.load(std::memory_order_relaxed);
    }

    const ProfileId resolved = profiler.scopeId(m_name);
    m_id.store(resolved, std::memory_order_relaxed);
    m_owner.store(&profiler, std::memory_order_release);
    return resolved;
}

}  // namespace voxl
