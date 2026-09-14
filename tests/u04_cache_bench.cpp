// The buffer pool, measured. A point lookup still reads 3 pages logically
// (Unit 2's number is unchanged), but with a cache the upper levels of the
// tree -- the root and internal nodes, touched on every single query -- stop
// hitting the disk. This benchmark runs a random point-lookup workload over a
// fixed million-key tree through pools of growing size and reports disk reads
// per lookup and the hit rate (both deterministic under the fixed seed), then
// a robust min-of-runs timing comparison of a tiny pool versus a full one.
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

struct PassStats {
  double disk_per, logical_per, secs;
};
PassStats measure(BTree& tree, const std::vector<Key>& probes, BufferPool& pool) {
  pool.reset_stats();
  const double t0 = now_s();
  std::uint64_t found = 0;
  for (Key k : probes)
    if (tree.search(k)) ++found;
  const double secs = now_s() - t0;
  (void)found;
  return {double(pool.disk_reads()) / probes.size(),
          double(pool.read_count()) / probes.size(), secs};
}

}  // namespace

int main() {
  const char* path = "u04_cache_bench.db";
  ::unlink(path);
  constexpr Key kN = 1000000;
  constexpr int kLookups = 200000;

  // Build the canonical tree: one million distinct keys, ascending, exactly
  // the tree Units 2 and 3 measured. Sync it to disk, then close.
  std::uint32_t pages = 0;
  int height = 0;
  {
    Pager pager(path);
    BufferPool pool(pager, 8192);
    BTree tree(pool);
    for (Key k = 0; k < kN; ++k) tree.insert(k, k);
    pool.sync();
    pages = pool.page_count();
    height = tree.height();
    std::printf("tree: %u pages on disk, height %d. Every lookup reads %d pages logically.\n\n",
                pages, height, height);
  }

  // A fixed set of random probe keys. (Whether a probe hits or misses, the
  // search reads the same root-to-leaf path, so this measures traversal I/O.)
  std::vector<Key> probes(kLookups);
  std::mt19937_64 pr(999);
  for (int i = 0; i < kLookups; ++i) probes[i] = pr() % kN;

  std::printf("| pool frames | pool covers | disk reads / lookup | hit rate |\n");
  std::printf("|---:|---:|---:|---:|\n");
  const std::size_t sizes[] = {8, 64, 512, 4096, 16384};
  for (std::size_t frames : sizes) {
    Pager pager(path);
    BufferPool pool(pager, frames);
    BTree tree(pool);
    for (Key k : probes) (void)tree.search(k);  // warm
    const PassStats s = measure(tree, probes, pool);
    const double hit = 100.0 * (1.0 - s.disk_per / s.logical_per);
    const double cover = std::min(100.0, 100.0 * double(frames) / double(pages));
    std::printf("| %zu | %.1f%% of file | %.2f | %.1f%% |\n", frames, cover,
                s.disk_per, hit);
  }

  // Timing, robustly: the same 200,000 lookups with a tiny pool (constant
  // disk traffic) versus a full pool (served from memory). Absolute times on
  // this virtualized box are noisy, so we take the best of several runs.
  auto best_secs = [&](std::size_t frames) {
    double best = 1e30;
    Pager pager(path);
    BufferPool pool(pager, frames);
    BTree tree(pool);
    for (Key k : probes) (void)tree.search(k);  // warm once
    for (int rep = 0; rep < 5; ++rep)
      best = std::min(best, measure(tree, probes, pool).secs);
    return best;
  };
  const double tiny = best_secs(8);
  const double full = best_secs(16384);
  std::printf(
      "\nsame %d lookups, best of 5: %.0f ms with 8 frames vs %.0f ms fully "
      "cached -- %.1fx faster from the cache, with identical logical work.\n",
      kLookups, tiny * 1e3, full * 1e3, tiny / full);
  std::printf(
      "logical reads stay at the tree height; disk reads fall as the pool "
      "holds more of the tree. Even an 8-frame pool keeps the root resident "
      "(it is touched on every lookup), so one of the three reads never hits "
      "the disk; larger pools hold the internal nodes and then the leaves, "
      "driving disk reads to zero once the whole tree fits.\n");

  ::unlink(path);
  return 0;
}
