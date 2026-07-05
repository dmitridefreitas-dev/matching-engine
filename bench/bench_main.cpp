// Benchmark harness. Methodology over magic numbers:
//
//   * pinned to one core (SetThreadAffinityMask / sched_setaffinity)
//   * explicit warmup pass, excluded from every statistic
//   * two measurement modes, reported separately:
//       - throughput: one clock around the whole replay (no per-op timing
//         distortion), reported as million ops/second
//       - latency: TSC read around EVERY operation (see lob/tsc.hpp — the
//         100ns granularity of steady_clock on Windows would quantise the
//         percentiles); p50/p90/p99/p99.9/max in nanoseconds. The ~5-10ns
//         cost of the rdtsc pair is INCLUDED and stated in the README.
//   * identical op streams for both engines (same seed / same file)
//
// Output is appended as CSV so the analysis notebook can consume every run:
//   impl,flow,ops,fills,throughput_mops,p50_ns,p90_ns,p99_ns,p999_ns,max_ns

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lob/fast_book.hpp"
#include "lob/flow.hpp"
#include "lob/lobster.hpp"
#include "lob/map_book.hpp"
#include "lob/tsc.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#endif

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

void pin_to_core(unsigned core) {
#if defined(_WIN32)
    SetThreadAffinityMask(GetCurrentThread(), 1ull << core);
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);
#endif
}

struct RunResult {
    std::size_t ops = 0;
    std::size_t fills = 0;
    double throughput_mops = 0.0;
    std::uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

template <typename Book>
Book fresh_book(Price min_price, Price max_price);

template <>
MapBook fresh_book<MapBook>(Price, Price) { return MapBook{}; }

template <>
FastBook fresh_book<FastBook>(Price min_price, Price max_price) {
    return FastBook{min_price, max_price};
}

template <typename Book>
RunResult run(const std::vector<FlowOp>& ops, Price min_price, Price max_price) {
    RunResult result;
    result.ops = ops.size();
    std::vector<Fill> fills;
    fills.reserve(256);

    {  // warmup: full replay on a throwaway book, nothing recorded
        Book warm = fresh_book<Book>(min_price, max_price);
        for (const FlowOp& op : ops) {
            fills.clear();
            apply(warm, op, fills);
        }
    }

    {  // throughput: one clock around the whole replay
        Book book = fresh_book<Book>(min_price, max_price);
        std::size_t fill_count = 0;
        const auto start = Clock::now();
        for (const FlowOp& op : ops) {
            fills.clear();
            apply(book, op, fills);
            fill_count += fills.size();
        }
        const auto stop = Clock::now();
        const double seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(stop - start).count();
        result.throughput_mops = static_cast<double>(ops.size()) / seconds / 1e6;
        result.fills = fill_count;
    }

    {  // latency: per-op TSC timestamps (rdtsc overhead included, stated)
        Book book = fresh_book<Book>(min_price, max_price);
        const double ns_per_tick = tsc_ns_per_tick();
        std::vector<std::uint64_t> ticks;
        ticks.reserve(ops.size());
        for (const FlowOp& op : ops) {
            fills.clear();
            const std::uint64_t t0 = tsc_now();
            apply(book, op, fills);
            const std::uint64_t t1 = tsc_now();
            ticks.push_back(t1 - t0);
        }
        std::sort(ticks.begin(), ticks.end());
        auto pct = [&](double p) {
            const std::uint64_t t =
                ticks[static_cast<std::size_t>(p * (static_cast<double>(ticks.size()) - 1.0))];
            return static_cast<std::uint64_t>(static_cast<double>(t) * ns_per_tick);
        };
        result.p50 = pct(0.50);
        result.p90 = pct(0.90);
        result.p99 = pct(0.99);
        result.p999 = pct(0.999);
        result.max =
            static_cast<std::uint64_t>(static_cast<double>(ticks.back()) * ns_per_tick);
    }
    return result;
}

void emit(std::ostream& out, const std::string& impl, const std::string& flow,
          const RunResult& r) {
    out << impl << ',' << flow << ',' << r.ops << ',' << r.fills << ','
        << r.throughput_mops << ',' << r.p50 << ',' << r.p90 << ',' << r.p99 << ','
        << r.p999 << ',' << r.max << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::string flow = "synthetic";
    std::string file;
    std::string csv_path;
    std::size_t n_ops = 1'000'000;
    std::uint64_t seed = 42;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (arg == "--flow") flow = next();
        else if (arg == "--file") file = next();
        else if (arg == "--ops") n_ops = std::stoull(next());
        else if (arg == "--seed") seed = std::stoull(next());
        else if (arg == "--csv") csv_path = next();
        else {
            std::cerr << "usage: lob_bench [--flow synthetic|lobster] [--file msgs.csv]\n"
                         "                 [--ops N] [--seed S] [--csv out.csv]\n";
            return 2;
        }
    }

    pin_to_core(0);

    std::vector<FlowOp> ops;
    Price min_price = 0, max_price = 0;
    std::string flow_label = flow;

    if (flow == "synthetic") {
        FlowConfig cfg;
        cfg.operations = n_ops;
        cfg.seed = seed;
        ops = generate_flow(cfg);
        min_price = cfg.min_price;
        max_price = cfg.max_price;
        flow_label += "-seed" + std::to_string(seed);
    } else if (flow == "lobster") {
        LobsterStats stats;
        ops = load_lobster(file, stats);
        // Ladder bounds from the file itself, with headroom.
        const Price margin = (stats.max_price - stats.min_price) / 4 + 1;
        min_price = stats.min_price > margin ? stats.min_price - margin : 1;
        max_price = stats.max_price + margin;
        std::cerr << "lobster: " << ops.size() << " ops (" << stats.submits
                  << " submits, " << stats.cancels << " cancels, " << stats.reduces
                  << " reduces, " << stats.synthesized_executions
                  << " synthesized executions, " << stats.skipped << " skipped), "
                  << "price range [" << stats.min_price << ", " << stats.max_price
                  << "]\n";
    } else {
        std::cerr << "unknown --flow " << flow << '\n';
        return 2;
    }

    const RunResult map_result = run<MapBook>(ops, min_price, max_price);
    const RunResult fast_result = run<FastBook>(ops, min_price, max_price);

    std::cout << "impl,flow,ops,fills,throughput_mops,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
    emit(std::cout, "MapBook", flow_label, map_result);
    emit(std::cout, "FastBook", flow_label, fast_result);

    if (!csv_path.empty()) {
        const bool fresh = !std::ifstream(csv_path).good();
        std::ofstream out(csv_path, std::ios::app);
        if (fresh)
            out << "impl,flow,ops,fills,throughput_mops,p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
        emit(out, "MapBook", flow_label, map_result);
        emit(out, "FastBook", flow_label, fast_result);
    }

    // Sanity: identical streams must produce identical fill counts.
    if (map_result.fills != fast_result.fills) {
        std::cerr << "FILL COUNT MISMATCH: MapBook " << map_result.fills << " vs FastBook "
                  << fast_result.fills << '\n';
        return 1;
    }
    return 0;
}
