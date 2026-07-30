// Time.hpp is header-only on purpose: every function on it is a few arithmetic
// operations that sit directly in the frame loop or inside a timed scope, and
// an out-of-line call would be a measurable fraction of what it measures.
//
// This translation unit holds the compile-time checks that the rest of the
// engine's timing assumptions depend on. Several of them are the kind of thing
// that would otherwise only be discovered as "physics behaves differently on
// this machine".

#include "core/Time.hpp"

#include <chrono>
#include <type_traits>

namespace voxl {
namespace {

// ---- clock choice -------------------------------------------------------------
// A non-steady clock can jump backwards (NTP correction, user changing the
// system time), which would hand the fixed-step accumulator a negative delta
// and rewind the simulation. Nothing in the engine may swap this for
// system_clock.
static_assert(std::is_same_v<Clock, std::chrono::steady_clock>,
              "voxl::Clock must be steady_clock; a wall clock can step backwards");
static_assert(Clock::is_steady, "voxl::Clock must be monotonic");

// toSeconds() returns double specifically so an hour-long session does not
// quantise; if the duration's own representation were coarser than a
// microsecond the extra precision would be wasted.
static_assert(std::ratio_less_equal_v<Clock::period, std::micro>,
              "Clock resolution must be at least microseconds for frame timing");

// ---- fixed timestep ------------------------------------------------------------
static_assert(FrameClock::kMaxDeltaSeconds == 0.25f,
              "the stall clamp is part of the documented contract");
static_assert(FrameClock::kMaxFixedStepsPerFrame == 5,
              "the catch-up cap is part of the documented contract");
static_assert(FrameClock::kMaxFixedStepsPerFrame > 0,
              "a zero cap would freeze physics entirely");

// NOTE for the integrator, not an error: at the default 60 Hz the cap allows
// only 5 * 16.7ms = 83ms of simulated time per frame, well under the 250ms
// delta clamp. Any frame slower than ~83ms therefore *discards* simulated time
// (nextFixedStep() zeroes the accumulator) even though the clamp would have
// allowed it. That is the intended anti-death-spiral behaviour, but it means the
// two constants are not independent - raising kMaxDeltaSeconds without raising
// the cap has no effect on physics.
static_assert(FrameClock::kMaxDeltaSeconds >
                  static_cast<float>(FrameClock::kMaxFixedStepsPerFrame) * (1.0f / 60.0f),
              "clamp and catch-up cap are documented as clamp-looser-than-cap");

// ---- scoped timer --------------------------------------------------------------
// ScopedCpuTimer must be pinned: a moved-from timer would report its scope twice
// (once from the moved-from husk with a bogus start point), silently doubling
// whatever the overlay shows.
static_assert(!std::is_copy_constructible_v<ScopedCpuTimer>,
              "ScopedCpuTimer must not be copyable");
static_assert(!std::is_move_constructible_v<ScopedCpuTimer>, "ScopedCpuTimer must not be movable");

// TimingSample and SmoothedValue are copied into overlay snapshots by value, so
// they must stay trivially copyable and small enough that copying a table of
// them per frame is free.
static_assert(std::is_trivially_copyable_v<SmoothedValue>,
              "SmoothedValue is snapshotted by value each frame");
static_assert(std::is_trivially_copyable_v<TimingSample>,
              "TimingSample is snapshotted by value each frame");
static_assert(sizeof(TimingSample) <= 32, "TimingSample grew; the overlay copies these per frame");

// Stopwatch is nothing but a time point; anything larger means someone added
// bookkeeping to the hot path.
static_assert(sizeof(Stopwatch) == sizeof(TimePoint), "Stopwatch must stay a bare time point");
static_assert(std::is_trivially_copyable_v<Stopwatch>);

}  // namespace
}  // namespace voxl
