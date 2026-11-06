#pragma once
// Pager: a file viewed as an array of fixed-size 4 KiB pages.
#include <array>
#include <cstdint>
#include <fstream>
#include <string>

namespace minidb {

constexpr std::size_t kPageSize = 4096;
using PageId = std::uint32_t;
constexpr PageId kInvalidPage = 0xFFFFFFFFu;
using Page = std::array<std::uint8_t, kPageSize>;

class Pager {
public:
    explicit Pager(const std::string& path);  // opens, or creates if missing
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    PageId page_count() const noexcept { return count_; }
    PageId allocate();                      // appends a zero-filled page, returns its id
    void read(PageId id, Page& out);
    void write(PageId id, const Page& in);  // id must already be allocated
    void sync();                            // flush to the OS

    std::uint64_t reads() const noexcept { return reads_; }
    std::uint64_t writes() const noexcept { return writes_; }

private:
    std::fstream f_;
    PageId count_ = 0;
    std::uint64_t reads_ = 0, writes_ = 0;
};

} // namespace minidb
