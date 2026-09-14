// THE BUG, preserved for study: evicting a dirty frame without writing it
// back. A buffer pool keeps modified pages in memory and must write them to
// disk before reusing their frame. Skip that write and the update is not
// saved -- but nothing fails immediately. The page keeps its new value in
// memory until its frame is needed for something else; then the frame is
// reused, the change is dropped, and the page silently reverts to whatever
// was last on disk. Writes vanish once the working set outgrows the pool.
//
// This program updates a page, then touches enough other pages to force that
// page's frame to be evicted, and reads it back -- with write-back on (the
// correct pool) and off (the bug).
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Itests
//        tools/u04_lost_update_demo.cpp src/io.cpp src/pager.cpp
//        src/buffer_pool.cpp -o bin/u04_lost_update_demo
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "buffer_pool.hpp"
#include "common.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

constexpr std::size_t kMarkerOff = kPageHeaderSize;  // where we stash a marker

Page make_page(PageId id, std::uint64_t marker) {
  Page p;
  p.set_id(id);
  p.set_type(PageType::kSlotted);  // any non-meta type
  store_u64(p.data() + kMarkerOff, marker);
  return p;
}

std::uint64_t read_marker(BufferPool& pool, PageId id) {
  Page p;
  pool.read_page(id, p);
  return load_u64(p.data() + kMarkerOff);
}

// Run the same sequence with write-back either on (correct) or off (buggy),
// returning the value the page holds after its frame was evicted and reloaded.
std::uint64_t run(bool buggy) {
  const char* path = buggy ? "u04_demo_buggy.db" : "u04_demo_ok.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, /*capacity=*/3);

  // Six pages, each with an initial marker id*100, all flushed to disk. The
  // pool behaves correctly during this setup.
  std::vector<PageId> ids;
  for (int k = 0; k < 6; ++k) {
    const PageId id = pool.allocate_page();
    ids.push_back(id);
    pool.write_page(id, make_page(id, static_cast<std::uint64_t>(id) * 100));
  }
  pool.sync();  // every initial marker is now on stable storage

  // Only now do we break write-back, so the bug is isolated to the eviction
  // of a page we are about to modify -- not the setup.
  if (buggy) pool.demo_disable_writeback();

  const PageId victim = ids[2];
  const std::uint64_t before = read_marker(pool, victim);

  // Update the victim page in memory. It is now dirty in the pool.
  pool.write_page(victim, make_page(victim, 999));

  // Touch the other pages until the victim's frame is reused for one of them.
  for (int pass = 0; pass < 4 && pool.is_resident(victim); ++pass)
    for (PageId id : ids)
      if (id != victim) read_marker(pool, id);

  const std::uint64_t after = read_marker(pool, victim);
  std::printf(
      "  %-8s pool(3 frames): page %llu was %llu, updated to 999, then its "
      "frame was evicted;\n           reading it back now gives %llu%s\n",
      buggy ? "BUGGY" : "CORRECT", (unsigned long long)victim,
      (unsigned long long)before, (unsigned long long)after,
      after == 999 ? "  (the update survived)"
                   : "  <-- the update was LOST; the page reverted");
  ::unlink(path);
  return after;
}

}  // namespace

int main() {
  std::printf(
      "A dirty page must be written back before its frame is reused. Here a "
      "3-frame pool holds 6 pages, so eviction is forced.\n\n");
  run(/*buggy=*/false);
  std::printf("\n");
  run(/*buggy=*/true);
  return 0;
}
