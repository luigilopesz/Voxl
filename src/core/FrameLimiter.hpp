#pragma once

// Accurate frame pacing.
//
// WHY THIS EXISTS AT ALL
// ----------------------
// `glfwSwapInterval(1)` is a REQUEST. On hybrid-graphics laptops - and under
// several driver control-panel overrides - the WGL swap interval is advisory and
// simply ignored: this machine runs ~1250 fps at radius 8 with vsync switched
// on. The GPU then sits at its power limit producing frames nobody will ever
// see, the fans spin up, and the battery drains. A frame limiter that actually
// holds a rate is the only thing standing between the player and that, so this
// is a correctness feature, not a comfort one.
//
// WHY NOT JUST SLEEP
// ------------------
// Windows' scheduler granularity is the whole problem:
//
//   * A pure `sleep_for(remaining)` overshoots. The default timer resolution is
//     15.6 ms and even a 1 ms request routinely returns 1.4 ms later. At a 60 Hz
//     target (16.67 ms) that is an 8% error, and it is *biased* - sleep is only
//     ever late, never early - so the achieved rate lands well below the
//     requested one and jitters visibly.
//   * A pure spin holds the rate perfectly and burns an entire core doing it,
//     which on a laptop is the same thermal problem in a different costume.
//
// So: sleep for the bulk, spin for the tail. The only interesting question is
// where to put the boundary, and the answer must be measured rather than
// guessed, because it depends on the machine and on what else is running. This
// class keeps a running mean and standard deviation of how much each sleep
// overshoots its request (Welford, so it is numerically stable over hours of
// play) and stops sleeping once the remaining time drops below `mean + stddev +
// spinMargin`. The tail is then spun out with a pause instruction.
//
// On Windows 10 1803 and later the sleep itself is a high-resolution waitable
// timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), which is accurate to well
// under a millisecond without `timeBeginPeriod` - i.e. without raising the
// timer resolution for the whole system, which is a rude thing to do and hurts
// the battery it was meant to save. If the timer cannot be created the class
// falls back to `std::this_thread::sleep_for`; the adaptive estimate then
// simply learns a larger overshoot and spins a little longer.
//
// Thread safety: none. One limiter, on the thread that runs the frame loop.

#include "core/Time.hpp"

#include <chrono>
#include <cstdint>

namespace voxl {

/// What the limiter did, for the debug overlay and for the measurement harness.
struct FrameLimiterStats {
    /// Calls to `wait()` while enabled.
    std::uint64_t frames = 0;
    /// Frames that reached `wait()` already past their deadline, i.e. frames the
    /// limiter did not shape at all. A non-zero rate here means the machine
    /// cannot hold the requested rate and the cap is doing nothing.
    std::uint64_t overruns = 0;
    /// OS sleeps issued. Roughly one per frame when the estimate is good.
    std::uint64_t sleepCalls = 0;

    double sleptSeconds = 0.0;
    double spunSeconds  = 0.0;

    /// Current adaptive estimate of how late a sleep returns, and the two
    /// moments it is built from. Watching this settle is how you tell whether
    /// the split between sleeping and spinning is sane on a given machine.
    float overshootMeanMs   = 0.0f;
    float overshootStdDevMs = 0.0f;
    float overshootLastMs   = 0.0f;

    /// Fraction of the waiting time spent spinning, in [0, 1]. This is the
    /// number that says whether the limiter is saving power: it should be a few
    /// percent, not a half.
    [[nodiscard]] double spinFraction() const noexcept
    {
        const double total = sleptSeconds + spunSeconds;
        return total > 0.0 ? spunSeconds / total : 0.0;
    }
};

/// Holds a target frame rate by sleeping most of the way and spinning the rest.
///
/// Pinned in place: it owns an OS timer handle.
class FrameLimiter {
public:
    /// Below 1 fps the deadline arithmetic stops being meaningful; above 1000
    /// the spin phase costs more than the frame it is pacing.
    static constexpr float kMinTargetFps = 1.0f;
    static constexpr float kMaxTargetFps = 1000.0f;

    /// Hard ceiling on the learned overshoot estimate. Without it, one
    /// pathological sleep (a driver stall, a swapped-out page) would teach the
    /// limiter to spin for tens of milliseconds every frame afterwards - the
    /// exact core-burning behaviour the design exists to avoid.
    static constexpr std::chrono::microseconds kMaxOvershootEstimate{4000};

    /// `targetFps <= 0` constructs a disabled limiter, in which case `wait()` is
    /// a couple of comparisons.
    explicit FrameLimiter(float targetFps = 0.0f) noexcept;
    ~FrameLimiter();

    FrameLimiter(const FrameLimiter&)            = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;
    FrameLimiter(FrameLimiter&&)                 = delete;
    FrameLimiter& operator=(FrameLimiter&&)      = delete;

    /// Clamped into [kMinTargetFps, kMaxTargetFps]; anything <= 0 disables.
    /// Changing the target re-primes the deadline, so a change mid-session does
    /// not make one frame sprint to catch up with the old schedule.
    void setTargetFps(float fps) noexcept;

    [[nodiscard]] float targetFps() const noexcept { return m_targetFps; }
    [[nodiscard]] bool  enabled() const noexcept { return m_targetFps > 0.0f; }
    /// Zero when disabled.
    [[nodiscard]] std::chrono::nanoseconds period() const noexcept { return m_period; }

    /// Extra time, on top of the learned overshoot, that is always spun rather
    /// than slept. Larger is more accurate and less power-efficient.
    void setSpinMargin(std::chrono::microseconds margin) noexcept;
    [[nodiscard]] std::chrono::microseconds spinMargin() const noexcept { return m_spinMargin; }

    /// True when the OS gave us a sub-millisecond timer. False means the
    /// fallback path, which still works but spins more.
    [[nodiscard]] bool highResolutionSleep() const noexcept { return m_timer != nullptr; }

    /// Forgets the current deadline. Call after anything that blocked the frame
    /// loop for a long time - a world load, a shader recompile, resuming from a
    /// minimised window - so the limiter does not try to "catch up" on time that
    /// was never going to be frames.
    void reset() noexcept;

    /// Blocks until this frame's deadline, then advances the deadline by exactly
    /// one period. Call once per frame, last, after the buffer swap.
    ///
    /// Returns the wall time spent waiting - zero when disabled, when this is
    /// the first frame, or when the frame already overran its deadline.
    std::chrono::nanoseconds wait() noexcept;

    [[nodiscard]] const FrameLimiterStats& stats() const noexcept { return m_stats; }
    /// Clears the counters. Does NOT clear the overshoot estimate, which is a
    /// property of the machine rather than of the measurement window.
    void resetStats() noexcept;

private:
    /// One OS sleep of at most `duration`, measured. Feeds the estimate.
    void sleepOnce(std::chrono::nanoseconds duration) noexcept;
    void recordOvershoot(std::chrono::nanoseconds overshoot) noexcept;
    /// mean + stddev, clamped to kMaxOvershootEstimate.
    [[nodiscard]] std::chrono::nanoseconds overshootEstimate() const noexcept;

    float                   m_targetFps = 0.0f;
    std::chrono::nanoseconds m_period{0};
    std::chrono::microseconds m_spinMargin{200};

    /// Null when the high-resolution timer could not be created, or off Windows.
    /// Type-erased so this header does not drag <windows.h> into everything that
    /// includes it.
    void* m_timer = nullptr;

    TimePoint m_deadline{};
    bool      m_primed = false;

    // Welford accumulators over observed sleep overshoot, in seconds.
    double        m_overshootMean = 0.0;
    double        m_overshootM2   = 0.0;
    std::uint64_t m_overshootCount = 0;

    FrameLimiterStats m_stats{};
};

}  // namespace voxl
