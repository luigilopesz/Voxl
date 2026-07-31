#include "gpu_profiler.hpp"

#include <daxa/command_recorder.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    constexpr uint32_t POOL_COUNT = 4;      // ring depth; FRAMES_IN_FLIGHT is 1, so 4 is generous
    constexpr uint32_t MAX_QUERIES = 2048;  // 1024 scopes per frame
    constexpr uint32_t READBACK_LAG = 3;    // read the pool written READBACK_LAG frames ago
    constexpr uint32_t INVALID = ~0u;

    struct FrameSlot {
        uint64_t frame_index = 0;
        float time_s = 0.0f;
        float prev_frame_ms = 0.0f;
        uint32_t query_count = 0;              // number of timestamps written (2 per scope)
        std::vector<uint32_t> name_ids;        // one per scope, in submission order
        bool in_use = false;
    };

    struct Profiler {
        bool initialised = false;
        bool failed = false;
        std::string csv_path;
        float start_s = 4.0f;
        uint64_t max_frames = 20000;

        daxa::Device device;
        double ns_per_tick = 1.0;
        std::array<daxa::TimelineQueryPool, POOL_COUNT> pools{};
        std::array<FrameSlot, POOL_COUNT> slots{};

        // Current frame state.
        uint32_t cur_pool = 0;
        bool recording = false;   // this frame's commands carry timestamps
        bool needs_reset = false; // the pool has not been reset yet this frame

        // Interned pass names. Names come from TaskHead::name(), which is a compile-time constant,
        // but interning by content keeps the CSV header stable even if two heads share a spelling.
        std::vector<std::string> names;
        std::unordered_map<std::string, uint32_t> name_ids;

        // Column layout, locked on the first frame that produces results.
        bool layout_locked = false;
        std::vector<uint32_t> layout; // name id per column
        uint64_t rows_written = 0;
        uint64_t rows_dropped_layout = 0;
        uint64_t rows_dropped_unavailable = 0;
        std::ofstream csv;

        auto intern(std::string_view name) -> uint32_t {
            auto key = std::string{name};
            auto iter = name_ids.find(key);
            if (iter != name_ids.end()) {
                return iter->second;
            }
            auto id = static_cast<uint32_t>(names.size());
            names.push_back(key);
            name_ids.emplace(std::move(key), id);
            return id;
        }
    };

    auto self() -> Profiler & {
        static Profiler instance;
        return instance;
    }

    auto env(char const *name) -> std::string {
        // getenv rather than _dupenv_s: this is read a handful of times at startup and the
        // returned pointer is copied immediately.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        char const *value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        return value == nullptr ? std::string{} : std::string{value};
    }

    void write_header(Profiler &p) {
        // prev_frame_ms is gpu_input.delta_time at the top of the frame, i.e. the *previous*
        // frame's wall time. It is here only so a row can be sanity-checked against the matching
        // --bench-csv row; it is not a second, independently measured frame time.
        p.csv << "frame,t_s,prev_frame_ms,gpu_span_ms,gpu_sum_ms,gpu_gap_ms";
        auto seen = std::unordered_map<uint32_t, uint32_t>{};
        for (auto id : p.layout) {
            auto n = ++seen[id];
            p.csv << ',' << p.names[id];
            if (n > 1) {
                // Several passes share a TaskHead and run more than once a frame (blur mip
                // levels, the two rtdgi spatial passes). Suffix them so the columns stay
                // distinguishable rather than silently merged.
                p.csv << '#' << n;
            }
        }
        p.csv << '\n';
    }

    /// Pulls the results for one ring slot and appends a CSV row. Returns false if the GPU has
    /// not finished with that slot yet, in which case nothing is consumed.
    auto drain_slot(Profiler &p, uint32_t pool_index) -> bool {
        auto &slot = p.slots[pool_index];
        if (!slot.in_use || slot.query_count == 0) {
            return true;
        }
        auto results = p.pools[pool_index].get_query_results(0, slot.query_count);
        // results is {value, availability} pairs.
        for (uint32_t i = 0; i < slot.query_count; ++i) {
            if (results[static_cast<size_t>(i) * 2 + 1] == 0) {
                return false;
            }
        }
        slot.in_use = false;

        auto const scope_count = slot.query_count / 2;
        if (!p.layout_locked) {
            p.layout = slot.name_ids;
            p.layout_locked = true;
            p.csv.open(p.csv_path, std::ios::out | std::ios::trunc);
            if (!p.csv.is_open()) {
                std::cout << "[gpu_profiler] cannot open " << p.csv_path << std::endl;
                p.failed = true;
                return true;
            }
            write_header(p);
        }
        if (slot.name_ids != p.layout) {
            // record_tasks() ran again and changed the graph. Dropping is the honest response:
            // silently re-using the old header would mislabel every column after the change.
            ++p.rows_dropped_layout;
            return true;
        }

        auto first = results[0];
        auto last = results[0];
        double sum_ms = 0.0;
        auto cells = std::vector<double>(scope_count, 0.0);
        for (uint32_t s = 0; s < scope_count; ++s) {
            auto b = results[static_cast<size_t>(s) * 4 + 0];
            auto e = results[static_cast<size_t>(s) * 4 + 2];
            first = std::min(first, b);
            last = std::max(last, e);
            auto ms = static_cast<double>(e - b) * p.ns_per_tick * 1e-6;
            cells[s] = ms;
            sum_ms += ms;
        }
        auto span_ms = static_cast<double>(last - first) * p.ns_per_tick * 1e-6;

        p.csv << slot.frame_index << ',' << slot.time_s << ',' << slot.prev_frame_ms << ','
              << span_ms << ',' << sum_ms << ',' << (span_ms - sum_ms);
        for (auto ms : cells) {
            p.csv << ',' << ms;
        }
        p.csv << '\n';
        ++p.rows_written;
        if ((p.rows_written % 256) == 0) {
            p.csv.flush();
        }
        return true;
    }

    void init(Profiler &p, daxa::Device device) {
        p.initialised = true;
        p.device = device;
        auto const &limits = device.properties().limits;
        p.ns_per_tick = static_cast<double>(limits.timestamp_period);
        if (limits.timestamp_compute_and_graphics == 0) {
            std::cout << "[gpu_profiler] device reports timestampComputeAndGraphics = 0; "
                         "per-pass timings would be unreliable -- profiler disabled"
                      << std::endl;
            p.failed = true;
            return;
        }
        for (uint32_t i = 0; i < POOL_COUNT; ++i) {
            p.pools[i] = device.create_timeline_query_pool({
                .query_count = MAX_QUERIES,
                .name = "gpu_profiler_pool",
            });
        }
        p.csv.precision(4);
        p.csv << std::fixed;
        std::cout << "[gpu_profiler] on: " << p.csv_path
                  << " (timestamp_period " << p.ns_per_tick << " ns, start after "
                  << p.start_s << " s)" << std::endl;
    }
} // namespace

auto gpu_profiler::enabled() -> bool {
    auto &p = self();
    static bool const on = [&] {
        p.csv_path = env("VOXL_GPU_PROFILE");
        if (p.csv_path.empty()) {
            return false;
        }
        auto start = env("VOXL_GPU_PROFILE_START_S");
        if (!start.empty()) {
            p.start_s = std::strtof(start.c_str(), nullptr);
        }
        auto cap = env("VOXL_GPU_PROFILE_MAX_FRAMES");
        if (!cap.empty()) {
            p.max_frames = std::strtoull(cap.c_str(), nullptr, 10);
        }
        return true;
    }();
    return on && !p.failed;
}

void gpu_profiler::begin_frame(daxa::Device device, uint64_t frame_index, float time_s, float prev_frame_ms) {
    if (!enabled()) {
        return;
    }
    auto &p = self();
    if (!p.initialised) {
        init(p, device);
        if (p.failed) {
            return;
        }
    }
    p.recording = time_s >= p.start_s && p.rows_written < p.max_frames;
    if (!p.recording) {
        return;
    }
    p.cur_pool = static_cast<uint32_t>(frame_index % POOL_COUNT);
    auto &slot = p.slots[p.cur_pool];
    if (slot.in_use) {
        // The ring wrapped onto a slot the GPU has not finished with. Only possible if the
        // driver is more than POOL_COUNT frames behind; give up on that slot rather than
        // overwrite queries that are still being written.
        ++p.rows_dropped_unavailable;
    }
    slot.frame_index = frame_index;
    slot.time_s = time_s;
    slot.prev_frame_ms = prev_frame_ms;
    slot.query_count = 0;
    slot.name_ids.clear();
    slot.in_use = true;
    p.needs_reset = true;
}

void gpu_profiler::end_frame() {
    auto &p = self();
    if (!enabled() || !p.initialised || p.failed) {
        return;
    }
    if (!p.recording) {
        return;
    }
    // Read the slot written READBACK_LAG frames ago. Availability is still checked, so a slow
    // frame costs a deferred row rather than a stall.
    auto older = (p.cur_pool + POOL_COUNT - READBACK_LAG) % POOL_COUNT;
    (void)drain_slot(p, older);
}

auto gpu_profiler::scope_begin(daxa::CommandRecorder &recorder, std::string_view name) -> uint32_t {
    auto &p = self();
    if (!enabled() || !p.initialised || p.failed || !p.recording) {
        return INVALID;
    }
    auto &slot = p.slots[p.cur_pool];
    if (slot.query_count + 2 > MAX_QUERIES) {
        return INVALID;
    }
    if (p.needs_reset) {
        // Lazy, because the pool has to be reset before the first write of the frame and the
        // first submit of the frame is the sky graph, which is conditional. Doing it here puts
        // the reset in whichever command list opens the frame. vkCmdResetQueryPool is illegal
        // inside a render pass; every task that can be first here is a compute/transfer task.
        recorder.reset_timestamps({
            .query_pool = p.pools[p.cur_pool],
            .start_index = 0,
            .count = MAX_QUERIES,
        });
        p.needs_reset = false;
    }
    auto index = slot.query_count;
    slot.query_count += 2;
    slot.name_ids.push_back(p.intern(name));
    recorder.write_timestamp({
        .query_pool = p.pools[p.cur_pool],
        .pipeline_stage = daxa::PipelineStageFlagBits::BOTTOM_OF_PIPE,
        .query_index = index,
    });
    return index;
}

void gpu_profiler::scope_end(daxa::CommandRecorder &recorder, uint32_t handle) {
    if (handle == INVALID) {
        return;
    }
    auto &p = self();
    recorder.write_timestamp({
        .query_pool = p.pools[p.cur_pool],
        .pipeline_stage = daxa::PipelineStageFlagBits::BOTTOM_OF_PIPE,
        .query_index = handle + 1,
    });
}

void gpu_profiler::shutdown() {
    auto &p = self();
    if (!p.initialised) {
        return;
    }
    // The device is idle by the time this is called, so every outstanding slot is readable.
    for (uint32_t i = 0; i < POOL_COUNT; ++i) {
        (void)drain_slot(p, i);
    }
    if (p.csv.is_open()) {
        p.csv.flush();
        p.csv.close();
    }
    std::cout << "[gpu_profiler] " << p.rows_written << " frames written to " << p.csv_path
              << "; " << p.rows_dropped_layout << " dropped on a task-graph change, "
              << p.rows_dropped_unavailable << " dropped as unavailable" << std::endl;
    // Released here, not in a static destructor: these hold a weak reference to the device and
    // must not outlive main().
    for (auto &pool : p.pools) {
        pool = {};
    }
    p.device = {};
    p.initialised = false;
    p.failed = true; // no further recording after shutdown
}
