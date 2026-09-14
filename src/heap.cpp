#include "heap.hpp"

#include "common.hpp"
#include "slotted_page.hpp"

namespace labdb {

Heap::Heap(BufferPool& pool) : pool_(pool), head_(pool.heap_head()) {}

PageId Heap::new_page() {
  const PageId id = pool_.allocate_page();  // a zeroed frame, header id set
  Page pg;
  pool_.read_page(id, pg);
  SlottedPage sp(pg);
  sp.init(id);                 // type = slotted, empty directory
  pg.set_next_page(head_);     // link the old head behind this new page
  pool_.write_page(id, pg);
  head_ = id;
  pool_.set_heap_head(id);     // persist: the table now starts here
  return id;
}

RID Heap::insert(std::span<const std::uint8_t> rec) {
  check_that(!rec.empty() && rec.size() <= kMaxRecordSize,
             "record does not fit in a single page (overflow pages are a "
             "later unit)");
  if (head_ == kNullPage) new_page();  // first row: create the first page

  // Try the head page.
  {
    Page pg;
    pool_.read_page(head_, pg);
    SlottedPage sp(pg);
    if (auto slot = sp.insert(rec)) {
      pool_.write_page(head_, pg);
      return RID{head_, *slot};
    }
  }
  // Head is full: prepend a fresh page and insert there (it must fit).
  const PageId id = new_page();
  Page pg;
  pool_.read_page(id, pg);
  SlottedPage sp(pg);
  auto slot = sp.insert(rec);
  check_that(slot.has_value(), "record did not fit in an empty page");
  pool_.write_page(id, pg);
  return RID{id, *slot};
}

std::vector<std::uint8_t> Heap::get(RID rid) const {
  Page pg;
  pool_.read_page(rid.page, pg);
  SlottedPage sp(pg);
  auto span = sp.read(rid.slot);
  check_that(span.has_value(), "get: no live record at this RID");
  return std::vector<std::uint8_t>(span->begin(), span->end());
}

bool Heap::erase(RID rid) {
  Page pg;
  pool_.read_page(rid.page, pg);
  SlottedPage sp(pg);
  const bool ok = sp.erase(rid.slot);
  if (ok) pool_.write_page(rid.page, pg);
  return ok;
}

}  // namespace labdb
