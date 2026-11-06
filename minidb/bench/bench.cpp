// How does buffer-pool size change disk I/O and speed? Run in Release mode.
// "disk reads" counts page reads issued to the file (the OS page cache hides their latency).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "minidb/bplustree.hpp"

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

int main() {
    constexpr int N = 100000;
    std::vector<std::int64_t> keys(N);
    std::mt19937_64 rng(42);
    for (auto& k : keys) k = static_cast<std::int64_t>(rng() % 10'000'000);

    std::printf("%d random inserts, %d random point lookups, 2000 range scans (width ~100)\n\n", N, N);
    std::printf("%10s | %9s %8s | %9s %8s | %8s | %10s %10s | %s\n", "cache(pg)", "ins ops/s", "hit%", "get ops/s",
                "hit%", "scan/s", "disk reads", "disk writes", "height/pages");
    for (std::size_t cache : {4u, 32u, 256u, 4096u}) {
        const std::string path = (std::filesystem::temp_directory_path() / "minidb_bench.db").string();
        std::filesystem::remove(path);
        minidb::BPlusTree::Options o;
        o.cache_pages = cache;
        minidb::BPlusTree t(path, o);

        auto t0 = Clock::now();
        for (std::int64_t k : keys) t.put(k, "value-" + std::to_string(k));
        const double ins = secs(t0);
        const auto ins_stats = t.cache_stats();

        t0 = Clock::now();
        std::size_t found = 0;
        for (std::int64_t k : keys) found += t.get(k).has_value();
        const double get = secs(t0);
        const auto get_stats = t.cache_stats();
        const double get_hits = static_cast<double>(get_stats.hits - ins_stats.hits);
        const double get_total = get_hits + static_cast<double>(get_stats.misses - ins_stats.misses);

        t0 = Clock::now();
        std::size_t rows = 0;
        for (int i = 0; i < 2000; ++i) {
            const std::int64_t lo = static_cast<std::int64_t>(rng() % 10'000'000);
            rows += t.scan(lo, lo + 1000).size();
        }
        const double scan = secs(t0);

        std::printf("%10zu | %9.0f %7.1f%% | %9.0f %7.1f%% | %8.0f | %10llu %10llu | %zu/%u  (found %zu, scanned %zu rows)\n", cache,
                    N / ins, ins_stats.hit_rate() * 100, N / get, get_total ? get_hits / get_total * 100 : 0.0, 2000 / scan,
                    static_cast<unsigned long long>(t.disk_reads()), static_cast<unsigned long long>(t.disk_writes()),
                    t.height(), t.page_count(), found, rows);
        t.flush();
        std::filesystem::remove(path);
    }
}
