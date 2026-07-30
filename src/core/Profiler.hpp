#pragma once

// Named per-frame CPU scope timers for the debug overlay.
//
// NOT part of the frozen contract phase. Time.hpp already provides a
// TimingSample that a subsystem can own by hand, but the overlay needs to
// *enumerate* scopes it does not know about at compile time ("Mesh.Greedy",
// "World.Stream", "Light.Propagate") and show a table of them. That is the only
// thing this file adds.
//
// There is deliberately no global/singleton Profiler. The application owns one
// and passes a reference to whatever it measures. That keeps the engine free of
// global mutable state, lets a unit test use an isolated instance, and means two
// worlds cannot pollute each other's timings.
//
// Cost model: entering a scope is a Stopwatch construction (one clock read);
// leaving it is a clock read plus two relaxed atomic fetch_adds on a cache-line
// isolated slot. There is no lock and no allocation on the timed path, which is
// what makes it safe to use inside a meshing job.
//
// Thread safety: `scopeId`, `addSample`, `addSampleMs`, `enabled`, `setEnabled`
// and `scopeCount` are safe from any thread - workers time their own scopes.
// `endFrame`, `snapshot`, `scopeStats`, `scopeName`, `frameIndex`,
// `frameTotalMs`, `reset` and `resetExtremes` are MAIN THREAD ONLY: the
// per-frame history they touch is deliberately unsynchronised so that reading it
// for the overlay costs nothing.

#include "core/Time.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace voxl {

/// Index of an interned scope. Resolve once with `Profiler::scopeId` and keep
/// it; the id is stable for the lifetime of the Profiler.
using ProfileId = std::uint16_t;

/// Returned when the profiler has run out of slots. Sampling with it is a no-op,
/// so a call site never has to check.
inline constexpr ProfileId kInvalidProfileId = static_cast<ProfileId>(0xFFFFu);

/// One row of the overlay's timing table.
struct ProfileScopeStats {
    /// Points at the caller-owned literal handed to `scopeId`; empty for an
    /// invalid id.
    std::string_view name;
    /// Total time spent inside this scope during the last completed frame. This
    /// is a sum, not a max: 200 chunk meshes report their combined cost, which
    /// is the number that explains a frame spike.
    float lastMs = 0.0f;
    /// Smoothed / extreme values of `lastMs` across frames. Frames in which the
    /// scope was never entered are skipped rather than recorded as 0ms.
    float averageMs = 0.0f;
    float minMs     = 0.0f;
    float maxMs     = 0.0f;
    /// Times the scope was entered during the last completed frame.
    std::uint32_t lastCalls = 0;
    /// Times the scope has been entered since construction (or `reset`).
    std::uint64_t totalCalls = 0;
};

/// Registry of named CPU scopes, accumulated per frame.
///
/// Slots are fixed at construction so the timed path never allocates and an id
/// never dangles. 64 is generous: the whole engine is expected to use on the
/// order of 20 scopes, and a caller that needs a per-chunk breakdown wants a
/// different tool anyway.
class Profiler {
public:
    static constexpr std::size_t kMaxScopes = 64;

    Profiler() = default;

    // Pinned: workers hold raw references to a Profiler and the accumulator
    // array contains atomics, so neither copying nor moving is meaningful.
    Profiler(const Profiler&)            = delete;
    Profiler& operator=(const Profiler&) = delete;
    Profiler(Profiler&&)                 = delete;
    Profiler& operator=(Profiler&&)      = delete;

    /// Interns `name` and returns its stable id, or `kInvalidProfileId` when all
    /// slots are taken (logged once). Idempotent: the same name always maps to
    /// the same id.
    ///
    /// `name` is stored as a view, so it MUST outlive the Profiler - pass a
    /// string literal. Safe from any thread but takes a lock, so resolve ids
    /// during setup rather than inside a job.
    [[nodiscard]] ProfileId scopeId(std::string_view name);

    /// Adds one measurement to `id`'s current-frame accumulator. Any thread.
    /// A `kInvalidProfileId` or an out-of-range id is silently ignored.
    void addSample(ProfileId id, Clock::duration elapsed) noexcept;
    void addSampleMs(ProfileId id, float milliseconds) noexcept;

    /// Folds this frame's accumulators into the history and zeroes them.
    /// MAIN THREAD ONLY, once per frame, after the frame's work has been
    /// handed back. Samples that land after this point are simply attributed to
    /// the next frame; nothing is lost.
    void endFrame() noexcept;

    /// Turning the profiler off makes `addSample` an early return. The scopes
    /// keep their ids and their history, so toggling it does not renumber the
    /// overlay rows out from under the user.
    void setEnabled(bool on) noexcept { m_enabled.store(on, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    [[nodiscard]] std::size_t scopeCount() const noexcept
    {
        return m_count.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::string_view scopeName(ProfileId id) const;
    [[nodiscard]] ProfileScopeStats scopeStats(ProfileId id) const;

    /// Fills `out` with one row per interned scope, in id order. Takes the
    /// buffer by reference so the overlay can keep it across frames and not
    /// allocate. MAIN THREAD ONLY.
    void snapshot(std::vector<ProfileScopeStats>& out) const;
    [[nodiscard]] std::vector<ProfileScopeStats> snapshot() const;

    /// Clears min/max but keeps the smoothed values - the overlay's "reset
    /// peaks" action.
    void resetExtremes() noexcept;

    /// Clears all history and accumulators. Interned names and ids survive, so
    /// call sites holding an id stay valid.
    void reset() noexcept;

    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return m_frameIndex; }

    /// Sum of every scope's `lastMs`. Nested scopes are counted by every level
    /// that encloses them, so this is an "attention" figure for the overlay, not
    /// the frame's exclusive CPU time - do not compare it against
    /// FrameClock::lastFrameMs() and conclude anything.
    [[nodiscard]] float frameTotalMs() const noexcept { return m_frameTotalMs; }

private:
    // C4324 (padding due to alignas) is the entire point: two workers timing
    // different scopes must not write to the same cache line, or the atomic
    // increments serialise on coherence traffic instead of being free.
#pragma warning(push)
#pragma warning(disable : 4324)
    struct alignas(64) Accumulator {
        std::atomic<std::int64_t>  nanos{0};
        std::atomic<std::uint32_t> calls{0};
    };
#pragma warning(pop)

    /// Main-thread-only history. Not padded: only one thread ever touches it.
    struct History {
        TimingSample  sample;
        float         lastMs     = 0.0f;
        std::uint32_t lastCalls  = 0;
        std::uint64_t totalCalls = 0;
    };

    [[nodiscard]] bool valid(ProfileId id) const noexcept
    {
        return id != kInvalidProfileId && static_cast<std::size_t>(id) < scopeCount();
    }

    std::array<Accumulator, kMaxScopes> m_accumulators;
    std::array<History, kMaxScopes>     m_history;

    /// Guards `m_names` only. Interning happens during setup; the overlay reads
    /// names once per frame. Neither is hot, so a plain mutex is correct and the
    /// timed path never touches it.
    mutable std::mutex                       m_nameMutex;
    std::array<std::string_view, kMaxScopes> m_names;
    /// Published with release after the name is written, so a reader that sees
    /// the new count also sees the name.
    std::atomic<std::size_t> m_count{0};

    std::atomic<bool> m_enabled{true};
    std::uint64_t     m_frameIndex   = 0;
    float             m_frameTotalMs = 0.0f;
    /// Guarded by m_nameMutex; keeps the "profiler is full" warning from being
    /// emitted once per frame forever.
    bool m_warnedFull = false;
};

/// Times its enclosing scope into a Profiler slot.
///
/// Unlike ScopedCpuTimer this accumulates rather than overwrites, so a scope
/// entered many times per frame reports its total. Re-entering the same scope
/// recursively double-counts the inner time; name recursive scopes accordingly.
class ProfileScope {
public:
    ProfileScope(Profiler& profiler, ProfileId id) noexcept : m_profiler(&profiler), m_id(id) {}

    ~ProfileScope() { m_profiler->addSample(m_id, m_watch.elapsed()); }

    ProfileScope(const ProfileScope&)            = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;
    ProfileScope(ProfileScope&&)                 = delete;
    ProfileScope& operator=(ProfileScope&&)      = delete;

private:
    Stopwatch m_watch;
    Profiler* m_profiler;
    ProfileId m_id;
};

/// Memoises the id of a literal scope name so the timed path does no interning.
///
/// This exists only to back VOXL_PROFILE_SCOPE, where it lives in a
/// function-local `static`. The constructor is constexpr so that static is
/// constant-initialised and MSVC emits no thread-safe-init guard. It caches the
/// owning Profiler alongside the id and re-interns if asked about a different
/// one, which is what makes the macro safe in tests that build their own
/// Profiler.
class ProfileIdCache {
public:
    constexpr explicit ProfileIdCache(std::string_view name) noexcept : m_name(name) {}

    ProfileIdCache(const ProfileIdCache&)            = delete;
    ProfileIdCache& operator=(const ProfileIdCache&) = delete;

    /// Two threads racing here both call `scopeId`, which is idempotent, so they
    /// store the same value; the race is benign by construction.
    ///
    /// Not noexcept: the first call for a given Profiler falls through to
    /// `scopeId`, which takes a mutex.
    [[nodiscard]] ProfileId id(Profiler& profiler);

private:
    std::string_view             m_name;
    std::atomic<const Profiler*> m_owner{nullptr};
    std::atomic<ProfileId>       m_id{kInvalidProfileId};
};

}  // namespace voxl

#define VOXL_PROFILE_CONCAT_INNER(a, b) a##b
#define VOXL_PROFILE_CONCAT(a, b) VOXL_PROFILE_CONCAT_INNER(a, b)

/// Preferred form: the caller already holds the id (resolved in its constructor),
/// so there is no per-entry lookup at all.
///     VOXL_PROFILE_SCOPE_ID(m_profiler, m_meshScope);
#define VOXL_PROFILE_SCOPE_ID(profiler, id) \
    ::voxl::ProfileScope VOXL_PROFILE_CONCAT(voxlProfileScope_, __LINE__)((profiler), (id))

/// Convenience form for one-off sites:
///     VOXL_PROFILE_SCOPE(profiler, "World.Stream");
/// `name` must be a string literal. Costs one relaxed atomic load per entry
/// after the first.
#define VOXL_PROFILE_SCOPE(profiler, name)                                                 \
    static ::voxl::ProfileIdCache VOXL_PROFILE_CONCAT(voxlProfileCache_, __LINE__)(name);   \
    ::voxl::ProfileScope VOXL_PROFILE_CONCAT(voxlProfileScope_, __LINE__)(                 \
        (profiler), VOXL_PROFILE_CONCAT(voxlProfileCache_, __LINE__).id(profiler))
