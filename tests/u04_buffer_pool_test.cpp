// Unit 4 buffer pool correctness. Four properties, each checked directly:
//  1. persistence through eviction -- more pages than frames still round-trip;
//  2. a pinned frame is never evicted, and is served as a hit while pinned;
//  3. a dirty page is written back on eviction (the trap's positive control);
//  4. a working set that fits the pool is served from memory on re-access.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "buffer_pool.hpp"
#include "common.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {
constexpr std::size_t kOff = kPageHeaderSize;

Page marked(PageId id, std::uint64_t m) {
  Page p;
  p.set_id(id);
  p.set_type(PageType::kSlotted);
  store_u64(p.data() + kOff, m);
  return p;
}
std::uint64_t marker(BufferPool& pool, PageId id) {
  Page p;
  pool.read_page(id, p);
  return load_u64(p.data() + kOff);
}
}  // namespace

int main() {
  const char* path = "u04_buffer_pool_test.db";
  ::unlink(path);

  // ---- 1. persistence through eviction ----
  std::vector<PageId> ids;
  std::uint64_t evictions_seen = 0;
  {
    Pager pager(path);
    BufferPool pool(pager, /*capacity=*/8);  // far fewer frames than pages
    for (int k = 0; k < 200; ++k) {
      const PageId id = pool.allocate_page();
      ids.push_back(id);
      pool.write_page(id, marked(id, static_cast<std::uint64_t>(id) * 7 + 1));
    }
    // With 8 frames and 200 pages, the great majority were evicted and must
    // have been written back. Read them all: every marker must be intact.
    for (PageId id : ids) REQUIRE(marker(pool, id) == static_cast<std::uint64_t>(id) * 7 + 1);
    REQUIRE(pool.evictions() > 0);  // eviction really happened
    evictions_seen = pool.evictions();
    pool.sync();
  }
  {  // reopen with a brand-new pool: the data is on disk, not in any frame
    Pager pager(path);
    BufferPool pool(pager, 8);
    for (PageId id : ids)
      REQUIRE(marker(pool, id) == static_cast<std::uint64_t>(id) * 7 + 1);
    std::printf("PASS persistence: 200 pages through an 8-frame pool round-trip (%llu evictions, all written back)\n",
                (unsigned long long)evictions_seen);
  }

  // ---- 2. pinning prevents eviction; a pinned page is a hit ----
  {
    Pager pager(path);
    BufferPool pool(pager, 4);
    const PageId pinned = ids[0];
    Page& p = pool.fetch_pin(pinned);  // hold a pin
    (void)p;
    pool.reset_stats();
    // Fault in many other pages -- far more than the 4 frames. The pinned
    // frame must never be chosen as a victim.
    for (int k = 1; k < 100; ++k) {
      (void)marker(pool, ids[k]);
      REQUIRE(pool.is_resident(pinned));  // still here, every step
    }
    // Accessing the pinned page is a hit: no disk read for it.
    const std::uint64_t reads_before = pool.disk_reads();
    (void)marker(pool, pinned);
    REQUIRE(pool.disk_reads() == reads_before);
    pool.unpin(pinned, /*dirty=*/false);
    // Once unpinned, it becomes an ordinary eviction candidate again.
    for (int k = 1; k < 100; ++k) (void)marker(pool, ids[k]);
    REQUIRE(!pool.is_resident(pinned));
    std::printf("PASS pinning: a pinned frame survived 99 competing faults, then was evictable once unpinned\n");
  }

  // ---- 3. dirty write-back on eviction (positive control for the trap) ----
  {
    Pager pager(path);
    BufferPool pool(pager, 3);
    const PageId a = ids[5];
    pool.write_page(a, marked(a, 424242));   // dirty in the pool
    for (int pass = 0; pass < 4 && pool.is_resident(a); ++pass)
      for (int k = 0; k < 10; ++k)
        if (ids[k] != a) (void)marker(pool, ids[k]);  // force a's eviction
    REQUIRE(!pool.is_resident(a));            // it was evicted...
    REQUIRE(marker(pool, a) == 424242);       // ...and written back correctly
    std::printf("PASS write-back: an updated page evicted under pressure kept its new value\n");
  }

  // ---- 4. a working set that fits is served from memory ----
  {
    Pager pager(path);
    BufferPool pool(pager, 32);
    // Warm: touch 32 pages once (fills the pool). Then touch them again.
    for (int k = 0; k < 32; ++k) (void)marker(pool, ids[k]);
    pool.reset_stats();
    for (int rep = 0; rep < 10; ++rep)
      for (int k = 0; k < 32; ++k) (void)marker(pool, ids[k]);
    REQUIRE(pool.disk_reads() == 0);          // 320 accesses, zero disk reads
    REQUIRE(pool.read_count() == 320);        // all logical, all hits
    std::printf("PASS caching: 320 accesses to a 32-page working set in a 32-frame pool did 0 disk reads\n");
  }

  ::unlink(path);
  std::printf("u04_buffer_pool_test: PASS\n");
  return 0;
}
