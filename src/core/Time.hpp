#pragma once

// Frame timing, the fixed-timestep accumulator and the scoped CPU timer.
//
// One FrameClock lives in the application and is ticked exactly once per frame.
// Everything else in the engine reads deltas from it rather than calling the
// clock itself, so a paused or time-scaled game is a single change here instead
// of a hunt through every subsystem.
//
// Thread safety: none of these types are thread safe. FrameClock is main-thread
// only. TimingSample/ScopedCpuTimer may be used on a worker as long as each
// sample instance belongs to one thread.

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace voxl {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// Seconds as a double. Accumulating a float over an hour of play loses enough
/// precision to visibly quantise animation, so wall-clock totals stay double
/// and only per-frame deltas are handed out as float.
[[nodiscard]] inline double toSeconds(Clock::duration duration) noexcept
{
    return std::chrono::duration<double>(duration).count();
}

[[nodiscard]] inline float toMilliseconds(Clock::duration duration) noexcept
{
    return std::chrono::duration<float, std::milli>(duration).count();
}

// ---------------------------------------------------------------- stopwatch --

/// Restartable elapsed-time measurement. No smoothing, no bookkeeping.
class Stopwatch {
public:
    Stopwatch() noexcept : m_start(Clock::now()) {}

    void restart() noexcept { m_start = Clock::now(); }

    [[nodiscard]] Clock::duration elapsed() const noexcept { return Clock::now() - m_start; }
    [[nodiscard]] double elapsedSeconds() const noexcept { return toSeconds(elapsed()); }
    [[nodiscard]] float  elapsedMilliseconds() const noexcept { return toMilliseconds(elapsed()); }

private:
    TimePoint m_start;
};

// ------------------------------------------------------------- smoothing --

/// Exponential moving average.
///
/// A ring buffer of the last N frames would be more "correct", but the overlay
/// only needs a number that stops flickering, and an EMA does that in 8 bytes
/// with no branch.
class SmoothedValue {
public:
    /// `smoothing` is the weight given to each new sample, in (0, 1]. 0.1 settles
    /// in roughly 20 frames, which reads as instant to a human but hides the
    /// single-frame spikes that make an FPS counter unreadable.
    explicit SmoothedValue(float smoothing = 0.1f) noexcept
        : m_smoothing(std::clamp(smoothing, 0.001f, 1.0f))
    {
    }

    void add(float sample) noexcept
    {
        if (!m_primed) {
            m_value  = sample;
            m_primed = true;
            return;
        }
        m_value += (sample - m_value) * m_smoothing;
    }

    [[nodiscard]] float value() const noexcept { return m_value; }
    void reset() noexcept
    {
        m_value  = 0.0f;
        m_primed = false;
    }

private:
    float m_smoothing;
    float m_value  = 0.0f;
    bool  m_primed = false;
};

// ---------------------------------------------------------- profiling ------

/// One named timing slot in the debug overlay: last, smoothed, min and max in
/// milliseconds. Owned by whatever subsystem is being measured.
class TimingSample {
public:
    TimingSample() = default;
    explicit TimingSample(float smoothing) noexcept : m_average(smoothing) {}

    void add(float milliseconds) noexcept
    {
        m_last = milliseconds;
        m_average.add(milliseconds);
        m_min = m_count == 0 ? milliseconds : std::min(m_min, milliseconds);
        m_max = m_count == 0 ? milliseconds : std::max(m_max, milliseconds);
        ++m_count;
    }

    [[nodiscard]] float lastMs() const noexcept { return m_last; }
    [[nodiscard]] float averageMs() const noexcept { return m_average.value(); }
    [[nodiscard]] float minMs() const noexcept { return m_count == 0 ? 0.0f : m_min; }
    [[nodiscard]] float maxMs() const noexcept { return m_count == 0 ? 0.0f : m_max; }
    [[nodiscard]] std::uint64_t count() const noexcept { return m_count; }

    /// Clears min/max but keeps the smoothed value, so the overlay's "reset
    /// peaks" button does not blank the whole row.
    void resetExtremes() noexcept { m_count = 0; }

private:
    SmoothedValue m_average{0.1f};
    float         m_last  = 0.0f;
    float         m_min   = 0.0f;
    float         m_max   = 0.0f;
    std::uint64_t m_count = 0;
};

/// Times its enclosing scope and reports on destruction.
///
/// CPU wall time only. GPU work is asynchronous, so wrapping draw calls in this
/// measures how long it took to *submit* them, not how long they took to run;
/// use GL timer queries for that.
class ScopedCpuTimer {
public:
    explicit ScopedCpuTimer(TimingSample& target) noexcept : m_sample(&target) {}
    explicit ScopedCpuTimer(float& outMilliseconds) noexcept : m_out(&outMilliseconds) {}

    ~ScopedCpuTimer()
    {
        const float elapsed = m_watch.elapsedMilliseconds();
        if (m_sample != nullptr) {
            m_sample->add(elapsed);
        }
        if (m_out != nullptr) {
            *m_out = elapsed;
        }
    }

    ScopedCpuTimer(const ScopedCpuTimer&)            = delete;
    ScopedCpuTimer& operator=(const ScopedCpuTimer&) = delete;
    ScopedCpuTimer(ScopedCpuTimer&&)                 = delete;
    ScopedCpuTimer& operator=(ScopedCpuTimer&&)      = delete;

private:
    Stopwatch     m_watch;
    TimingSample* m_sample = nullptr;
    float*        m_out    = nullptr;
};

#define VOXL_TIME_SCOPE_CONCAT_INNER(a, b) a##b
#define VOXL_TIME_SCOPE_CONCAT(a, b) VOXL_TIME_SCOPE_CONCAT_INNER(a, b)

/// `VOXL_TIME_SCOPE(m_meshTiming);` - target is a TimingSample or a float.
#define VOXL_TIME_SCOPE(target) \
    ::voxl::ScopedCpuTimer VOXL_TIME_SCOPE_CONCAT(voxlScopedTimer_, __LINE__)(target)

// ------------------------------------------------------------ frame clock --

/// Per-frame time source and fixed-timestep driver.
///
/// Usage:
///     clock.tick();
///     while (clock.nextFixedStep()) { physics.step(clock.fixedDeltaSeconds()); }
///     render(clock.deltaSeconds(), clock.fixedAlpha());
class FrameClock {
public:
    /// Defaults: 60 Hz physics, a 250 ms delta clamp, and at most 5 catch-up
    /// steps per frame.
    explicit FrameClock(float fixedTimestep = 1.0f / 60.0f) noexcept
        : m_fixedTimestep(fixedTimestep), m_start(Clock::now()), m_lastTick(m_start)
    {
    }

    /// Samples the clock. Call exactly once, at the very top of the frame.
    void tick() noexcept
    {
        const TimePoint now = Clock::now();
        m_rawDelta          = static_cast<float>(toSeconds(now - m_lastTick));
        m_lastTick          = now;

        // A blocked frame (window drag, shader compile, breakpoint) produces a
        // multi-second delta. Feeding that to physics teleports the player
        // through the world, so the simulation sees a clamped value while the
        // raw one stays available for diagnostics.
        m_delta = std::min(m_rawDelta, kMaxDeltaSeconds) * m_timeScale;

        m_totalSeconds += static_cast<double>(m_delta);
        ++m_frameIndex;
        m_frameTime.add(m_rawDelta * 1000.0f);

        m_accumulator += m_delta;
        m_stepsThisFrame = 0;
    }

    /// Consumes one fixed step. Loop on it; it returns false when the frame's
    /// accumulated time is used up or the catch-up cap is hit.
    ///
    /// The cap prevents the death spiral where each frame is slower than the
    /// physics it must catch up on, so the accumulator grows without bound.
    /// When it trips we deliberately drop simulated time rather than freeze.
    [[nodiscard]] bool nextFixedStep() noexcept
    {
        if (m_accumulator < m_fixedTimestep) {
            return false;
        }
        if (m_stepsThisFrame >= kMaxFixedStepsPerFrame) {
            m_accumulator = 0.0f;
            return false;
        }
        m_accumulator -= m_fixedTimestep;
        ++m_stepsThisFrame;
        return true;
    }

    /// Fraction of a fixed step already elapsed, in [0, 1). Render code uses it
    /// to interpolate between the previous and current physics state.
    [[nodiscard]] float fixedAlpha() const noexcept { return m_accumulator / m_fixedTimestep; }

    [[nodiscard]] float fixedDeltaSeconds() const noexcept { return m_fixedTimestep; }
    void setFixedTimestep(float seconds) noexcept
    {
        m_fixedTimestep = std::max(seconds, 1.0f / 1000.0f);
    }

    /// Clamped, time-scaled delta. This is what gameplay should use.
    [[nodiscard]] float deltaSeconds() const noexcept { return m_delta; }
    /// Unclamped, unscaled delta. Diagnostics only.
    [[nodiscard]] float rawDeltaSeconds() const noexcept { return m_rawDelta; }

    [[nodiscard]] float smoothedFrameMs() const noexcept { return m_frameTime.averageMs(); }
    [[nodiscard]] float lastFrameMs() const noexcept { return m_frameTime.lastMs(); }
    [[nodiscard]] const TimingSample& frameTime() const noexcept { return m_frameTime; }

    [[nodiscard]] float fps() const noexcept
    {
        const float ms = m_frameTime.averageMs();
        return ms > 0.0f ? 1000.0f / ms : 0.0f;
    }

    /// Scaled simulation time since construction. Pauses when timeScale is 0.
    [[nodiscard]] double totalSeconds() const noexcept { return m_totalSeconds; }
    /// Unscaled wall time since construction; used for the day/night debug UI
    /// and for anything that must keep running while paused.
    [[nodiscard]] double wallSeconds() const noexcept { return toSeconds(Clock::now() - m_start); }

    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return m_frameIndex; }

    /// 0 pauses the simulation while the renderer keeps drawing.
    void setTimeScale(float scale) noexcept { m_timeScale = std::max(scale, 0.0f); }
    [[nodiscard]] float timeScale() const noexcept { return m_timeScale; }

    /// Discards the accumulated delta. Call after a long blocking operation
    /// (world load, level switch) so the next tick does not report it.
    void resetAfterStall() noexcept
    {
        m_lastTick       = Clock::now();
        m_accumulator    = 0.0f;
        m_rawDelta       = 0.0f;
        m_delta          = 0.0f;
        m_stepsThisFrame = 0;
    }

    static constexpr float       kMaxDeltaSeconds        = 0.25f;
    static constexpr std::size_t kMaxFixedStepsPerFrame  = 5;

private:
    float m_fixedTimestep;

    TimePoint m_start;
    TimePoint m_lastTick;

    float  m_rawDelta       = 0.0f;
    float  m_delta          = 0.0f;
    float  m_accumulator    = 0.0f;
    float  m_timeScale      = 1.0f;
    double m_totalSeconds   = 0.0;
    std::uint64_t m_frameIndex = 0;
    std::size_t   m_stepsThisFrame = 0;

    TimingSample m_frameTime{0.05f};
};

}  // namespace voxl
