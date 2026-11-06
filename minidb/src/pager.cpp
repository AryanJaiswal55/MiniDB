#include "minidb/pager.hpp"

#include <stdexcept>

namespace minidb {

Pager::Pager(const std::string& path) {
    f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f_) {  // does not exist yet: create, then reopen read/write
        f_.clear();
        { std::ofstream create(path, std::ios::binary); }
        f_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f_) throw std::runtime_error("minidb: cannot open " + path);
    }
    f_.seekg(0, std::ios::end);
    const std::streamoff size = f_.tellg();
    if (size < 0 || size % static_cast<std::streamoff>(kPageSize) != 0)
        throw std::runtime_error("minidb: corrupt file (size is not a multiple of the page size)");
    count_ = static_cast<PageId>(size / static_cast<std::streamoff>(kPageSize));
}

PageId Pager::allocate() {
    const Page zero{};
    f_.seekp(static_cast<std::streamoff>(count_) * static_cast<std::streamoff>(kPageSize));
    f_.write(reinterpret_cast<const char*>(zero.data()), static_cast<std::streamsize>(kPageSize));
    if (!f_) throw std::runtime_error("minidb: write failed while extending file");
    ++writes_;
    return count_++;
}

void Pager::read(PageId id, Page& out) {
    if (id >= count_) throw std::out_of_range("minidb: read of unallocated page");
    f_.seekg(static_cast<std::streamoff>(id) * static_cast<std::streamoff>(kPageSize));
    f_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(kPageSize));
    if (!f_) throw std::runtime_error("minidb: read failed");
    ++reads_;
}

void Pager::write(PageId id, const Page& in) {
    if (id >= count_) throw std::out_of_range("minidb: write to unallocated page");
    f_.seekp(static_cast<std::streamoff>(id) * static_cast<std::streamoff>(kPageSize));
    f_.write(reinterpret_cast<const char*>(in.data()), static_cast<std::streamsize>(kPageSize));
    if (!f_) throw std::runtime_error("minidb: write failed");
    ++writes_;
}

void Pager::sync() {
    f_.flush();
    if (!f_) throw std::runtime_error("minidb: flush failed");
}

} // namespace minidb
