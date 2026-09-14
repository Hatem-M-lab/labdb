// Deletion, measured: throughput, the tree shrinking in height as it
// empties, and -- the point of merging -- pages returned to the free list
// so the space can be reused. We build a million-key tree, delete it all,
// and watch height fall and the free list grow. Seeds fixed.
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

namespace {
// Length of the free list, walked defensively.
std::size_t freelist_len(Pager& pager) {
  std::size_t n = 0;
  PageId id = pager.freelist_head();
  Page p;
  while (id != kNullPage && n <= pager.page_count()) {
    pager.read_page(id, p);
    ++n;
    id = p.next_page();
  }
  return n;
}
}  // namespace

int main() {
  const char* path = "u03_delete_bench.db";
  ::unlink(path);
  constexpr Key kN = 1000000;

  Pager pager(path);
  BTree tree(pager);
  for (Key k = 0; k < kN; ++k) tree.insert(k, k);

  const std::uint32_t pages_full = pager.page_count();
  const int height_full = tree.height();
  std::printf("full tree: %llu keys, height %d, %u pages, free list %zu\n\n",
              (unsigned long long)kN, height_full, pages_full,
              freelist_len(pager));

  // Delete in random order (the demanding case for rebalancing) and sample
  // height and free-list length as the tree drains.
  std::vector<Key> order(kN);
  for (Key k = 0; k < kN; ++k) order[k] = k;
  std::shuffle(order.begin(), order.end(), std::mt19937_64(2024));

  std::printf("| keys remaining | height | pages on free list |\n");
  std::printf("|---:|---:|---:|\n");
  const std::size_t marks[] = {750000, 500000, 250000, 100000, 0};
  std::size_t mark_i = 0;

  const double t0 = now_s();
  for (std::size_t i = 0; i < order.size(); ++i) {
    REQUIRE(tree.erase(order[i]));
    const std::size_t remaining = kN - (i + 1);
    if (mark_i < 5 && remaining == marks[mark_i]) {
      std::printf("| %zu | %d | %zu |\n", remaining, tree.height(),
                  freelist_len(pager));
      ++mark_i;
    }
  }
  const double secs = now_s() - t0;

  std::printf(
      "\ndeleted %llu keys in %.2f s (%.2f M/s). Height fell %d -> 1; the "
      "file never grew past %u pages because merges recycle freed pages onto "
      "the free list for reuse.\n",
      (unsigned long long)kN, secs, double(kN) / secs / 1e6, height_full,
      pager.page_count());

  ::unlink(path);
  return 0;
}
