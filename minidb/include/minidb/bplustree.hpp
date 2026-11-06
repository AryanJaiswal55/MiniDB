#pragma once
// A disk-backed B+ tree: int64 keys -> short string values (<= 56 bytes).
//
//  * nodes are 4 KiB pages, accessed only through the BufferPool
//  * leaves are chained (next pointer) so range scans never revisit internal nodes
//  * inserts split nodes bottom-up; the root splits grow the tree by one level
//  * deletes are lazy: the key is removed from its leaf, nodes are never merged
//    (so space is not reclaimed; see README "Limitations")
//  * single-threaded; no write-ahead log, so a crash mid-update can corrupt the file
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "minidb/buffer_pool.hpp"
#include "minidb/pager.hpp"

namespace minidb {

constexpr std::size_t kMaxValue = 56;       // bytes per value
constexpr std::size_t kMaxLeafCap = 62;     // entries per leaf page
constexpr std::size_t kMaxInternalCap = 339;// keys per internal page

class BPlusTree {
public:
    struct Options {
        std::uint16_t leaf_cap = 60;       // max entries per leaf before it splits
        std::uint16_t internal_cap = 300;  // max keys per internal node before it splits
        std::size_t cache_pages = 256;     // buffer pool size
    };

    // Creates the file if needed. For an existing file the node capacities stored in
    // its header win over `opts` (the on-disk layout depends on them).
    explicit BPlusTree(const std::string& path, Options opts);
    explicit BPlusTree(const std::string& path) : BPlusTree(path, Options{}) {}
    ~BPlusTree();
    BPlusTree(const BPlusTree&) = delete;
    BPlusTree& operator=(const BPlusTree&) = delete;

    bool put(std::int64_t key, const std::string& value);  // true if new, false if overwritten
    std::optional<std::string> get(std::int64_t key);
    bool erase(std::int64_t key);                           // true if the key existed
    // All entries with lo <= key <= hi, in key order.
    std::vector<std::pair<std::int64_t, std::string>> scan(std::int64_t lo, std::int64_t hi);

    std::size_t size() const noexcept { return count_; }
    std::size_t height();   // 1 = a single leaf
    void flush();           // write all dirty pages to the file

    // Structural self-check. Returns "" if every invariant holds, else a description of
    // the first violation found: key order, separator bounds, equal leaf depth,
    // node fill limits, leaf chain order and entry count.
    std::string validate();

    const BufferPool::Stats& cache_stats() const noexcept { return pool_.stats(); }
    std::uint64_t disk_reads() const noexcept { return pager_.reads(); }
    std::uint64_t disk_writes() const noexcept { return pager_.writes(); }
    std::uint16_t leaf_cap() const noexcept { return leaf_cap_; }
    std::uint16_t internal_cap() const noexcept { return internal_cap_; }
    PageId page_count() const noexcept { return pager_.page_count(); }

private:
    struct Node {
        bool leaf = true;
        std::vector<std::int64_t> keys;
        std::vector<std::string> values;   // leaf only
        std::vector<PageId> children;      // internal only (keys.size() + 1 entries)
        PageId next = kInvalidPage;        // leaf only: right sibling
    };
    struct Split {
        bool happened = false;
        std::int64_t separator = 0;
        PageId right = kInvalidPage;
    };

    Node load(PageId id);
    void store(PageId id, const Node& n);
    void write_header();
    Split insert_rec(PageId id, std::int64_t key, const std::string& value, bool& inserted);
    PageId find_leaf(std::int64_t key);
    void validate_rec(PageId id, const std::optional<std::int64_t>& lo,
                      const std::optional<std::int64_t>& hi, std::size_t depth,
                      std::size_t& leaf_depth, std::size_t& entries, std::string& err);

    Pager pager_;
    BufferPool pool_;
    PageId root_ = kInvalidPage;
    std::uint16_t leaf_cap_ = 0, internal_cap_ = 0;
    std::uint64_t count_ = 0;
};

} // namespace minidb
