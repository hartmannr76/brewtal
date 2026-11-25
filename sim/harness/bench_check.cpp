/**
 * Presubmit UI benchmark: the same script as the on-device one (lib/app bench.h), on the host
 * harness with a device-paced virtual clock, compared with bench/host_baseline.csv.
 *
 *   bench_check <baseline.csv>                    compare; exit 1 if the UI got more expensive
 *   bench_check <baseline.csv> --update-baseline  write the current numbers as the new baseline
 *
 * What's measured is exact and repeatable: how many pixels each frame renders and invalidates,
 * which is what sets the frame rate on the device. fps and avg_ms are estimates for the
 * CrowPanel from those (bench_model.h); tools/device_bench.py measures the real thing.
 * Increases need a deliberate --update-baseline in the same commit; improvements pass (and say
 * so, so the baseline can be tightened).
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern "C" {
void bd_start(double);
void bd_frame(double);
void bd_set_demo(int);
const char * bd_bench_run();
const char * bd_next_log();
}

namespace {

using Key = std::pair<std::string, std::string>;   // phase, metric

/* metric -> +1 higher is better, -1 lower is better; gated ones fail on a >kTolerance change */
struct Rule {
    const char * metric;
    int sense;
    bool gated;
};
const Rule kRules[] = {
    { "rendered_px_per_frame", -1, true },   // exact: the work per frame
    { "px_per_frame", -1, true },            // exact: area invalidated per frame
    { "fps", +1, true },                     // estimated from the work (bench_model.h)
    { "avg_ms", -1, false },
    { "max_ms", -1, false },
    { "frames", +1, false },
};
constexpr double kTolerance = 0.05;   // the numbers are deterministic: 5% is a real change

std::map<Key, double> parse(const std::string & text)
{
    std::map<Key, double> out;
    std::istringstream in(text);
    std::string line;
    while(std::getline(in, line)) {
        const size_t p = line.find("BENCH phase=");
        if(p == std::string::npos) continue;
        std::istringstream f(line.substr(p + 6));
        std::string kv, phase;
        while(f >> kv) {
            const size_t eq = kv.find('=');
            if(eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if(k == "phase") phase = v;
            else if(k != "ms") out[{ phase, k }] = std::atof(v.c_str());
        }
    }
    return out;
}

std::map<Key, double> load(const char * path, std::vector<Key> & order)
{
    std::map<Key, double> out;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);   // header
    while(std::getline(in, line)) {
        std::istringstream f(line);
        std::string phase, metric, value;
        if(std::getline(f, phase, ',') && std::getline(f, metric, ',') && std::getline(f, value)) {
            out[{ phase, metric }] = std::atof(value.c_str());
            order.push_back({ phase, metric });
        }
    }
    return out;
}

const Rule * rule(const std::string & metric)
{
    for(const Rule & r : kRules)
        if(metric == r.metric) return &r;
    return nullptr;
}

}  // namespace

int main(int argc, char ** argv)
{
    if(argc < 2) {
        std::fprintf(stderr, "usage: bench_check <baseline.csv> [--update-baseline]\n");
        return 2;
    }
    const char * path = argv[1];
    const bool update = argc > 2 && std::strcmp(argv[2], "--update-baseline") == 0;

    bd_start(0);
    bd_set_demo(1);   /* the device bench is a demo build: same pill, same work */
    bd_frame(16);
    const std::string text = bd_bench_run();
    while(*bd_next_log()) {}
    if(text.find("BENCH done") == std::string::npos) {
        std::printf("bench_check: the benchmark did not finish\n%s", text.c_str());
        return 1;
    }

    /* phases in run order, metrics as logged */
    std::vector<Key> order;
    std::map<Key, double> now;
    {
        std::istringstream in(text);
        std::string line;
        while(std::getline(in, line)) {
            for(const auto & kv : parse(line)) {
                if(!rule(kv.first.second)) continue;
                now[kv.first] = kv.second;
                order.push_back(kv.first);
            }
        }
    }

    if(update) {
        std::ofstream out(path, std::ios::trunc);
        out << "phase,metric,value\n";
        for(const Key & k : order) {
            char v[32];
            std::snprintf(v, sizeof(v), "%g", now[k]);
            out << k.first << ',' << k.second << ',' << v << '\n';
        }
        std::printf("bench_check: baseline written to %s (%zu values)\n", path, order.size());
        return 0;
    }

    std::vector<Key> base_order;
    const std::map<Key, double> base = load(path, base_order);
    if(base.empty()) {
        std::printf("bench_check: no baseline at %s. Run: bench_check %s --update-baseline\n", path, path);
        return 1;
    }

    std::printf("bench_check: host UI benchmark (fps/avg_ms are CrowPanel estimates)\n");
    std::printf("%-12s %-22s %12s %12s %8s\n", "phase", "metric", "baseline", "now", "change");
    int worse = 0, better = 0;
    for(const Key & k : order) {
        const Rule * r = rule(k.second);
        const auto b = base.find(k);
        const double v = now[k];
        char change[16] = "", flag[24] = "";
        if(b == base.end()) {
            std::snprintf(flag, sizeof(flag), "  NEW");
            if(r->gated) worse++;
        }
        else if(b->second != 0.0) {
            const double pct = (v - b->second) / std::fabs(b->second);
            std::snprintf(change, sizeof(change), "%+.1f%%", pct * 100.0);
            if(r->gated && -r->sense * pct > kTolerance) {
                std::snprintf(flag, sizeof(flag), "  WORSE");
                worse++;
            }
            else if(r->gated && r->sense * pct > kTolerance) {
                std::snprintf(flag, sizeof(flag), "  better");
                better++;
            }
        }
        std::printf("%-12s %-22s %12.1f %12.1f %8s%s\n", k.first.c_str(), k.second.c_str(),
                    b == base.end() ? 0.0 : b->second, v, change, flag);
    }
    for(const Key & k : base_order)
        if(!now.count(k) && rule(k.second) && rule(k.second)->gated) {
            std::printf("%-12s %-22s missing from this run\n", k.first.c_str(), k.second.c_str());
            worse++;
        }

    if(worse) {
        std::printf("\nbench_check FAILED: %d value(s) more than %.0f%% worse than %s (or new/missing).\n"
                    "If the change is intended, update the baseline in the same commit:\n"
                    "  bench_check %s --update-baseline\n",
                    worse, kTolerance * 100.0, path, path);
        return 1;
    }
    if(better) std::printf("\n%d value(s) improved; lock them in with --update-baseline.\n", better);
    std::printf("bench_check: OK\n");
    return 0;
}
