// Forensic-trap regression test. The disappearing-median bug shows up the
// instant a leaf splits: the boundary key becomes unfindable. This test
// drives the real engine through hundreds of splits at every level and
// asserts that every key -- especially the separators promoted into
// internal nodes -- is still reachable by search. If anyone ever changes a
// leaf split from copy-up to move-up, this fails immediately.
#include <unistd.h>

#include <cstdio>
#include <vector>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "buffer_pool.hpp"
#include "pager.hpp"

using namespace labdb;

int main() {
  const char* path = "u02_split_regression_test.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 4096);
  BTree tree(pool);

  // Insert enough ascending keys to build a multi-level tree with many
  // splits. Ascending order makes the boundary keys land exactly on split
  // points, which is precisely where a move-up bug drops them.
  constexpr Key kN = 300000;
  for (Key k = 0; k < kN; ++k) REQUIRE(tree.insert(k, k ^ 0x5a5a5a5a));
  REQUIRE(tree.height() >= 3);

  // Every single key must be findable. Under the move-up bug, ~1 key per
  // leaf split (there are thousands) would be missing.
  std::size_t missing = 0;
  for (Key k = 0; k < kN; ++k) {
    auto got = tree.search(k);
    if (!got || *got != (k ^ 0x5a5a5a5a)) ++missing;
  }
  REQUIRE(missing == 0);

  // Collect the separators actually promoted into internal nodes and prove
  // each one still resolves to its value in a leaf -- the direct assertion
  // that promotion was a copy, not a move.
  Page p;
  std::size_t separators = 0, sep_missing = 0;
  for (PageId id = 1; id < pool.page_count(); ++id) {
    pool.read_page(id, p);
    if (p.type() != PageType::kBTreeInternal) continue;
    InternalNode n(p);
    for (std::uint16_t i = 0; i < n.count(); ++i) {
      const Key sep = n.key_at(i);
      ++separators;
      auto got = tree.search(sep);
      if (!got || *got != (sep ^ 0x5a5a5a5a)) ++sep_missing;
    }
  }
  REQUIRE(separators > 0);
  REQUIRE(sep_missing == 0);

  std::printf(
      "u02_split_regression_test: PASS (%llu keys, height %d, %zu promoted "
      "separators all still in leaves, 0 lost to splits)\n",
      (unsigned long long)kN, tree.height(), separators);

  ::unlink(path);
  return 0;
}
