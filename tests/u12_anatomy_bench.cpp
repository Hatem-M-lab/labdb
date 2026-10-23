// Challenges 12.1 and 12.2: where a scanned row's time goes.
//
// Unit 11 measured a row of a SeqScan at about 140 ns. Here the scan is taken
// apart and built up again one step at a time over the same 200,000 rows,
// every step a loop of its own (tests/anatomy.hpp), so that each line's time
// minus the line before it is what that step costs. First the scan as Unit 11
// ran it; then as Unit 12 runs it, the page pinned instead of copied and the
// row decoded in place; then in batches. Every page is in the buffer pool.
// All the steps are timed in turn, round after round (tests/interleave.hpp),
// so that a slow moment of the machine falls on every step alike. Medians of
// 15 rounds, with the fastest and slowest.
//
// The same steps, counted in instructions instead of timed, are in
// scripts/u12_anatomy_counts.sh (valgrind, through tools/u12_row_probe.cpp).
#include <unistd.h>

#include <cstdio>
#include <functional>
#include <vector>

#include "anatomy.hpp"
#include "harness.hpp"
#include "interleave.hpp"
#include "pager.hpp"

using namespace labdb;
using namespace labdb::test;

int main() {
  const char* path = "u12_anatomy.db";
  ::unlink(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    build_events(e);
  }
  Pager pager(path);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  const TableInfo info = *cat.find_table("events");
  std::printf("\nevents: %d rows of (id, v, w); the heap is %u pages, %.0f rows a page, all in the pool\n",
              kAnatomyRows, info.heap_pages, static_cast<double>(kAnatomyRows) / info.heap_pages);

  // Every step, and the two allocations alone for scale, timed in turn.
  const Step steps[] = {Step::kWalk, Step::kZeroPage, Step::kCopyPage, Step::kCopyRecord, Step::kDecode,
                        Step::kScanCopy, Step::kPin, Step::kDecodeInto, Step::kScanPin, Step::kBatches};
  std::uint64_t sum = 0;
  std::vector<std::function<void()>> runs;
  for (Step s : steps) runs.push_back([&, s] { sum += run_step(s, pool, cat, kAnatomyRows); });
  runs.push_back([] {
    for (int i = 0; i < kAnatomyRows; ++i) {
      std::vector<std::uint8_t> v(28);
      do_not_optimize(v.data());
      Row r(3);
      do_not_optimize(r.data());
    }
  });
  std::vector<Timing> t = interleaved(runs, 15);
  do_not_optimize(sum);
  for (Timing& x : t) x = {x.median * 1e9 / kAnatomyRows, x.lo * 1e9 / kAnatomyRows, x.hi * 1e9 / kAnatomyRows};
  const Timing &walk = t[0], &zero = t[1], &page = t[2], &rec = t[3], &dec = t[4], &scan_copy = t[5], &pin = t[6],
               &into = t[7], &scan_pin = t[8], &batches = t[9], &alloc = t[10];
  const auto line = [](const char* what, const Timing& x, const Timing* before) {
    std::printf("  %-58s %6.1f", what, x.median);
    if (before) std::printf("  %+6.1f", x.median - before->median);
    else std::printf("        ");
    std::printf("   (%.1f-%.1f)\n", x.lo, x.hi);
  };

  std::printf("\na scanned row, one step at a time (ns per row: median of 15 rounds, then what the step added)\n");
  std::printf("\n  as Unit 11 reads it                                          ns/row   added   (fastest-slowest)\n");
  line("walk the table's B+Tree: each entry's key and RID", walk, nullptr);
  line("+ a fresh Page for the row: 4 KiB zero-filled", zero, &walk);
  line("+ the row's heap page copied into it out of the pool", page, &zero);
  line("+ the record copied out of that into a new vector", rec, &page);
  line("+ decoded into a newly allocated Row", dec, &rec);
  line("SeqScan::next() reading this way (Unit 11's scan)", scan_copy, &dec);
  std::printf("\n  for scale: a record's vector and a 3-field Row, allocated and freed, alone: %.1f ns\n",
              alloc.median);

  std::printf("\n  as Unit 12 reads it\n");
  line("walk + pin the row's page (once a page), find the record", pin, &walk);
  line("+ decode it in place, into the same Row", into, &pin);
  line("SeqScan::next() reading this way (Unit 12's row path)", scan_pin, &into);
  line("SeqScan::next_batch(), 1,024 rows a call (Unit 12's batches)", batches, nullptr);

  std::printf("\n  per row, Unit 11's scan against Unit 12's: %.1f ns -> %.1f ns a row (%.1fx); in batches %.1f ns (%.1fx)\n",
              scan_copy.median, scan_pin.median, scan_copy.median / scan_pin.median, batches.median,
              scan_copy.median / batches.median);
  return 0;
}
