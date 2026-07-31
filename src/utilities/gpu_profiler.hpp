#pragma once

#include <daxa/daxa.hpp>
#include <cstdint>
#include <string_view>

// ------------------------------------------------------------------------------------------------
// Per-pass GPU timing, off by default, driven entirely by environment variables.
// ------------------------------------------------------------------------------------------------
// WHY ENVIRONMENT VARIABLES AND NOT A CLI FLAG. This is measurement scaffolding, not a feature.
// Several people are editing this tree at once and application/cli.* is a hot file; a profiler
// that needs three files changed to turn on is a profiler that causes merge conflicts. Everything
// here is reachable from `$env:VOXL_GPU_PROFILE = 'out.csv'` and nothing else in the engine has to
// know it exists.
//
//   VOXL_GPU_PROFILE          path to the CSV to write at exit. Unset => the profiler is a no-op
//                             and not a single timestamp is written.
//   VOXL_GPU_PROFILE_START_S  wall-clock seconds (from the first frame) before recording starts.
//                             Default 4. Exists because record_tasks() runs again on the first
//                             resize, and the pass layout must be locked after that, not before.
//   VOXL_GPU_PROFILE_MAX_FRAMES  cap on recorded frames. Default 20000.
//
// HOW IT MEASURES. One daxa::TimelineQueryPool per slot of a 4-deep ring. Every profiled scope
// writes a BOTTOM_OF_PIPE timestamp before and after the task's own commands. BOTTOM_OF_PIPE on
// both ends is deliberate: the task graph records its pipeline barriers *between* task callbacks,
// so a scope's opening timestamp only resolves once the preceding barrier has, and barrier/layout
// -transition time therefore falls in the gap between scopes instead of being charged to a pass.
// The frame's GPU span (last close minus first open) minus the sum of the scopes is that gap, and
// it is reported rather than hidden.
//
// Results are read back three frames late (`end_frame` reads the pool that was written at
// frame - 3) and every read checks the per-query availability bit. A same-frame readback would
// stall the CPU on the GPU and destroy the very number being measured.
//
// COST WHEN ON. Two vkCmdWriteTimestamp per pass. Measured overhead is in the report; it is
// small but not zero, so leave it off for frame-time baselines.

namespace gpu_profiler {
    /// True when VOXL_GPU_PROFILE is set to a non-empty path. Read once, on first call.
    auto enabled() -> bool;

    /// CPU-side frame boundary. Call before anything is submitted for the frame.
    /// `time_s` and `prev_frame_ms` are only carried through to the CSV so a row can be lined up
    /// with the matching row of --bench-csv. `prev_frame_ms` is the previous frame's wall time.
    void begin_frame(daxa::Device device, uint64_t frame_index, float time_s, float prev_frame_ms);

    /// CPU-side frame end. Call after the frame's last submit. Reads back an older pool.
    void end_frame();

    /// Drains the last few frames, closes the CSV and releases the query pools. Must be called
    /// while the device is still alive and idle -- the pools hold a weak reference to it.
    void shutdown();

    /// Opens a scope in `recorder`. Returns a handle for scope_end, or ~0u if not recording.
    auto scope_begin(daxa::CommandRecorder &recorder, std::string_view name) -> uint32_t;
    void scope_end(daxa::CommandRecorder &recorder, uint32_t handle);

    /// RAII form. Cheap enough to instantiate unconditionally: when the profiler is off, the
    /// constructor does one atomic-free bool test and returns.
    struct Scope {
        Scope(daxa::CommandRecorder &recorder, std::string_view name)
            : recorder_{&recorder}, handle_{scope_begin(recorder, name)} {}
        ~Scope() { scope_end(*recorder_, handle_); }
        Scope(Scope const &) = delete;
        Scope(Scope &&) = delete;
        auto operator=(Scope const &) -> Scope & = delete;
        auto operator=(Scope &&) -> Scope & = delete;

      private:
        daxa::CommandRecorder *recorder_;
        uint32_t handle_;
    };
} // namespace gpu_profiler
