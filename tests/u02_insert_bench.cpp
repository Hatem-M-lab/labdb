// Insert cost and the fill-factor tax. Building a B+Tree by inserting one
// key at a time costs a descent plus an occasional split. This benchmark
// measures build throughput and -- more interestingly -- how full the
// leaves end up, for ascending versus random insertion order. The two
// numbers are very different, and the difference is a real, classic
// property of one-at-a-time B+Tree construction. Seeds fixed.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <algorithm>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "buffer_pool.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

struct Result {
  double build_s;
  std::size_t leaves;
  std::size_t internal;
  std::size_t keys;
  int height;
};

Result build(const std::string& path, const std::vector<Key>& order) {
  ::unlink(path.c_str());
  Pager pager(path);
  BufferPool pool(pager, 4096);
  BTree tree(pool);
  const double t0 = now_s();
  for (Key k : order) tree.insert(k, k);
  const double build_s = now_s() - t0;

  Page p;
  Result r{build_s, 0, 0, order.size(), tree.height()};
  for (PageId id = 1; id < pool.page_count(); ++id) {
    pool.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) ++r.leaves;
    else if (p.type() == PageType::kBTreeInternal) ++r.internal;
  }
  ::unlink(path.c_str());
  return r;
}

void report(const char* label, const Result& r) {
  const double fill =
      100.0 * double(r.keys) / (double(r.leaves) * kLeafMaxEntries);
  const double rate = double(r.keys) / r.build_s / 1e6;
  std::printf("| %s | %.2f | %.2f | %zu | %d | %.1f%% |\n", label, r.build_s,
              rate, r.leaves, r.height, fill);
}

}  // namespace

int main() {
  constexpr Key kN = 1000000;

  std::vector<Key> ascending(kN);
  for (Key i = 0; i < kN; ++i) ascending[i] = i;

  std::vector<Key> shuffled = ascending;
  std::mt19937_64 rng(20260705);
  std::shuffle(shuffled.begin(), shuffled.end(), rng);

  const Result seq = build("u02_ins_seq.db", ascending);
  const Result rnd = build("u02_ins_rnd.db", shuffled);

  std::printf(
      "inserting %llu keys one at a time (leaf capacity = %u entries):\n\n",
      (unsigned long long)kN, kLeafMaxEntries);
  std::printf("| order | build (s) | M keys/s | leaves | height | leaf fill |\n");
  std::printf("|---|---:|---:|---:|---:|---:|\n");
  report("ascending", seq);
  report("random", rnd);

  const double seq_fill =
      100.0 * double(seq.keys) / (double(seq.leaves) * kLeafMaxEntries);
  const double rnd_fill =
      100.0 * double(rnd.keys) / (double(rnd.leaves) * kLeafMaxEntries);
  std::printf(
      "\nascending fill %.0f%%, random fill %.0f%%: the same keys, %.0f%% "
      "more leaves when inserted in order.\n",
      seq_fill, rnd_fill,
      100.0 * (double(seq.leaves) - double(rnd.leaves)) / double(rnd.leaves));

  return 0;
}
