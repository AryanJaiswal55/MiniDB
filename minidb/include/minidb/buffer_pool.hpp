#pragma once
// BufferPool: write-back LRU page cache in front of the Pager.
//  * read() hit  -> copy out of memory, move frame to the MRU end
//  * read() miss -> load from disk, evicting the LRU frame (writing it back if dirty)
//  * write()     -> update the cached frame, mark dirty; disk is touched on eviction/flush
#include <cstdint>
#include <list>
#include <unordered_map>

#include "minidb/pager.hpp"

namespace minidb {

class BufferPool {
public:
    struct Stats {
        std::uint64_t hits = 0, misses = 0, evictions = 0, writebacks = 0;
        double hit_rate() const { return hits + misses ? static_cast<double>(hits) / (hits + misses) : 0.0; }
    };

    BufferPool(Pager& pager, std::size_t capacity_pages);

    void read(PageId id, Page& out);
    void write(PageId id, const Page& in);
    PageId allocate() { return pager_.allocate(); }
    void flush_all();  // write back every dirty frame (frames stay cached)

    const Stats& stats() const noexcept { return stats_; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t cached() const noexcept { return frames_.size(); }

private:
    struct Frame {
        PageId id;
        Page data;
        bool dirty;
    };
    using FrameList = std::list<Frame>;  // front = most recently used

    FrameList::iterator touch(FrameList::iterator it);
    void make_room();

    Pager& pager_;
    std::size_t capacity_;
    FrameList frames_;
    std::unordered_map<PageId, FrameList::iterator> index_;
    Stats stats_;
};

} // namespace minidb
