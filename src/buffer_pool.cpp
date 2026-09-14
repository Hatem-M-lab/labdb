#include "buffer_pool.hpp"

#include "common.hpp"  // fail, check_that

namespace labdb {

BufferPool::BufferPool(Pager& pager, std::size_t capacity) : pager_(pager) {
  check_that(capacity >= 1, "buffer pool needs at least one frame");
  frames_.resize(capacity);
}

BufferPool::~BufferPool() {
  // Last-chance flush: write back anything still dirty so a caller that
  // forgot to sync() does not silently lose an update. (A destructor cannot
  // report I/O errors usefully; sync() is the checked path.)
  for (auto& f : frames_)
    if (f.id != kNullPage && f.dirty) pager_.write_page(f.id, f.page);
}

// Clock / second chance. Sweep the frames in a ring: never evict a pinned
// frame; give a frame whose ref bit is set one reprieve (clear the bit and
// move on); evict the first unpinned frame whose ref bit is already clear.
std::size_t BufferPool::choose_victim() {
  const std::size_t n = frames_.size();
  for (std::size_t scanned = 0; scanned < 2 * n + 1; ++scanned) {
    const std::size_t here = hand_;
    hand_ = (hand_ + 1) % n;
    Frame& f = frames_[here];
    if (f.pins > 0) continue;                 // pinned: off limits
    if (f.ref) { f.ref = false; continue; }   // recently used: second chance
    return here;                              // cold and unpinned: evict it
  }
  fail("buffer pool exhausted: every frame is pinned "
       "(capacity is smaller than the working pin set)");
}

void BufferPool::evict(std::size_t fi) {
  Frame& f = frames_[fi];
  if (f.id == kNullPage) return;
  if (f.dirty && writeback_) {
    pager_.write_page(f.id, f.page);  // write the modified page back first
    ++disk_writes_;
  }
  table_.erase(f.id);
  ++evictions_;
  f.id = kNullPage;
  f.dirty = false;
  f.ref = false;
}

std::size_t BufferPool::obtain(PageId id, bool load_from_disk) {
  auto it = table_.find(id);
  if (it != table_.end()) {           // resident: a cache hit
    Frame& f = frames_[it->second];
    ++f.pins;
    f.ref = true;
    return it->second;
  }
  // Miss. Prefer an empty frame; otherwise evict a clock victim to reuse one.
  std::size_t fi = frames_.size();
  for (std::size_t i = 0; i < frames_.size(); ++i)
    if (frames_[i].id == kNullPage && frames_[i].pins == 0) {
      fi = i;
      break;
    }
  if (fi == frames_.size()) {
    fi = choose_victim();
    evict(fi);
  }

  Frame& f = frames_[fi];
  if (load_from_disk) {
    pager_.read_page(id, f.page);     // the page fault: one real disk read
    ++disk_reads_;
  } else {
    f.page = Page{};                  // fresh zeroed frame (a new allocation)
    f.page.set_id(id);                // header id matches so it flushes clean
  }
  f.id = id;
  f.pins = 1;
  f.dirty = false;
  f.ref = true;
  table_[id] = fi;
  return fi;
}

Page& BufferPool::fetch_pin(PageId id) {
  return frames_[obtain(id, /*load_from_disk=*/true)].page;
}

void BufferPool::unpin(PageId id, bool dirty) {
  auto it = table_.find(id);
  check_that(it != table_.end(), "unpin of a page that is not resident");
  Frame& f = frames_[it->second];
  check_that(f.pins > 0, "unpin of a frame that is not pinned");
  if (dirty) f.dirty = true;
  --f.pins;
}

void BufferPool::read_page(PageId id, Page& out) {
  ++logical_reads_;
  const std::size_t fi = obtain(id, /*load_from_disk=*/true);
  out = frames_[fi].page;             // copy the resident frame out
  --frames_[fi].pins;                 // release (a read leaves the frame clean)
}

void BufferPool::write_page(PageId id, const Page& in) {
  // A full-page overwrite: when the page is not resident we do NOT read its
  // old contents -- we are replacing every byte.
  const bool resident = table_.count(id) != 0;
  const std::size_t fi = obtain(id, /*load_from_disk=*/resident);
  frames_[fi].page = in;              // copy the caller's bytes into the frame
  frames_[fi].dirty = true;
  --frames_[fi].pins;
}

PageId BufferPool::allocate_page() {
  const PageId id = pager_.allocate_page();
  const std::size_t fi = obtain(id, /*load_from_disk=*/false);  // fresh frame
  frames_[fi].dirty = true;           // a new page must reach disk eventually
  --frames_[fi].pins;
  return id;
}

void BufferPool::free_page(PageId id) {
  auto it = table_.find(id);
  if (it != table_.end()) {           // drop the frame: freed contents are void
    Frame& f = frames_[it->second];
    check_that(f.pins == 0, "free_page of a pinned page");
    f.id = kNullPage;
    f.dirty = false;                  // no write-back for a freed page
    f.ref = false;
    f.page = Page{};
    table_.erase(it);
  }
  pager_.free_page(id);
}

void BufferPool::sync() {
  for (auto& f : frames_)
    if (f.id != kNullPage && f.dirty) {
      pager_.write_page(f.id, f.page);
      ++disk_writes_;
      f.dirty = false;
    }
  pager_.sync();
}

}  // namespace labdb
