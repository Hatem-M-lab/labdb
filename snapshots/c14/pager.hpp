#pragma once
// Pager -- names pages and moves them between memory and one database file.
// Challenge 1.4 version: open/read/write/extend/sync. Allocation with a
// free list arrives in Challenge 1.5.
//
// File layout: page i lives at byte offset i * kPageSize. Page 0 is the
// meta page: magic string, page count. It never leaves the pager.
//
// Honesty note: the pager is NOT crash-safe. A crash between two related
// page writes leaves the file inconsistent, and even a single page write
// can tear. Making that survivable is the entire subject of Unit 13.
// Today's contract: correct under clean operation, loud under I/O failure.

#include <cstdint>
#include <string>

#include "page.hpp"

namespace labdb {

class Pager {
 public:
  // Opens the database file, creating and formatting it if it is empty.
  explicit Pager(const std::string& path);
  ~Pager();

  Pager(const Pager&) = delete;
  Pager& operator=(const Pager&) = delete;

  // Read/write one full page. Page 0 is off limits; ids must be in range.
  // write_page requires the page's own header id to match its destination.
  void read_page(PageId id, Page& out);
  void write_page(PageId id, const Page& in);

  // Grow the file by one zeroed page and return its id.
  PageId extend();

  // Flush the meta page and fsync the file.
  void sync();

  std::uint32_t page_count() const { return page_count_; }

 private:
  void store_meta();

  int fd_ = -1;
  std::uint32_t page_count_ = 0;
};

}  // namespace labdb
