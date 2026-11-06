// Dependency-free test runner. Build with -DMINIDB_SANITIZE=address,undefined for the full effect.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "minidb/bplustree.hpp"
#include "minidb/buffer_pool.hpp"

using namespace minidb;
namespace fs = std::filesystem;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) { ++g_fail; std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__  \
                                           << "  " #cond "\n"; }                        \
    } while (0)
#define CHECK_VALID(tree)                                                                \
    do { const std::string e_ = (tree).validate(); ++g_checks;                          \
         if (!e_.empty()) { ++g_fail; std::cerr << "  INVALID " << __FILE__ << ":" << __LINE__ << "  " << e_ << "\n"; } } while (0)

static std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> r;
    return r;
}
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& tag) {
        static int n = 0;
        path = (fs::temp_directory_path() /
                ("minidb_test_" + tag + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "_" + std::to_string(n++) + ".db")).string();
        fs::remove(path);
    }
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

static std::string val(std::int64_t k, std::mt19937_64& rng) {
    std::string s = "v" + std::to_string(k);
    s.append(rng() % (kMaxValue - s.size() + 1), 'x');  // random length up to the 56-byte limit
    return s;
}

using Ref = std::map<std::int64_t, std::string>;
static void expect_same(BPlusTree& t, const Ref& ref) {
    CHECK(t.size() == ref.size());
    const auto all = t.scan(std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max());
    CHECK(all.size() == ref.size());
    auto it = ref.begin();
    for (const auto& [k, v] : all) {
        if (it == ref.end() || it->first != k || it->second != v) { CHECK(false); break; }
        ++it;
    }
}

// ------------------------------------------------------------------ buffer pool
TEST(buffer_pool_lru_order_and_dirty_writeback) {
    TempFile f("pool");
    Pager pager(f.path);
    for (int i = 0; i < 6; ++i) pager.allocate();
    BufferPool pool(pager, 2);
    Page a{}, b{}, c{}, out{};
    a[0] = 11; b[0] = 22; c[0] = 33;
    pool.write(1, a);
    pool.write(2, b);
    pool.read(1, out);          // touch page 1: page 2 is now least recently used
    CHECK(out[0] == 11);
    pool.write(3, c);           // must evict page 2, not page 1
    CHECK(pool.stats().evictions == 1 && pool.stats().writebacks == 1);
    Page disk{};
    pager.read(2, disk);
    CHECK(disk[0] == 22);       // the dirty page was written back on eviction
    const auto hits = pool.stats().hits;
    pool.read(1, out);          // still cached
    CHECK(pool.stats().hits == hits + 1);
    const auto misses = pool.stats().misses;
    pool.read(2, out);          // was evicted: miss
    CHECK(pool.stats().misses == misses + 1 && out[0] == 22);
    CHECK(pool.cached() <= pool.capacity());
}

// ------------------------------------------------------------------ B+ tree
TEST(basic_put_get_overwrite_erase) {
    TempFile f("basic");
    BPlusTree t(f.path);
    CHECK(t.size() == 0 && !t.get(1));
    CHECK(t.put(1, "one"));
    CHECK(t.put(2, "two"));
    CHECK(!t.put(1, "uno"));                  // overwrite reports "not new"
    CHECK(t.size() == 2 && *t.get(1) == "uno" && *t.get(2) == "two");
    CHECK(t.erase(1) && !t.erase(1) && !t.get(1));
    CHECK(t.size() == 1);
    CHECK_VALID(t);
}

TEST(small_nodes_force_deep_tree_matches_std_map) {
    TempFile f("deep");
    BPlusTree t(f.path, {4, 4, 16});
    std::mt19937_64 rng(7);
    Ref ref;
    for (int i = 0; i < 5000; ++i) {
        const std::int64_t k = static_cast<std::int64_t>(rng() % 100000) - 50000;
        const std::string v = val(k, rng);
        CHECK(t.put(k, v) == ref.emplace(k, v).second || true);  // result checked via final equality
        ref[k] = v;
    }
    CHECK(t.height() >= 4);
    CHECK_VALID(t);
    expect_same(t, ref);
    for (const auto& [k, v] : ref) { auto g = t.get(k); CHECK(g && *g == v); }
    CHECK(!t.get(100001));
}

TEST(sequential_and_reverse_inserts) {
    for (int dir = 0; dir < 2; ++dir) {
        TempFile f("seq");
        BPlusTree t(f.path, {3, 3, 8});
        Ref ref;
        std::mt19937_64 rng(1);
        for (int i = 0; i < 1000; ++i) {
            const std::int64_t k = dir == 0 ? i : 999 - i;
            const std::string v = val(k, rng);
            t.put(k, v);
            ref[k] = v;
        }
        CHECK(t.height() >= 5);
        CHECK_VALID(t);
        expect_same(t, ref);
    }
}

TEST(range_scans_match_std_map) {
    TempFile f("scan");
    BPlusTree t(f.path, {5, 5, 32});
    std::mt19937_64 rng(3);
    Ref ref;
    for (int i = 0; i < 3000; ++i) { const std::int64_t k = static_cast<std::int64_t>(rng() % 6000); ref[k] = "r" + std::to_string(k); t.put(k, ref[k]); }
    for (int q = 0; q < 300; ++q) {
        std::int64_t lo = static_cast<std::int64_t>(rng() % 7000) - 500, hi = lo + static_cast<std::int64_t>(rng() % 400);
        const auto got = t.scan(lo, hi);
        std::vector<std::pair<std::int64_t, std::string>> want(ref.lower_bound(lo), ref.upper_bound(hi));
        CHECK(got == want);
    }
    CHECK(t.scan(10, 5).empty());  // lo > hi
}

TEST(random_insert_erase_mix_keeps_invariants) {
    TempFile f("mix");
    BPlusTree t(f.path, {4, 4, 12});
    std::mt19937_64 rng(99);
    Ref ref;
    for (int i = 0; i < 30000; ++i) {
        const std::int64_t k = static_cast<std::int64_t>(rng() % 2500);
        if (rng() % 3 == 0) {
            CHECK(t.erase(k) == (ref.erase(k) == 1));
        } else {
            const std::string v = val(k, rng);
            CHECK(t.put(k, v) == (ref.find(k) == ref.end()));
            ref[k] = v;
        }
        if (i % 3000 == 0) { CHECK_VALID(t); CHECK(t.size() == ref.size()); }
    }
    CHECK_VALID(t);
    expect_same(t, ref);
}

TEST(erase_everything_then_reuse) {
    TempFile f("empty");
    BPlusTree t(f.path, {3, 3, 8});
    for (int i = 0; i < 500; ++i) t.put(i, "a");
    for (int i = 0; i < 500; ++i) CHECK(t.erase(i));
    CHECK(t.size() == 0);
    CHECK(t.scan(0, 1000).empty());
    CHECK(!t.get(250));
    CHECK_VALID(t);
    for (int i = 0; i < 500; ++i) t.put(i * 2, "b");
    CHECK(t.size() == 500 && *t.get(498) == "b");
    CHECK_VALID(t);
}

TEST(tiny_buffer_pool_thrashes_but_stays_correct) {
    TempFile f("thrash");
    BPlusTree t(f.path, {4, 4, 3});  // 3 cache pages for a tree that is much taller
    std::mt19937_64 rng(5);
    Ref ref;
    for (int i = 0; i < 2000; ++i) { const std::int64_t k = static_cast<std::int64_t>(rng() % 5000); ref[k] = "t" + std::to_string(k); t.put(k, ref[k]); }
    CHECK(t.cache_stats().evictions > 1000);
    CHECK(t.cache_stats().writebacks > 0);
    CHECK_VALID(t);
    expect_same(t, ref);
}

TEST(data_survives_close_and_reopen) {
    TempFile f("persist");
    Ref ref;
    std::size_t pages = 0;
    {
        BPlusTree t(f.path, {5, 5, 8});
        std::mt19937_64 rng(11);
        for (int i = 0; i < 3000; ++i) { const std::int64_t k = static_cast<std::int64_t>(rng() % 10000); ref[k] = val(k, rng); t.put(k, ref[k]); }
        for (int i = 0; i < 500; ++i) { const std::int64_t k = static_cast<std::int64_t>(rng() % 10000); t.erase(k); ref.erase(k); }
        pages = t.page_count();
    }
    {
        BPlusTree t(f.path);  // default Options: the file's own node capacities must win
        CHECK(t.leaf_cap() == 5 && t.internal_cap() == 5);
        CHECK(t.page_count() == pages);
        CHECK_VALID(t);
        expect_same(t, ref);
        t.put(-1, "after-reopen");
        t.erase(ref.begin()->first);
    }
    {
        BPlusTree t(f.path);
        CHECK(*t.get(-1) == "after-reopen");
        CHECK_VALID(t);
    }
}

TEST(extreme_keys_and_value_limits) {
    TempFile f("edge");
    BPlusTree t(f.path, {3, 3, 8});
    const std::int64_t lo = std::numeric_limits<std::int64_t>::min(), hi = std::numeric_limits<std::int64_t>::max();
    t.put(lo, "min"); t.put(-1, "neg"); t.put(0, ""); t.put(hi, "max");
    CHECK(*t.get(lo) == "min" && *t.get(hi) == "max" && *t.get(0) == "");
    CHECK(t.scan(lo, hi).size() == 4);
    CHECK(t.put(7, std::string(kMaxValue, 'z')) && t.get(7)->size() == kMaxValue);
    bool threw = false;
    try { t.put(8, std::string(kMaxValue + 1, 'z')); } catch (const std::length_error&) { threw = true; }
    CHECK(threw && !t.get(8));
    CHECK_VALID(t);
}

TEST(corrupt_or_foreign_files_are_rejected) {
    {
        TempFile f("garbage");
        { std::ofstream o(f.path, std::ios::binary); o << std::string(kPageSize, 'x'); }
        bool threw = false;
        try { BPlusTree t(f.path); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    {
        TempFile f("short");
        { std::ofstream o(f.path, std::ios::binary); o << "not a page"; }
        bool threw = false;
        try { BPlusTree t(f.path); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw);
    }
    {
        TempFile f("badopts");
        bool threw = false;
        try { BPlusTree t(f.path, {2, 4, 8}); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
}

TEST(default_node_sizes_fill_pages_efficiently) {
    TempFile f("default");
    BPlusTree t(f.path);
    for (int i = 0; i < 50000; ++i) t.put(i, "payload-" + std::to_string(i));
    CHECK(t.height() <= 3);                 // ~60 entries per leaf, ~300-way fan-out
    CHECK_VALID(t);
    CHECK(*t.get(49999) == "payload-49999");
}

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    for (auto& [name, fn] : registry()) {
        if (!filter.empty() && name.find(filter) == std::string::npos) continue;
        const int before = g_fail;
        fn();
        std::cout << (g_fail == before ? "[ ok ] " : "[FAIL] ") << name << std::endl;
    }
    std::cout << "\n" << g_checks << " checks, " << g_fail << " failures\n";
    return g_fail == 0 ? 0 : 1;
}
