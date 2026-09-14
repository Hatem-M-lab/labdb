#pragma once
// Pager -- names pages and moves them between memory and one database file.
// Challenge 1.4 (open/read/write/sync) and 1.5 (allocate/free + free list).
//
// File layout: page i lives at byte offset i * kPageSize. Page 0 is the
// meta page: magic string, page count, free-list head. It never leaves
// the pager.
//
// Honesty note, repeated wherever it matters: the pager is NOT crash-safe.
// A crash between two related page writes leaves the file inconsistent,
// and even a single page write can tear. Making that survivable is the
// entire subject of Unit 13. Today's contract is: correct under clean
// operation, loud under I/O failure.

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

  // Hand out a page id: reuse the free-list head if there is one, else
  // grow the file by one zeroed page. The page's old contents are
  // unspecified -- the caller must format it before use.
  PageId allocate_page();

  // Return a page to the free list. Detects double-frees.
  void free_page(PageId id);

  // Flush the meta page and fsync the file: everything written so far is
  // now on stable storage (as far as fsync's promise goes -- Unit 13 has
  // much more to say about that sentence).
  void sync();

  std::uint32_t page_count() const { return page_count_; }
  PageId freelist_head() const { return freelist_head_; }

  // The B+Tree root lives in the meta page so it survives a reopen. A
  // fresh database reports kNullPage ("no tree yet"); Unit 2 uses this to
  // decide whether to plant a new tree or open an existing one. (When the
  // catalog arrives in Unit 7, this single-root field is replaced by a
  // per-table root; for now one tree is enough.)
  PageId btree_root() const { return btree_root_; }
  void set_btree_root(PageId id);

  // A second persistent root, alongside the tree: the head page of the
  // table heap that stores real rows (Unit 5). kNullPage means no rows yet.
  PageId heap_head() const { return heap_head_; }
  void set_heap_head(PageId id);

  // Instrumentation: how many page reads have happened. Unit 2's headline
  // measurement counts the reads a point lookup costs; this is the meter.
  std::uint64_t read_count() const { return reads_; }
  void reset_read_count() { reads_ = 0; }

  // Walks the free list and counts it. O(free pages) reads; for tests
  // and the stats output, not for hot paths.
  std::uint32_t freelist_length();

 private:
  void store_meta();

  int fd_ = -1;
  std::uint32_t page_count_ = 0;
  PageId freelist_head_ = kNullPage;
  PageId btree_root_ = kNullPage;
  PageId heap_head_ = kNullPage;
  std::uint64_t reads_ = 0;
};

}  // namespace labdb
