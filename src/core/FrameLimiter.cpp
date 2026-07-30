#include "core/FrameLimiter.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#ifdef _WIN32
    #include <windows.h>
    // Present since the Windows 10 1803 SDK. Defined defensively so an older
    // SDK still compiles - CreateWaitableTimerExW simply fails at runtime with
    // an invalid parameter and the fallback path takes over.
    #ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
        #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
    #endif
#endif

namespace voxl {
namespace {

/// Seeds the adaptive estimate so the very first frame is already roughly
/// paced. High-resolution timers land within a few hundred microseconds;
/// `sleep_for` on a default-resolution Windows timer is far worse, so the two
/// paths start from different guesses and converge from there.
constexpr double kSeedOvershootHighResSeconds = 0.0004;  // 0.4 ms
constexpr double kSeedOvershootFallbackSeconds = 0.0016; // 1.6 ms

/// Hint to the core that this is a spin-wait: on x86 it is `pause`, which drops
/// the hyperthread's issue rate and cuts the power the spin burns by an order
/// of magnitude. Plain `std::this_thread::yield()` is NOT a substitute here - it
/// is a syscall that can hand the core to another thread for a full quantum,
/// which is precisely the overshoot the spin phase exists to avoid.
inline void spinPause() noexcept
{
#ifdef _WIN32
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
}

}  // namespace

FrameLimiter::FrameLimiter(float targetFps) noexcept
{
#ifdef _WIN32
    // No timeBeginPeriod(). Raising the system timer resolution to 1 ms is a
    // process-wide, machine-wide side effect that costs battery on every other
    // process too - the opposite of what a limiter added to save power should
    // do. The high-resolution waitable timer gives the same accuracy to this
    // one wait only.
    m_timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (m_timer == nullptr) {
        VOXL_LOG_WARN(
            "FrameLimiter: no high-resolution waitable timer; falling back to sleep_for, which "
            "will spin more");
    }
#endif

    m_overshootMean  = highResolutionSleep() ? kSeedOvershootHighResSeconds
                                             : kSeedOvershootFallbackSeconds;
    m_overshootM2    = 0.0;
    m_overshootCount = 1;

    setTargetFps(targetFps);
}

FrameLimiter::~FrameLimiter()
{
#ifdef _WIN32
    if (m_timer != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(m_timer));
    }
#endif
    m_timer = nullptr;
}

void FrameLimiter::setTargetFps(float fps) noexcept
{
    if (!(fps > 0.0f)) {  // written this way so a NaN disables rather than divides
        m_targetFps = 0.0f;
        m_period    = std::chrono::nanoseconds{0};
        m_primed    = false;
        return;
    }

    const float clamped = std::clamp(fps, kMinTargetFps, kMaxTargetFps);
    if (clamped != m_targetFps) {
        // The old deadline belongs to the old schedule; keeping it would make
        // the first frame after the change either sprint or stall by up to a
        // full period.
        m_primed = false;
    }
    m_targetFps = clamped;

    const double periodSeconds = 1.0 / static_cast<double>(clamped);
    m_period = std::chrono::nanoseconds{static_cast<std::int64_t>(periodSeconds * 1e9)};
}

void FrameLimiter::setSpinMargin(std::chrono::microseconds margin) noexcept
{
    m_spinMargin = std::clamp(margin, std::chrono::microseconds{0}, std::chrono::microseconds{5000});
}

void FrameLimiter::reset() noexcept
{
    m_primed = false;
}

void FrameLimiter::resetStats() noexcept
{
    m_stats.frames       = 0;
    m_stats.overruns     = 0;
    m_stats.sleepCalls   = 0;
    m_stats.sleptSeconds = 0.0;
    m_stats.spunSeconds  = 0.0;
    // The overshoot estimate deliberately survives: it describes the machine,
    // not the measurement window, and throwing it away would make the first
    // frames after a stats reset badly paced for no reason.
}

namespace {

/// Seconds as a double from ANY duration type. core/Time.hpp's `toSeconds`
/// takes a `Clock::duration`, and handing it a `std::chrono::nanoseconds` only
/// compiles by luck of the two happening to be the same type on this platform.
template <typename Rep, typename Period>
[[nodiscard]] double durationSeconds(std::chrono::duration<Rep, Period> duration) noexcept
{
    return std::chrono::duration<double>(duration).count();
}

}  // namespace

std::chrono::nanoseconds FrameLimiter::overshootEstimate() const noexcept
{
    const double variance =
        m_overshootCount > 1 ? m_overshootM2 / static_cast<double>(m_overshootCount - 1) : 0.0;
    const double stdDev = variance > 0.0 ? std::sqrt(variance) : 0.0;

    // mean + one standard deviation, not the mean: sleeping until the *average*
    // wake-up point means half of all sleeps land past the deadline, and a
    // missed deadline cannot be spun back.
    const double seconds = m_overshootMean + stdDev;
    const auto   estimate =
        std::chrono::nanoseconds{static_cast<std::int64_t>(std::max(seconds, 0.0) * 1e9)};
    return std::min(estimate, std::chrono::nanoseconds{kMaxOvershootEstimate});
}

void FrameLimiter::recordOvershoot(std::chrono::nanoseconds overshoot) noexcept
{
    const double sample = std::max(durationSeconds(overshoot), 0.0);

    ++m_overshootCount;
    const double delta = sample - m_overshootMean;
    m_overshootMean += delta / static_cast<double>(m_overshootCount);
    m_overshootM2 += delta * (sample - m_overshootMean);

    const double variance =
        m_overshootCount > 1 ? m_overshootM2 / static_cast<double>(m_overshootCount - 1) : 0.0;

    m_stats.overshootLastMs   = static_cast<float>(sample * 1000.0);
    m_stats.overshootMeanMs   = static_cast<float>(m_overshootMean * 1000.0);
    m_stats.overshootStdDevMs = static_cast<float>(std::sqrt(std::max(variance, 0.0)) * 1000.0);
}

void FrameLimiter::sleepOnce(std::chrono::nanoseconds duration) noexcept
{
    if (duration <= std::chrono::nanoseconds{0}) {
        return;
    }

    const TimePoint before = Clock::now();
    bool            slept  = false;

#ifdef _WIN32
    if (m_timer != nullptr) {
        LARGE_INTEGER due{};
        // Negative is relative, in 100 ns units. Round up: a timer that fires
        // fractionally early would be handed straight back to the loop, which
        // would then record a negative overshoot and skew the estimate low.
        const std::int64_t hundredNanos = (duration.count() + 99) / 100;
        due.QuadPart = -hundredNanos;
        if (::SetWaitableTimerEx(static_cast<HANDLE>(m_timer), &due, 0, nullptr, nullptr, nullptr,
                                 0) != 0) {
            ::WaitForSingleObject(static_cast<HANDLE>(m_timer), INFINITE);
            slept = true;
        }
    }
#endif

    if (!slept) {
        std::this_thread::sleep_for(duration);
    }

    const Clock::duration actual = Clock::now() - before;
    ++m_stats.sleepCalls;
    m_stats.sleptSeconds += durationSeconds(actual);
    recordOvershoot(std::chrono::duration_cast<std::chrono::nanoseconds>(actual) - duration);
}

std::chrono::nanoseconds FrameLimiter::wait() noexcept
{
    if (!enabled()) {
        m_primed = false;
        return std::chrono::nanoseconds{0};
    }

    const TimePoint       entry  = Clock::now();
    const Clock::duration period = std::chrono::duration_cast<Clock::duration>(m_period);
    ++m_stats.frames;

    if (!m_primed) {
        m_deadline = entry + period;
        m_primed   = true;
        return std::chrono::nanoseconds{0};
    }

    if (entry >= m_deadline) {
        ++m_stats.overruns;
        // A frame that missed its slot cannot be un-missed. Advancing by one
        // period lets the next frame absorb a small debt (which is what keeps
        // the AVERAGE rate on target through ordinary jitter), but a frame that
        // is more than a whole period late is a stall, and paying that debt back
        // would mean running several frames flat out - visible as a burst of
        // speed right after a hitch. Resynchronise instead.
        const Clock::duration late = entry - m_deadline;
        m_deadline = (late > period) ? entry + period : m_deadline + period;
        return std::chrono::nanoseconds{0};
    }

    // ---- sleep phase: give the core back for everything but the tail ----
    for (;;) {
        const TimePoint now = Clock::now();
        if (now >= m_deadline) {
            break;
        }
        const Clock::duration remaining = m_deadline - now;
        const auto            threshold = overshootEstimate() + m_spinMargin;
        if (remaining <= threshold) {
            break;
        }
        sleepOnce(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - threshold));
    }

    // ---- spin phase: the last fraction of a millisecond ----
    const TimePoint spinStart = Clock::now();
    while (Clock::now() < m_deadline) {
        spinPause();
    }
    const TimePoint end = Clock::now();
    m_stats.spunSeconds += durationSeconds(end - spinStart);

    m_deadline += period;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - entry);
}

}  // namespace voxl
