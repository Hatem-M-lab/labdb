#pragma once
// Heap -- an unordered table of rows, stored as records in a chain of slotted
// pages over the buffer pool. Unit 5.
//
// This is where the slotted page from Unit 1 -- built, tested, and then idle
// while the B+Tree used its own leaf format -- finally earns its place. Each
// row is one variable-length cell; its address is a RID = (page, slot). The
// B+Tree can now hold a RID as its 8-byte value, so an indexed lookup returns
// the address of a real typed row.
//
// The heap's pages are threaded together by the page header's next_page link,
// newest first; the head is remembered in the meta page (Pager::heap_head) so
// the table survives a reopen. Insert tries the head page and, when it is
// full, prepends a fresh page. (A page that fills and is passed over is not
// revisited even if later erases free space in it: this heap has no free-space
// map. That is a real limitation, noted where it matters, not a bug.)

#include <cstdint>
#include <span>
#include <vector>

#include "buffer_pool.hpp"
#include "record.hpp"

namespace labdb {

class Heap {
 public:
  explicit Heap(BufferPool& pool);

  // Store a record's bytes; returns its RID. The record must fit in one page.
  RID insert(std::span<const std::uint8_t> rec);

  // Copy out the bytes of the record at `rid`. Fails if no live record is
  // there (e.g. a bad RID, or one that was erased).
  std::vector<std::uint8_t> get(RID rid) const;

  // Erase the record at `rid`. Returns false if it was not live.
  bool erase(RID rid);

  // Head page of the heap chain (kNullPage until the first insert).
  PageId head() const { return head_; }

 private:
  PageId new_page();  // allocate + format a fresh slotted page, linked at head

  BufferPool& pool_;
  PageId head_;
};

}  // namespace labdb
