// Forensic-trap regression test. The broken-sibling-chain bug shows up when
// deletion merges leaves: if the survivor doesn't inherit the freed leaf's
// next pointer, a range scan stops early and loses every key past the merge.
// This test forces thousands of merges by deleting most of a large tree,
// then walks the entire chain and asserts it still reaches every remaining
// key in order. Under the bug, the scan count would fall short.
#include <unistd.h>

#include <cstdio>
#include <set>
#include <vector>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace labdb;

int main() {
  const char* path = "u03_scan_regression_test.db";
  ::unlink(path);
  Pager pager(path);
  BTree tree(pager);

  // Insert a dense ascending run so leaves are packed and adjacent, then
  // delete 7 of every 8 keys. Deleting most keys from packed leaves forces
  // heavy merging -- exactly the operation the trap corrupts.
  constexpr Key kN = 400000;
  for (Key k = 0; k < kN; ++k) tree.insert(k, k);

  std::set<Key> survivors;
  for (Key k = 0; k < kN; ++k) {
    if (k % 8 == 0) {
      survivors.insert(k);  // keep every 8th key
    } else {
      REQUIRE(tree.erase(k));
    }
  }

  // A full scan must reach exactly the survivors, in order. If any merge
  // severed the chain, the scan ends early and this count is short.
  std::vector<Key> scanned;
  for (auto c = tree.seek(0); c.valid(); c.next()) scanned.push_back(c.key());
  REQUIRE(scanned.size() == survivors.size());
  std::size_t i = 0;
  Key prev = 0;
  bool first = true;
  for (Key k : survivors) {
    REQUIRE(scanned[i] == k);  // same keys, same order
    if (!first) REQUIRE(prev < k);
    prev = k;
    first = false;
    ++i;
  }

  // Spot-check that a bounded range across many former merge points is also
  // exact -- the scan doesn't just reach the end, it reaches it correctly.
  std::vector<Key> rng_got;
  for (auto c = tree.seek(100000); c.valid() && c.key() <= 300000; c.next())
    rng_got.push_back(c.key());
  std::size_t rng_want = 0;
  for (Key k : survivors)
    if (k >= 100000 && k <= 300000) ++rng_want;
  REQUIRE(rng_got.size() == rng_want);

  std::printf(
      "u03_scan_regression_test: PASS (deleted %llu of %llu keys forcing "
      "thousands of merges; full scan still reaches all %zu survivors in "
      "order, height %d)\n",
      (unsigned long long)(kN - survivors.size()), (unsigned long long)kN,
      survivors.size(), tree.height());

  ::unlink(path);
  return 0;
}
