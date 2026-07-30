#include "Harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <numeric>
#include <ostream>
#include <string>
#include <vector>

namespace voxl::bench {

namespace detail {
volatile std::uint64_t g_sink = 0;
}  // namespace detail

// ------------------------------------------------------------- statistics --

Stats summarise(std::vector<double> runsMs)
{
    Stats stats;
    if (runsMs.empty()) {
        return stats;
    }

    std::sort(runsMs.begin(), runsMs.end());
    const std::size_t n = runsMs.size();

    stats.runs = n;
    stats.min  = runsMs.front();
    stats.max  = runsMs.back();
    stats.mean = std::accumulate(runsMs.begin(), runsMs.end(), 0.0) / static_cast<double>(n);

    stats.median = (n % 2 == 1) ? runsMs[n / 2]
                                : 0.5 * (runsMs[n / 2 - 1] + runsMs[n / 2]);

    // Nearest rank; see the header for why this is not interpolated.
    const auto rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    stats.p95       = runsMs[std::min(n, std::max<std::size_t>(rank, 1)) - 1];

    double variance = 0.0;
    for (const double value : runsMs) {
        const double delta = value - stats.mean;
        variance += delta * delta;
    }
    // Sample variance: with 15 runs the population form is biased low by ~7%,
    // and this number exists precisely to say how much to distrust the median.
    variance /= static_cast<double>(n > 1 ? n - 1 : 1);
    stats.stddev = std::sqrt(variance);

    return stats;
}

// ------------------------------------------------------------ CaseContext --

void CaseContext::counter(std::string_view name, double value, std::string_view unit)
{
    for (Counter& existing : m_counters) {
        if (existing.name == name) {
            existing.value = value;
            existing.unit  = std::string(unit);
            return;
        }
    }
    m_counters.push_back(Counter{std::string(name), value, std::string(unit)});
}

void CaseContext::note(std::string_view text)
{
    // Appends rather than replaces, and de-duplicates: setup runs once but a
    // body may annotate itself on every run, and a case that wants to say two
    // things should not have to concatenate them by hand.
    if (m_note.find(text) != std::string::npos) {
        return;
    }
    if (!m_note.empty()) {
        m_note += "; ";
    }
    m_note += text;
}

// ------------------------------------------------------------- CaseResult --

double CaseResult::perOpMicroseconds() const noexcept
{
    if (opsPerRun <= 0.0) {
        return 0.0;
    }
    return stats.median * 1000.0 / opsPerRun;
}

double CaseResult::opsPerSecond() const noexcept
{
    if (stats.median <= 0.0) {
        return 0.0;
    }
    return opsPerRun / (stats.median / 1000.0);
}

// ----------------------------------------------------------------- Runner --

void Runner::noteGroup(const std::string& group)
{
    if (std::find(m_groupOrder.begin(), m_groupOrder.end(), group) == m_groupOrder.end()) {
        m_groupOrder.push_back(group);
    }
}

void Runner::add(Case testCase)
{
    noteGroup(testCase.group);
    m_cases.push_back(std::move(testCase));
}

void Runner::addUnavailable(std::string group, std::string name, std::string reason)
{
    noteGroup(group);

    CaseResult result;
    result.group       = std::move(group);
    result.name        = std::move(name);
    result.unavailable = true;
    result.note        = std::move(reason);
    m_results.push_back(std::move(result));
}

void Runner::addDerived(Derived derived)
{
    m_derived.push_back(std::move(derived));
}

bool Runner::selected(std::string_view group, std::string_view name) const
{
    if (m_config.filter.empty()) {
        return true;
    }
    const std::string key = std::string(group) + "/" + std::string(name);
    return key.find(m_config.filter) != std::string::npos;
}

const CaseResult* Runner::find(std::string_view group, std::string_view name) const
{
    for (const CaseResult& result : m_results) {
        if (result.group == group && result.name == name) {
            return &result;
        }
    }
    return nullptr;
}

void Runner::runAll()
{
    using Clock = std::chrono::steady_clock;

    for (Case& testCase : m_cases) {
        if (!selected(testCase.group, testCase.name)) {
            continue;
        }

        const int warmup =
            m_config.warmupOverride >= 0 ? m_config.warmupOverride : testCase.warmupRuns;
        const int samples =
            m_config.sampleOverride > 0 ? m_config.sampleOverride : testCase.sampleRuns;

        CaseContext context;
        if (testCase.setup) {
            testCase.setup(context);
        }

        if (!testCase.body) {
            addUnavailable(testCase.group, testCase.name, "case has no body");
            continue;
        }

        for (int i = 0; i < warmup; ++i) {
            if (testCase.prepare) {
                testCase.prepare(context);
            }
            testCase.body(context);
        }

        std::vector<double> runsMs;
        runsMs.reserve(static_cast<std::size_t>(std::max(samples, 1)));
        for (int i = 0; i < samples; ++i) {
            if (testCase.prepare) {
                testCase.prepare(context);
            }
            const Clock::time_point start = Clock::now();
            testCase.body(context);
            const std::chrono::duration<double, std::milli> elapsed = Clock::now() - start;
            runsMs.push_back(elapsed.count());
        }

        CaseResult result;
        result.group     = testCase.group;
        result.name      = testCase.name;
        result.unit      = testCase.unit;
        result.opsPerRun = testCase.opsPerRun;
        result.stats     = summarise(std::move(runsMs));
        result.counters  = context.counters();
        result.note      = context.noteText();

        if (result.stats.median < kUnreliableMedianMs) {
            if (!result.note.empty()) {
                result.note += "; ";
            }
            result.note +=
                "median is below the harness's reliability floor - read it as an upper bound on "
                "a cost that clock granularity and loop hoisting cannot separate from zero";
        }

        if (m_config.verbose) {
            std::fprintf(stdout, "  %-14s %-34s median %10.4f ms  (%.4f us/%s)\n",
                         result.group.c_str(), result.name.c_str(), result.stats.median,
                         result.perOpMicroseconds(), result.unit.c_str());
            std::fflush(stdout);
        }

        m_results.push_back(std::move(result));
    }
}

namespace {

/// Human-readable per-operation cost. Sub-microsecond costs (a palette get) and
/// multi-millisecond ones (a batch of 256 chunks) both have to fit one column.
std::string formatPerOp(double microseconds)
{
    if (microseconds <= 0.0) {
        return "-";
    }
    if (microseconds < 1.0) {
        return std::format("{:.1f} ns", microseconds * 1000.0);
    }
    if (microseconds < 1000.0) {
        return std::format("{:.2f} us", microseconds);
    }
    return std::format("{:.3f} ms", microseconds / 1000.0);
}

std::string formatRate(double perSecond)
{
    if (perSecond <= 0.0) {
        return "-";
    }
    if (perSecond >= 1e6) {
        return std::format("{:.2f} M/s", perSecond / 1e6);
    }
    if (perSecond >= 1e3) {
        return std::format("{:.1f} k/s", perSecond / 1e3);
    }
    return std::format("{:.1f} /s", perSecond);
}

std::string formatCounter(const Counter& counter)
{
    const double magnitude = std::fabs(counter.value);
    const std::string value = (magnitude != 0.0 && magnitude < 0.01)
                                  ? std::format("{:.5f}", counter.value)
                              : (magnitude >= 100000.0) ? std::format("{:.0f}", counter.value)
                                                        : std::format("{:.3f}", counter.value);
    return counter.unit.empty() ? std::format("{} = {}", counter.name, value)
                                : std::format("{} = {} {}", counter.name, value, counter.unit);
}

}  // namespace

void Runner::listCases(std::ostream& out) const
{
    for (const Case& testCase : m_cases) {
        if (selected(testCase.group, testCase.name)) {
            out << testCase.group << "/" << testCase.name << "\n";
        }
    }
    for (const CaseResult& result : m_results) {
        if (result.unavailable && selected(result.group, result.name)) {
            out << result.group << "/" << result.name << "  [unavailable: " << result.note << "]\n";
        }
    }
}

void Runner::printTable(std::ostream& out) const
{
    out << "\n";
    out << "================================================================================"
           "=========================\n";
    out << " BENCHMARK RESULTS\n";
    out << "================================================================================"
           "=========================\n";
    out << std::format("{:<13} {:<32} {:>4} {:>9} {:>9} {:>9} {:>9} {:>9}  {:<11} {:<11}\n",
                       "GROUP", "CASE", "RUN", "MIN ms", "MED ms", "MEAN ms", "P95 ms", "MAX ms",
                       "PER-OP", "THROUGHPUT");
    out << "--------------------------------------------------------------------------------"
           "-------------------------\n";

    // Grouped by registration order rather than by result order: an unavailable
    // case is recorded while its group is being registered and a measured one
    // only after it runs, so plain insertion order would herd every "NOT
    // MEASURED" row to the top of the report, away from its own group.
    bool firstGroup = true;
    for (const std::string& group : m_groupOrder) {
        if (!firstGroup) {
            out << "\n";
        }
        firstGroup = false;

        for (const CaseResult& result : m_results) {
            if (result.group != group) {
                continue;
            }

            if (result.unavailable) {
                out << std::format("{:<13} {:<32} {:>4} {:>59}\n", result.group, result.name, "-",
                                   "NOT MEASURED");
                out << std::format("{:<13} {:<32}   reason: {}\n", "", "", result.note);
                continue;
            }

            out << std::format(
                "{:<13} {:<32} {:>4} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f}  {:<11} {:<11}\n",
                result.group, result.name, result.stats.runs, result.stats.min,
                result.stats.median, result.stats.mean, result.stats.p95, result.stats.max,
                formatPerOp(result.perOpMicroseconds()),
                formatRate(result.opsPerSecond()) + " " + result.unit);

            for (const Counter& counter : result.counters) {
                out << std::format("{:<13} {:<32}   {}\n", "", "", formatCounter(counter));
            }
            if (!result.note.empty()) {
                out << std::format("{:<13} {:<32}   note: {}\n", "", "", result.note);
            }
        }
    }

    if (!m_derived.empty()) {
        out << "\n";
        out << "================================================================================"
               "=========================\n";
        out << " DERIVED METRICS (computed from the rows above, not timed directly)\n";
        out << "================================================================================"
               "=========================\n";
        for (const Derived& derived : m_derived) {
            out << std::format("{:<13} {:<32} {:<26} {:>12.3f} {:<8} {}\n", derived.group,
                               derived.name, derived.metric, derived.value, derived.unit,
                               derived.verdict);
        }
    }
    out << "\n";
}

namespace {

/// RFC-4180 quoting, applied only where a field can contain a comma. Notes and
/// verdicts are free text written by case authors, so they can.
std::string csvField(const std::string& text)
{
    if (text.find_first_of(",\"\n") == std::string::npos) {
        return text;
    }
    std::string quoted = "\"";
    for (const char character : text) {
        if (character == '"') {
            quoted += '"';
        }
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

}  // namespace

bool Runner::writeCsv(const std::filesystem::path& path) const
{
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }

    file << "group,case,kind,metric,value,unit,note\n";
    for (const std::string& group : m_groupOrder) {
        for (const CaseResult& result : m_results) {
            if (result.group != group) {
                continue;
            }

            const std::string prefix = csvField(result.group) + "," + csvField(result.name) + ",";
            if (result.unavailable) {
                file << prefix << "status,unavailable,,," << csvField(result.note) << "\n";
                continue;
            }

            const auto row = [&](const char* metric, double value, const std::string& unit) {
                file << prefix << "timing," << metric << "," << std::format("{:.6f}", value) << ","
                     << csvField(unit) << ",\n";
            };
            row("min_ms", result.stats.min, "ms");
            row("median_ms", result.stats.median, "ms");
            row("mean_ms", result.stats.mean, "ms");
            row("p95_ms", result.stats.p95, "ms");
            row("max_ms", result.stats.max, "ms");
            row("stddev_ms", result.stats.stddev, "ms");
            row("runs", static_cast<double>(result.stats.runs), "count");
            row("ops_per_run", result.opsPerRun, result.unit);
            row("per_op_us", result.perOpMicroseconds(), "us/" + result.unit);
            row("ops_per_sec", result.opsPerSecond(), result.unit + "/s");

            for (const Counter& counter : result.counters) {
                file << prefix << "counter," << csvField(counter.name) << ","
                     << std::format("{:.6f}", counter.value) << "," << csvField(counter.unit)
                     << ",\n";
            }
            if (!result.note.empty()) {
                file << prefix << "note,note,,," << csvField(result.note) << "\n";
            }
        }
    }

    for (const Derived& derived : m_derived) {
        file << csvField(derived.group) << "," << csvField(derived.name) << ",derived,"
             << csvField(derived.metric) << "," << std::format("{:.6f}", derived.value) << ","
             << csvField(derived.unit) << "," << csvField(derived.verdict) << "\n";
    }

    return file.good();
}

}  // namespace voxl::bench
