// The headline measurement: point lookup, before and after an index.
// Unit 1 ended on one number -- to find a record among a million, the
// engine had to scan up to 16,950 pages. Here we build a B+Tree over a
// million keys and count the pages a lookup actually reads. Seeds fixed.
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
  const char* path = "u02_search_bench.db";
  ::unlink(path);
  constexpr Key kN = 1000000;

  Pager pager(path);
  BTree tree(pager);

  // Build: a million random keys.
  std::mt19937_64 rng(424242);
  std::vector<Key> keys;
  keys.reserve(kN);
  const double t_build0 = now_s();
  for (Key i = 0; i < kN; ++i) {
    const Key k = rng();
    tree.insert(k, k ^ 0xd1b54a32d192ed03ULL);
    keys.push_back(k);
  }
  const double build_s = now_s() - t_build0;

  // Count leaf and internal pages (the tree's data pages vs routing pages).
  Page p;
  std::size_t leaves = 0, internal = 0;
  for (PageId id = 1; id < pager.page_count(); ++id) {
    pager.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) ++leaves;
    else if (p.type() == PageType::kBTreeInternal) ++internal;
  }

  std::printf("built B+Tree: %llu keys, height %d, %zu leaves + %zu internal "
              "= %u pages, in %.2f s\n\n",
              (unsigned long long)kN, tree.height(), leaves, internal,
              pager.page_count(), build_s);

  // Pick a random sample of present keys and a disjoint set of absent keys.
  std::shuffle(keys.begin(), keys.end(), rng);
  constexpr int kSample = 100000;

  // Pages read per lookup: reset the pager's meter, do one search, read it
  // back. This is the exact number of pread() calls the lookup made.
  std::uint64_t total_reads_present = 0;
  const double t_p0 = now_s();
  for (int i = 0; i < kSample; ++i) {
    pager.reset_read_count();
    volatile auto v = tree.search(keys[static_cast<std::size_t>(i)]);
    (void)v;
    total_reads_present += pager.read_count();
  }
  const double present_s = now_s() - t_p0;

  std::uint64_t total_reads_absent = 0;
  std::mt19937_64 arng(999);
  const double t_a0 = now_s();
  for (int i = 0; i < kSample; ++i) {
    pager.reset_read_count();
    // odd synthetic keys unlikely to collide with the 64-bit random set
    volatile auto v = tree.search((static_cast<Key>(i) << 1) | 1);
    (void)v;
    total_reads_absent += pager.read_count();
  }
  const double absent_s = now_s() - t_a0;

  const double reads_present = double(total_reads_present) / kSample;
  const double reads_absent = double(total_reads_absent) / kSample;
  const double ns_present = present_s / kSample * 1e9;
  const double ns_absent = absent_s / kSample * 1e9;

  // The Unit 1 baseline: a point lookup by full scan reads every data page
  // until it finds the key (worst case all of them). For a million
  // 64-byte records that was 16,950 pages.
  constexpr double kU1ScanPages = 16950.0;

  std::printf("| lookup | pages read (measured) | warm latency | vs Unit 1 full scan |\n");
  std::printf("|---|---:|---:|---:|\n");
  std::printf("| present key | %.2f | %.0f ns | %.0fx fewer pages |\n",
              reads_present, ns_present, kU1ScanPages / reads_present);
  std::printf("| absent key  | %.2f | %.0f ns | %.0fx fewer pages |\n",
              reads_absent, ns_absent, kU1ScanPages / reads_absent);
  std::printf("| Unit 1 scan | %.0f (worst case) | -- | 1x |\n", kU1ScanPages);

  std::printf(
      "\npages read per lookup equals the tree height (%d): one read per "
      "level, root to leaf.\n",
      tree.height());

  ::unlink(path);
  return 0;
}
