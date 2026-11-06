#include "minidb/buffer_pool.hpp"

#include <stdexcept>

namespace minidb {

BufferPool::BufferPool(Pager& pager, std::size_t capacity_pages)
    : pager_(pager), capacity_(capacity_pages) {
    if (capacity_ == 0) throw std::invalid_argument("minidb: buffer pool needs at least one page");
}

BufferPool::FrameList::iterator BufferPool::touch(FrameList::iterator it) {
    frames_.splice(frames_.begin(), frames_, it);  // O(1): relink, iterator stays valid
    return frames_.begin();
}

void BufferPool::make_room() {
    while (frames_.size() >= capacity_) {
        Frame& victim = frames_.back();
        if (victim.dirty) { pager_.write(victim.id, victim.data); ++stats_.writebacks; }
        index_.erase(victim.id);
        frames_.pop_back();
        ++stats_.evictions;
    }
}

void BufferPool::read(PageId id, Page& out) {
    if (auto it = index_.find(id); it != index_.end()) {
        ++stats_.hits;
        out = touch(it->second)->data;
        return;
    }
    ++stats_.misses;
    make_room();
    frames_.push_front(Frame{id, Page{}, false});
    try {
        pager_.read(id, frames_.front().data);
    } catch (...) {
        frames_.pop_front();  // do not cache a frame that failed to load
        throw;
    }
    index_[id] = frames_.begin();
    out = frames_.front().data;
}

void BufferPool::write(PageId id, const Page& in) {
    if (id >= pager_.page_count()) throw std::out_of_range("minidb: write to unallocated page");
    if (auto it = index_.find(id); it != index_.end()) {
        auto f = touch(it->second);
        f->data = in;
        f->dirty = true;
        return;
    }
    make_room();  // whole-page overwrite: no need to read the old contents first
    frames_.push_front(Frame{id, in, true});
    index_[id] = frames_.begin();
}

void BufferPool::flush_all() {
    for (Frame& f : frames_)
        if (f.dirty) { pager_.write(f.id, f.data); f.dirty = false; ++stats_.writebacks; }
    pager_.sync();
}

} // namespace minidb
