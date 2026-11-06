#include "minidb/bplustree.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace minidb {

namespace {

constexpr char kMagic[8] = {'M', 'I', 'N', 'I', 'D', 'B', '0', '1'};
constexpr std::size_t kNodeHeader = 16;
constexpr std::size_t kLeafEntry = 8 + 1 + kMaxValue;  // key, length byte, padded value

static_assert(kNodeHeader + kMaxLeafCap * kLeafEntry <= kPageSize, "leaf does not fit a page");
static_assert(kNodeHeader + kMaxInternalCap * 8 + (kMaxInternalCap + 1) * 4 <= kPageSize,
              "internal node does not fit a page");

// Explicit little-endian encoding so files are portable across machines.
void put16(Page& p, std::size_t o, std::uint16_t v) { p[o] = v & 0xff; p[o + 1] = v >> 8; }
void put32(Page& p, std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[o + i] = (v >> (8 * i)) & 0xff; }
void put64(Page& p, std::size_t o, std::uint64_t v) { for (int i = 0; i < 8; ++i) p[o + i] = (v >> (8 * i)) & 0xff; }
std::uint16_t get16(const Page& p, std::size_t o) { return static_cast<std::uint16_t>(p[o] | (p[o + 1] << 8)); }
std::uint32_t get32(const Page& p, std::size_t o) { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[o + i]) << (8 * i); return v; }
std::uint64_t get64(const Page& p, std::size_t o) { std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[o + i]) << (8 * i); return v; }

} // namespace

// Header page (page 0): magic[8] | page_size u32 | root u32 | leaf_cap u16 | internal_cap u16 | count u64
BPlusTree::BPlusTree(const std::string& path, Options opts)
    : pager_(path), pool_(pager_, opts.cache_pages) {
    if (pager_.page_count() == 0) {
        if (opts.leaf_cap < 3 || opts.leaf_cap > kMaxLeafCap)
            throw std::invalid_argument("minidb: leaf_cap must be in [3, 62]");
        if (opts.internal_cap < 3 || opts.internal_cap > kMaxInternalCap)
            throw std::invalid_argument("minidb: internal_cap must be in [3, 339]");
        leaf_cap_ = opts.leaf_cap;
        internal_cap_ = opts.internal_cap;
        pool_.allocate();                 // page 0: header
        root_ = pool_.allocate();         // page 1: empty root leaf
        store(root_, Node{});
        write_header();
        return;
    }
    Page h;
    pool_.read(0, h);
    if (std::memcmp(h.data(), kMagic, sizeof kMagic) != 0)
        throw std::runtime_error("minidb: not a minidb file (bad magic)");
    if (get32(h, 8) != kPageSize) throw std::runtime_error("minidb: page size mismatch");
    root_ = get32(h, 12);
    leaf_cap_ = get16(h, 16);
    internal_cap_ = get16(h, 18);
    count_ = get64(h, 20);
    if (root_ == 0 || root_ >= pager_.page_count() || leaf_cap_ < 3 || leaf_cap_ > kMaxLeafCap ||
        internal_cap_ < 3 || internal_cap_ > kMaxInternalCap)
        throw std::runtime_error("minidb: corrupt header");
}

BPlusTree::~BPlusTree() {
    try { flush(); } catch (...) {}  // destructors must not throw
}

void BPlusTree::flush() { pool_.flush_all(); }

void BPlusTree::write_header() {
    Page h{};
    std::memcpy(h.data(), kMagic, sizeof kMagic);
    put32(h, 8, static_cast<std::uint32_t>(kPageSize));
    put32(h, 12, root_);
    put16(h, 16, leaf_cap_);
    put16(h, 18, internal_cap_);
    put64(h, 20, count_);
    pool_.write(0, h);
}

// Node page: type u8 (0 leaf, 1 internal) | pad | nkeys u16 @2 | next u32 @4 | payload @16
//   leaf     : nkeys x { key i64 | len u8 | value[56] }
//   internal : nkeys x key i64, then (nkeys + 1) x child u32
void BPlusTree::store(PageId id, const Node& n) {
    Page p{};
    p[0] = n.leaf ? 0 : 1;
    put16(p, 2, static_cast<std::uint16_t>(n.keys.size()));
    put32(p, 4, n.next);
    std::size_t o = kNodeHeader;
    if (n.leaf) {
        for (std::size_t i = 0; i < n.keys.size(); ++i, o += kLeafEntry) {
            put64(p, o, static_cast<std::uint64_t>(n.keys[i]));
            p[o + 8] = static_cast<std::uint8_t>(n.values[i].size());
            std::memcpy(&p[o + 9], n.values[i].data(), n.values[i].size());
        }
    } else {
        for (std::int64_t k : n.keys) { put64(p, o, static_cast<std::uint64_t>(k)); o += 8; }
        for (PageId c : n.children) { put32(p, o, c); o += 4; }
    }
    pool_.write(id, p);
}

BPlusTree::Node BPlusTree::load(PageId id) {
    Page p;
    pool_.read(id, p);
    Node n;
    n.leaf = p[0] == 0;
    const std::size_t nkeys = get16(p, 2);
    n.next = get32(p, 4);
    if (nkeys > (n.leaf ? kMaxLeafCap : kMaxInternalCap)) throw std::runtime_error("minidb: corrupt node");
    std::size_t o = kNodeHeader;
    if (n.leaf) {
        for (std::size_t i = 0; i < nkeys; ++i, o += kLeafEntry) {
            n.keys.push_back(static_cast<std::int64_t>(get64(p, o)));
            const std::size_t len = p[o + 8];
            if (len > kMaxValue) throw std::runtime_error("minidb: corrupt value length");
            n.values.emplace_back(reinterpret_cast<const char*>(&p[o + 9]), len);
        }
    } else {
        for (std::size_t i = 0; i < nkeys; ++i, o += 8) n.keys.push_back(static_cast<std::int64_t>(get64(p, o)));
        for (std::size_t i = 0; i <= nkeys; ++i, o += 4) n.children.push_back(get32(p, o));
    }
    return n;
}

// Child to descend into: the number of separator keys <= key. A key equal to a
// separator belongs to the RIGHT subtree (separators are the first key of the right node).
static std::size_t child_index(const std::vector<std::int64_t>& keys, std::int64_t key) {
    return static_cast<std::size_t>(std::upper_bound(keys.begin(), keys.end(), key) - keys.begin());
}

PageId BPlusTree::find_leaf(std::int64_t key) {
    PageId id = root_;
    for (;;) {
        Node n = load(id);
        if (n.leaf) return id;
        id = n.children[child_index(n.keys, key)];
    }
}

BPlusTree::Split BPlusTree::insert_rec(PageId id, std::int64_t key, const std::string& value, bool& inserted) {
    Node n = load(id);
    if (n.leaf) {
        const auto it = std::lower_bound(n.keys.begin(), n.keys.end(), key);
        const std::size_t idx = static_cast<std::size_t>(it - n.keys.begin());
        if (it != n.keys.end() && *it == key) {  // overwrite
            n.values[idx] = value;
            store(id, n);
            inserted = false;
            return {};
        }
        n.keys.insert(it, key);
        n.values.insert(n.values.begin() + static_cast<std::ptrdiff_t>(idx), value);
        inserted = true;
        if (n.keys.size() <= leaf_cap_) { store(id, n); return {}; }

        // Leaf overflow: right half moves to a new leaf spliced into the sibling chain.
        const std::size_t mid = n.keys.size() / 2;
        Node right;
        right.leaf = true;
        right.keys.assign(n.keys.begin() + static_cast<std::ptrdiff_t>(mid), n.keys.end());
        right.values.assign(n.values.begin() + static_cast<std::ptrdiff_t>(mid), n.values.end());
        n.keys.resize(mid);
        n.values.resize(mid);
        right.next = n.next;
        const PageId rid = pool_.allocate();
        n.next = rid;
        store(id, n);
        store(rid, right);
        return {true, right.keys.front(), rid};  // separator is COPIED up (stays in the leaf)
    }

    const std::size_t ci = child_index(n.keys, key);
    const Split s = insert_rec(n.children[ci], key, value, inserted);
    if (!s.happened) return {};
    n.keys.insert(n.keys.begin() + static_cast<std::ptrdiff_t>(ci), s.separator);
    n.children.insert(n.children.begin() + static_cast<std::ptrdiff_t>(ci) + 1, s.right);
    if (n.keys.size() <= internal_cap_) { store(id, n); return {}; }

    // Internal overflow: the middle key MOVES up (it does not stay in either half).
    const std::size_t mid = n.keys.size() / 2;
    const std::int64_t up = n.keys[mid];
    Node right;
    right.leaf = false;
    right.keys.assign(n.keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1, n.keys.end());
    right.children.assign(n.children.begin() + static_cast<std::ptrdiff_t>(mid) + 1, n.children.end());
    n.keys.resize(mid);
    n.children.resize(mid + 1);
    const PageId rid = pool_.allocate();
    store(id, n);
    store(rid, right);
    return {true, up, rid};
}

bool BPlusTree::put(std::int64_t key, const std::string& value) {
    if (value.size() > kMaxValue) throw std::length_error("minidb: value longer than 56 bytes");
    bool inserted = false;
    const Split s = insert_rec(root_, key, value, inserted);
    if (s.happened) {  // the root itself split: the tree grows one level
        Node r;
        r.leaf = false;
        r.keys = {s.separator};
        r.children = {root_, s.right};
        const PageId rid = pool_.allocate();
        store(rid, r);
        root_ = rid;
    }
    if (inserted) ++count_;
    if (s.happened || inserted) write_header();
    return inserted;
}

std::optional<std::string> BPlusTree::get(std::int64_t key) {
    Node leaf = load(find_leaf(key));
    const auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (it == leaf.keys.end() || *it != key) return std::nullopt;
    return leaf.values[static_cast<std::size_t>(it - leaf.keys.begin())];
}

bool BPlusTree::erase(std::int64_t key) {
    const PageId id = find_leaf(key);
    Node leaf = load(id);
    const auto it = std::lower_bound(leaf.keys.begin(), leaf.keys.end(), key);
    if (it == leaf.keys.end() || *it != key) return false;
    const auto idx = it - leaf.keys.begin();
    leaf.keys.erase(it);
    leaf.values.erase(leaf.values.begin() + idx);
    store(id, leaf);
    --count_;
    write_header();
    return true;
}

std::vector<std::pair<std::int64_t, std::string>> BPlusTree::scan(std::int64_t lo, std::int64_t hi) {
    std::vector<std::pair<std::int64_t, std::string>> out;
    if (lo > hi) return out;
    PageId id = find_leaf(lo);
    while (id != kInvalidPage) {  // follow the leaf chain; empty leaves (after erase) are skipped naturally
        const Node leaf = load(id);
        for (std::size_t i = 0; i < leaf.keys.size(); ++i) {
            if (leaf.keys[i] < lo) continue;
            if (leaf.keys[i] > hi) return out;
            out.emplace_back(leaf.keys[i], leaf.values[i]);
        }
        id = leaf.next;
    }
    return out;
}

std::size_t BPlusTree::height() {
    std::size_t h = 1;
    PageId id = root_;
    for (;;) {
        const Node n = load(id);
        if (n.leaf) return h;
        id = n.children.front();
        ++h;
    }
}

void BPlusTree::validate_rec(PageId id, const std::optional<std::int64_t>& lo,
                             const std::optional<std::int64_t>& hi, std::size_t depth,
                             std::size_t& leaf_depth, std::size_t& entries, std::string& err) {
    if (!err.empty()) return;
    const Node n = load(id);
    auto fail = [&](const std::string& m) { if (err.empty()) err = "page " + std::to_string(id) + ": " + m; };
    for (std::size_t i = 0; i < n.keys.size(); ++i) {
        if (i && n.keys[i - 1] >= n.keys[i]) return fail("keys not strictly increasing");
        if (lo && n.keys[i] < *lo) return fail("key below its subtree's lower bound");
        if (hi && n.keys[i] >= *hi) return fail("key not below its subtree's upper bound");
    }
    if (n.leaf) {
        if (n.values.size() != n.keys.size()) return fail("leaf keys/values size mismatch");
        if (n.keys.size() > leaf_cap_) return fail("leaf over capacity");
        if (leaf_depth == 0) leaf_depth = depth;
        else if (leaf_depth != depth) return fail("leaves are not all at the same depth");
        entries += n.keys.size();
        return;
    }
    if (n.keys.empty()) return fail("internal node without keys");
    if (n.children.size() != n.keys.size() + 1) return fail("internal node child count mismatch");
    if (n.keys.size() > internal_cap_) return fail("internal node over capacity");
    for (std::size_t i = 0; i < n.children.size(); ++i) {
        const std::optional<std::int64_t> clo = i == 0 ? lo : std::optional<std::int64_t>(n.keys[i - 1]);
        const std::optional<std::int64_t> chi = i == n.keys.size() ? hi : std::optional<std::int64_t>(n.keys[i]);
        validate_rec(n.children[i], clo, chi, depth + 1, leaf_depth, entries, err);
    }
}

std::string BPlusTree::validate() {
    std::string err;
    std::size_t leaf_depth = 0, entries = 0;
    validate_rec(root_, std::nullopt, std::nullopt, 1, leaf_depth, entries, err);
    if (!err.empty()) return err;
    if (entries != count_) return "entry count in tree (" + std::to_string(entries) + ") != header count (" + std::to_string(count_) + ")";

    // Leaf chain: start at the leftmost leaf, keys must ascend across the whole chain.
    PageId id = root_;
    for (Node n = load(id); !n.leaf; n = load(id)) id = n.children.front();
    std::size_t chained = 0;
    std::optional<std::int64_t> prev;
    while (id != kInvalidPage) {
        const Node leaf = load(id);
        for (std::int64_t k : leaf.keys) {
            if (prev && *prev >= k) return "leaf chain not in ascending key order";
            prev = k;
            ++chained;
        }
        id = leaf.next;
    }
    if (chained != entries) return "leaf chain reaches " + std::to_string(chained) + " entries, tree has " + std::to_string(entries);
    return "";
}

} // namespace minidb
