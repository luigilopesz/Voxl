#pragma once

// Timing framework for the offline benchmark harness.
//
// WHY A FRAMEWORK AND NOT ONE steady_clock PAIR PER SCENE
// ------------------------------------------------------
// Every workload measured here - terrain noise, greedy meshing, palette packing -
// is noisy on a laptop: turbo ramps, other processes, the first touch of a page,
// a cold branch predictor. A single timing of such a workload is not a
// measurement, it is one sample of a heavy-tailed distribution, and comparing two
// of them between commits produces confident nonsense in both directions.
//
// So every case runs `warmupRuns` untimed runs (which is what pays for page
// faults, branch training and the CPU reaching a steady clock) and then
// `sampleRuns` timed runs, and the report carries the whole shape: min, median,
// mean, p95, max, stddev. The median is the number to compare between commits -
// it ignores the one run that collided with a background task - while the gap
// between min and p95 tells you whether the median means anything at all.
//
// Cases also declare how many logical operations one run performs, so a case that
// meshes 256 chunks per run and a case that meshes one both report a comparable
// per-operation cost.
//
// NOTHING HERE TOUCHES OpenGL. The harness must run on a machine with no GPU and
// no window system, which is also what makes it usable from CI.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace voxl::bench {

// ------------------------------------------------------------- statistics --

/// Shape of one case's per-run wall times, all in milliseconds.
struct Stats {
    double      min    = 0.0;
    double      median = 0.0;
    double      mean   = 0.0;
    double      p95    = 0.0;
    double      max    = 0.0;
    double      stddev = 0.0;
    std::size_t runs   = 0;
};

/// Sorts a copy of `runsMs` and reduces it to the six numbers above.
///
/// p95 uses the nearest-rank definition (index `ceil(0.95 * n) - 1`) rather than
/// interpolation: with the 15-30 samples a benchmark run can afford, an
/// interpolated percentile invents precision that is not in the data, and the
/// nearest-rank value is always one of the times that was actually observed.
[[nodiscard]] Stats summarise(std::vector<double> runsMs);

// ------------------------------------------------------------- case output --

/// A number a case wants reported next to its timing: quads emitted, merge
/// ratio, bytes resident. These are properties of the workload, not of the
/// clock, so they are recorded once rather than sampled.
struct Counter {
    std::string name;
    double      value = 0.0;
    std::string unit;
};

/// Handed to a case's setup/prepare/body so it can attach counters and notes.
///
/// A counter written more than once keeps the last value. Case bodies run many
/// times and would otherwise accumulate one duplicate row per run.
class CaseContext {
public:
    void counter(std::string_view name, double value, std::string_view unit = "");

    /// Appends to the case's note, separated by "; ", ignoring text that is
    /// already there. A case body runs many times and would otherwise repeat
    /// itself once per run.
    void note(std::string_view text);

    [[nodiscard]] const std::vector<Counter>& counters() const noexcept { return m_counters; }
    [[nodiscard]] const std::string& noteText() const noexcept { return m_note; }

private:
    std::vector<Counter> m_counters;
    std::string          m_note;
};

// ---------------------------------------------------------------- the case --

/// One measured scenario.
///
/// The three callbacks exist because not all of a scenario's work is the part
/// being measured:
///   * `setup`   runs once, before the warm-up. Build the world, generate the
///               fixture chunks, resolve biome sites here.
///   * `prepare` runs before every warm-up and timed run, untimed. Use it to
///               restore mutable state a run destroys (a ChunkStorage that the
///               body writes into, a SubVoxelStore the body fills).
///   * `body`    is the timed unit. It must perform exactly `opsPerRun` logical
///               operations, every time.
struct Case {
    std::string group;
    std::string name;

    /// What one logical operation is: "chunk", "voxel", "damaged block". Only
    /// used to label the per-operation column.
    std::string unit = "op";

    /// Logical operations performed by a single `body()` call. Must be >= 1.
    double opsPerRun = 1.0;

    int warmupRuns = 3;
    int sampleRuns = 15;

    std::function<void(CaseContext&)> setup;
    std::function<void(CaseContext&)> prepare;
    std::function<void(CaseContext&)> body;
};

/// A case's timing plus everything it reported.
struct CaseResult {
    std::string group;
    std::string name;
    std::string unit;
    double      opsPerRun = 1.0;

    Stats                stats;
    std::vector<Counter> counters;
    std::string          note;

    /// Set when the case could not be measured at all - a dependency that is not
    /// in the tree yet, a fixture the generator refused to produce. `note` says
    /// why. An unavailable case is still reported, loudly, because silently
    /// dropping it is how a missing number turns into an invented one.
    bool unavailable = false;

    [[nodiscard]] double perOpMicroseconds() const noexcept;
    [[nodiscard]] double opsPerSecond() const noexcept;
};

/// A number computed from two or more case results rather than timed directly -
/// a LOD cost ratio, a job-system speedup. Kept separate from CaseResult so it
/// is obvious in the CSV which rows were measured and which were derived.
struct Derived {
    std::string group;
    std::string name;
    std::string metric;
    double      value = 0.0;
    std::string unit;
    /// Free text appended to the table row; used to flag a ratio that failed the
    /// expectation the case was written to check.
    std::string verdict;
};

// -------------------------------------------------------------- the runner --

struct RunnerConfig {
    /// Substring filter on "group/name". Empty runs everything.
    std::string filter;
    /// Overrides every case's own counts when > 0. -1 means "use the case's".
    int warmupOverride = -1;
    int sampleOverride = -1;
    /// Print each case's timing as it finishes rather than only at the end.
    bool verbose = true;
};

class Runner {
public:
    explicit Runner(RunnerConfig config) : m_config(std::move(config)) {}

    void add(Case testCase);

    /// Records a case that exists on paper but cannot be measured in this build.
    void addUnavailable(std::string group, std::string name, std::string reason);

    /// Records a number computed from results that have already been produced.
    void addDerived(Derived derived);

    /// True when the case would survive the filter. Lets an expensive setup be
    /// skipped entirely rather than run and thrown away.
    [[nodiscard]] bool selected(std::string_view group, std::string_view name) const;

    void runAll();

    [[nodiscard]] const std::vector<CaseResult>& results() const noexcept { return m_results; }
    [[nodiscard]] const CaseResult* find(std::string_view group, std::string_view name) const;

    /// Prints "group/name" for every registered case that survives the filter.
    void listCases(std::ostream& out) const;

    void printTable(std::ostream& out) const;

    /// Long-format CSV: one row per (case, metric). Long rather than wide so
    /// that adding a counter to one case does not shift every other case's
    /// columns, which is what makes a plain `diff` between two commits readable.
    bool writeCsv(const std::filesystem::path& path) const;

private:
    /// Remembers a group the first time anything in it is registered, so the
    /// report can be ordered by group even though unavailable cases are recorded
    /// during registration and measured ones only after they run.
    void noteGroup(const std::string& group);

    RunnerConfig             m_config;
    std::vector<Case>        m_cases;
    std::vector<CaseResult>  m_results;
    std::vector<Derived>     m_derived;
    std::vector<std::string> m_groupOrder;
};

/// Below this, a median is dominated by clock granularity and by whatever the
/// optimiser managed to hoist out of the loop, and the harness says so in the
/// case's note rather than letting the number be read as precise.
inline constexpr double kUnreliableMedianMs = 0.005;

// ------------------------------------------------------ optimiser barriers --

namespace detail {
/// Written by keep(); volatile so the store cannot be elided, which is what
/// stops the optimiser deleting a whole measured loop whose result is unused.
extern volatile std::uint64_t g_sink;
}  // namespace detail

inline void keep(std::uint64_t value) noexcept
{
    detail::g_sink = detail::g_sink + value;
}

inline void keep(double value) noexcept
{
    detail::g_sink = detail::g_sink + static_cast<std::uint64_t>(value);
}

}  // namespace voxl::bench
