// Challenge 9.6, a closer look at the last row: why does the nested loop's
// cost per pair rise at n = 100,000? Its inner loop walks a buffer of rows
// over and over, so walk a buffer shaped exactly like it -- Row objects of two
// Fields, allocated in order, as NestedLoopJoin::open() builds them -- and
// time one key comparison per row as the buffer grows. If the per-row cost
// climbs with the buffer's size alone, the cause is where the buffer lives
// in the memory hierarchy, not the join.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "harness.hpp"
#include "record.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::now_s;

int main() {
  std::printf("walking a buffer of N two-field rows, one key comparison per row; "
              "median of 3 runs of ~400 million visits\n\n");
  std::printf("  %8s  %12s  %20s\n", "N", "buffer size", "ns per row visited");
  for (int n : {10000, 25000, 50000, 75000, 100000, 150000, 200000}) {
    std::vector<Row> inner;
    inner.reserve(n);
    for (int i = 0; i < n; ++i) inner.push_back({Field::Int64(i), Field::Int32(i % 1000)});
    const long long kVisits = 400'000'000LL;
    const int passes = static_cast<int>(kVisits / n);
    std::vector<double> t;
    std::size_t hits = 0;
    for (int rep = 0; rep < 3; ++rep) {
      const double t0 = now_s();
      for (int p = 0; p < passes; ++p) {
        const std::int64_t probe = (static_cast<std::int64_t>(p) * 7919) % n;
        for (const Row& r : inner) hits += (!r[0].is_null && r[0].i64 == probe);
      }
      t.push_back((now_s() - t0) * 1e9 / (static_cast<double>(passes) * n));
    }
    do_not_optimize(hits);
    REQUIRE(hits == static_cast<std::size_t>(3) * passes);  // one match per pass
    std::sort(t.begin(), t.end());
    // each row: its vector header in `inner`, plus a heap block of 2 Fields
    // (and the allocator's 16-byte header)
    const double mb = n * (sizeof(Row) + 2 * sizeof(Field) + 16) / 1e6;
    std::printf("  %8d  %9.1f MB  %20.2f\n", n, mb, t[1]);
  }
  return 0;
}
