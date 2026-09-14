#pragma once
// BufferPool -- a fixed set of in-memory page frames cached over a Pager, so
// that hot pages are served from memory instead of a pread on every access.
// Unit 4.
//
// The pain this answers is the one Unit 3 ended on: every read_page was a
// real system call, and the upper levels of the B+Tree -- touched on every
// single query -- were re-read from the OS again and again. A buffer pool
// keeps those pages resident.
//
// Replacement policy: clock (second chance). Dirty frames are written back
// on eviction and on flush. A pinned frame is never chosen as a victim.
//
// It deliberately presents the SAME read/write/allocate/free/root/meter
// surface the pager did, so the B+Tree sits on top of it with only a change
// of type: the tree asks for pages exactly as before and is transparently
// cached. Underneath, a Pager still owns the file, the free list, and the
// meta page (which is never cached -- it stays consistent on disk).

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "page.hpp"
#include "pager.hpp"

namespace labdb {

class BufferPool {
 public:
  // Cache up to `capacity` frames over `pager`. capacity must exceed the
  // most pages any single operation pins at once -- a small handful for the
  // B+Tree (a root-to-leaf path plus a sibling or two).
  BufferPool(Pager& pager, std::size_t capacity);
  ~BufferPool();  // flushes dirty frames so no update is lost on shutdown

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // ---- low-level frame API: the real buffer-pool interface ----
  // Pin the frame holding `id` (faulting it in on a miss, evicting a victim
  // if the pool is full) and return a reference to its bytes. The frame will
  // not be evicted until unpinned. Every fetch_pin must be matched by an
  // unpin; pass dirty=true to unpin if you modified the page.
  Page& fetch_pin(PageId id);
  void unpin(PageId id, bool dirty);

  // ---- Pager-compatible convenience surface: what the B+Tree calls ----
  // Copy-based read/write built on pin + copy + unpin. A read is served from
  // the frame when resident, otherwise faulted in once.
  void read_page(PageId id, Page& out);
  void write_page(PageId id, const Page& in);
  PageId allocate_page();
  void free_page(PageId id);
  void sync();  // flush dirty frames, then fsync via the pager

  PageId btree_root() const { return pager_.btree_root(); }
  void set_btree_root(PageId id) { pager_.set_btree_root(id); }

  // Logical read meter: counts every read the caller requests, hit or miss,
  // so the B+Tree's "pages per lookup" figure is unchanged from Unit 2. The
  // disk counters below report what actually touched the file.
  std::uint64_t read_count() const { return logical_reads_; }
  void reset_read_count() { logical_reads_ = 0; }

  std::uint32_t page_count() const { return pager_.page_count(); }
  PageId freelist_head() const { return pager_.freelist_head(); }
  std::uint32_t freelist_length() { return pager_.freelist_length(); }

  // ---- cache instrumentation: the unit's payoff ----
  std::uint64_t disk_reads() const { return disk_reads_; }
  std::uint64_t disk_writes() const { return disk_writes_; }
  std::uint64_t evictions() const { return evictions_; }
  void reset_stats() {
    logical_reads_ = disk_reads_ = disk_writes_ = evictions_ = 0;
  }
  std::size_t capacity() const { return frames_.size(); }
  bool is_resident(PageId id) const { return table_.count(id) != 0; }

  // Demonstration switch ONLY (Challenge 4.4's forensic trap): with write-
  // back disabled, evicting a dirty frame drops it instead of writing it --
  // reproducing the classic lost-update bug. Real use never calls this; the
  // fix is exactly that eviction always writes dirty frames back first.
  void demo_disable_writeback() { writeback_ = false; }

 private:
  struct Frame {
    Page page;
    PageId id = kNullPage;  // page held here, or kNullPage if the frame is empty
    int pins = 0;           // in-use count; > 0 means un-evictable
    bool dirty = false;     // modified since load; must be written back
    bool ref = false;       // clock "recently used" bit (second chance)
  };

  // Find or make a pinned frame for `id`. On a miss, either read the page
  // from the pager (load_from_disk) or leave the bytes for the caller to
  // fill (allocate / full overwrite). Returns the frame index.
  std::size_t obtain(PageId id, bool load_from_disk);
  std::size_t choose_victim();  // clock sweep; returns an unpinned frame index
  void evict(std::size_t fi);   // write back if dirty, then drop the mapping

  Pager& pager_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, std::size_t> table_;
  std::size_t hand_ = 0;
  bool writeback_ = true;

  std::uint64_t logical_reads_ = 0;
  std::uint64_t disk_reads_ = 0;
  std::uint64_t disk_writes_ = 0;
  std::uint64_t evictions_ = 0;
};

}  // namespace labdb
