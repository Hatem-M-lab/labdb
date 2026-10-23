// The steps of tests/anatomy.hpp, one at a time, for counting instructions:
//
//   u12_row_probe build DB          build the events table in DB (run natively)
//   u12_row_probe STEP N DB         run one step over the first N rows of DB
//
// scripts/u12_anatomy_counts.sh runs each step under valgrind twice, over N
// rows and over none, and divides the difference by N: the instructions one
// row costs at that step, the same on every run.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "anatomy.hpp"
#include "pager.hpp"

using namespace labdb;
using namespace labdb::test;

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "build") {
    Pager pager(argv[2]);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    Executor e(cat);
    build_events(e);
    return 0;
  }
  if (argc != 4) {
    std::fprintf(stderr, "usage: u12_row_probe build DB | u12_row_probe STEP N DB\n");
    return 2;
  }
  const std::string want = argv[1];
  const std::size_t n = std::strtoull(argv[2], nullptr, 10);
  Pager pager(argv[3]);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  for (Step s : {Step::kWalk, Step::kZeroPage, Step::kCopyPage, Step::kCopyRecord, Step::kDecode, Step::kScanCopy, Step::kPin,
                 Step::kDecodeInto, Step::kScanPin, Step::kBatches})
    if (want == step_name(s)) {
      run_step(s, pool, cat, kAnatomyRows);  // every page into the pool first, uncounted
      const std::uint64_t sum = run_step(s, pool, cat, n);
      std::printf("%s %zu %llu\n", step_name(s), n, static_cast<unsigned long long>(sum));
      return 0;
    }
  std::fprintf(stderr, "no such step: %s\n", want.c_str());
  return 2;
}
