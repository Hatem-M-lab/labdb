// Range scans, measured. A B+Tree answers "every key in [lo, hi] in order"
// by descending once and then walking the leaf chain -- so the cost is the
// tree height plus one page per leaf's worth of results, and it depends on
// how many rows you asked for, NOT on how big the table is. This benchmark
// runs ranges of growing width over a fixed million-key tree and shows the
// page reads tracking the result size. Seeds fixed.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

int main() {
  const char* path = "u03_range_bench.db";
  ::unlink(path);
  constexpr Key kN = 1000000;

  Pager pager(path);
  BTree tree(pager);
  // Dense keys 0..N-1 so a width-W range holds ~W results.
  for (Key k = 0; k < kN; ++k) tree.insert(k, k);
  std::printf("tree: %llu keys, height %d, %u pages. Leaf holds %u entries.\n\n",
              (unsigned long long)kN, tree.height(), pager.page_count(),
              kLeafMaxEntries);

  std::printf("| range width | rows returned | pages read | ns/row |\n");
  std::printf("|---:|---:|---:|---:|\n");

  std::mt19937_64 rng(31337);
  const Key widths[] = {1, 100, 1000, 10000, 100000};
  for (Key w : widths) {
    // Average over several random windows of this width.
    const int trials = w >= 10000 ? 20 : 200;
    std::uint64_t total_reads = 0, total_rows = 0;
    const double t0 = now_s();
    for (int t = 0; t < trials; ++t) {
      const Key lo = rng() % (kN - w);
      const Key hi = lo + w - 1;
      pager.reset_read_count();
      std::uint64_t rows = 0;
      for (auto c = tree.seek(lo); c.valid() && c.key() <= hi; c.next()) ++rows;
      total_reads += pager.read_count();
      total_rows += rows;
    }
    const double secs = now_s() - t0;
    const double avg_reads = double(total_reads) / trials;
    const double avg_rows = double(total_rows) / trials;
    const double ns_row =
        total_rows ? secs / double(total_rows) * 1e9 : 0.0;
    std::printf("| %llu | %.0f | %.1f | %.1f |\n", (unsigned long long)w,
                avg_rows, avg_reads, ns_row);
  }

  std::printf(
      "\npages read = height (%d to find lo) + about rows/%u (one read per "
      "leaf of results): cost scales with the answer, not the table.\n",
      tree.height(), kLeafMaxEntries);

  ::unlink(path);
  return 0;
}
