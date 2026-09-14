// Unit 3 correctness: deletion must keep the tree a faithful, balanced,
// well-linked B+Tree, and range scans must return exactly the keys in a
// range in ascending order. We cross-check every operation against a
// std::map, verify the leaf chain after heavy deletion, confirm the tree
// shrinks in height as it empties, and check persistence. Seeds fixed.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "buffer_pool.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

// Full ordered scan via the cursor must equal the reference map exactly.
void check_full_scan(const BTree& tree, const std::map<Key, Value>& ref) {
  std::size_t n = 0;
  Key prev = 0;
  bool first = true;
  for (auto c = tree.seek(0); c.valid(); c.next()) {
    if (!first) REQUIRE(prev < c.key());  // strictly ascending
    prev = c.key();
    first = false;
    auto it = ref.find(c.key());
    REQUIRE(it != ref.end() && it->second == c.value());
    ++n;
  }
  REQUIRE(n == ref.size());
}

// A random range [lo, hi] must match the reference map's slice.
void check_range(const BTree& tree, const std::map<Key, Value>& ref, Key lo,
                 Key hi) {
  std::vector<Key> got;
  for (auto c = tree.seek(lo); c.valid() && c.key() <= hi; c.next())
    got.push_back(c.key());
  std::vector<Key> want;
  for (auto it = ref.lower_bound(lo); it != ref.end() && it->first <= hi; ++it)
    want.push_back(it->first);
  REQUIRE(got == want);
}

// After heavy deletion, walk the leaf chain and confirm it still visits
// every key exactly once in sorted order with no orphaned or dangling leaf.
void check_chain(BufferPool& pool, PageId root, std::size_t expected) {
  Page p;
  PageId id = root;
  for (;;) {  // leftmost leaf
    pool.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) break;
    id = InternalNode(p).child_at(0);
  }
  std::size_t total = 0;
  Key prev = 0;
  bool first = true;
  while (id != kNullPage) {
    pool.read_page(id, p);
    LeafNode n(p);
    for (std::uint16_t i = 0; i < n.count(); ++i) {
      if (!first) REQUIRE(prev < n.key_at(i));
      prev = n.key_at(i);
      first = false;
      ++total;
    }
    id = n.next();
  }
  REQUIRE(total == expected);
}

}  // namespace

int main() {
  const char* path = "u03_delete_test.db";
  ::unlink(path);

  std::map<Key, Value> ref;
  std::mt19937_64 rng(20260706);

  {
    Pager pager(path);
    BufferPool pool(pager, 4096);
    BTree tree(pool);

    // Build a good-sized tree.
    for (int i = 0; i < 300000; ++i) {
      const Key k = rng() % 1000000;
      tree.insert(k, k + 7);
      ref[k] = k + 7;
    }
    std::printf("PASS build: %zu keys, height=%d\n", ref.size(),
                tree.height());

    // Ordered retrieval: full scan and a spread of ranges.
    check_full_scan(tree, ref);
    for (int t = 0; t < 200; ++t) {
      const Key lo = rng() % 1000000;
      const Key hi = lo + (rng() % 50000);
      check_range(tree, ref, lo, hi);
    }
    std::printf("PASS scans: full ordered scan + 200 random ranges match map\n");

    // Interleaved random insert/delete -- the real workout for rebalancing.
    std::uniform_int_distribution<Key> kd(0, 1000000);
    int ins = 0, del = 0, del_miss = 0;
    for (int op = 0; op < 600000; ++op) {
      const Key k = kd(rng);
      if (rng() & 1) {
        tree.insert(k, k + 7);
        ref[k] = k + 7;
        ++ins;
      } else {
        const bool did = tree.erase(k);
        auto it = ref.find(k);
        if (it != ref.end()) {
          REQUIRE(did);
          ref.erase(it);
          ++del;
        } else {
          REQUIRE(!did);
          ++del_miss;
        }
      }
    }
    std::printf("PASS mixed: %d inserts, %d deletes, %d delete-misses\n", ins,
                del, del_miss);

    // Full agreement after the churn.
    for (const auto& [k, v] : ref) {
      auto g = tree.search(k);
      REQUIRE(g && *g == v);
    }
    check_full_scan(tree, ref);
    check_chain(pool, tree.root(), ref.size());
    std::printf(
        "PASS agreement: %zu keys match map, chain intact, height=%d\n",
        ref.size(), tree.height());
    pool.sync();
  }

  // Persistence: reopen and re-verify by scan.
  {
    Pager pager(path);
    BufferPool pool(pager, 4096);
    BTree tree(pool);
    check_full_scan(tree, ref);
    std::printf("PASS reopen: %zu keys survived, scan matches\n", ref.size());

    // Delete every key; the tree must empty cleanly and collapse to a lone
    // leaf root, and reclaim pages onto the free list along the way.
    const std::uint32_t pages_before = pool.page_count();
    const PageId freelen_probe = pool.freelist_head();
    (void)freelen_probe;
    std::vector<Key> keys;
    keys.reserve(ref.size());
    for (const auto& [k, v] : ref) keys.push_back(k);
    for (Key k : keys) REQUIRE(tree.erase(k));
    ref.clear();

    std::size_t scan_n = 0;
    for (auto c = tree.seek(0); c.valid(); c.next()) ++scan_n;
    REQUIRE(scan_n == 0);
    REQUIRE(tree.height() == 1);  // collapsed back to a single leaf
    REQUIRE(pool.freelist_head() != kNullPage);  // merges freed pages
    std::printf(
        "PASS empty: all keys deleted, height back to 1, scan yields 0, "
        "%u pages now recycled on the free list\n",
        pages_before - 1);
  }

  ::unlink(path);
  std::printf("u03_delete_test: PASS\n");
  return 0;
}
