// Unit 2 correctness: the B+Tree must agree with std::map on every key,
// keep its nodes sorted and correctly linked, route every lookup to the
// right leaf across splits, and survive a reopen. Seeds are fixed.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "btree.hpp"
#include "harness.hpp"
#include "page.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

// Walk every page of a tree-only database and check node invariants:
// each node's keys are strictly ascending, no node overflows its
// capacity, and the leaf sibling chain visits keys in globally sorted
// order with the right total count.
void check_structure(Pager& pager, PageId root, std::size_t expected_keys) {
  Page p;
  std::size_t internal = 0, leaf = 0;
  for (PageId id = 1; id < pager.page_count(); ++id) {
    pager.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) {
      ++leaf;
      LeafNode n(p);
      REQUIRE(n.count() <= kLeafMaxEntries);
      for (std::uint16_t i = 1; i < n.count(); ++i)
        REQUIRE(n.key_at(static_cast<std::uint16_t>(i - 1)) < n.key_at(i));
    } else if (p.type() == PageType::kBTreeInternal) {
      ++internal;
      InternalNode n(p);
      REQUIRE(n.count() <= kInternalMaxKeys);
      for (std::uint16_t i = 1; i < n.count(); ++i)
        REQUIRE(n.key_at(static_cast<std::uint16_t>(i - 1)) < n.key_at(i));
    }
  }
  REQUIRE(leaf >= 1);

  // Find the leftmost leaf, then walk next() pointers: keys must be sorted
  // across the whole chain and total to the expected count.
  PageId id = root;
  for (;;) {
    pager.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) break;
    InternalNode n(p);
    id = n.child_at(0);
  }
  std::size_t total = 0;
  Key prev = 0;
  bool first = true;
  std::size_t leaves_walked = 0;
  while (id != kNullPage) {
    pager.read_page(id, p);
    LeafNode n(p);
    ++leaves_walked;
    for (std::uint16_t i = 0; i < n.count(); ++i) {
      const Key k = n.key_at(i);
      if (!first) REQUIRE(prev < k);  // strictly ascending across leaves
      prev = k;
      first = false;
      ++total;
    }
    id = n.next();
  }
  REQUIRE(total == expected_keys);
  REQUIRE(leaves_walked == leaf);  // chain reaches every leaf, no orphans
  std::printf(
      "  structure ok: %zu leaves + %zu internal, sibling chain covers all "
      "%zu keys\n",
      leaf, internal, total);
}

}  // namespace

int main() {
  const char* path = "u02_btree_test.db";
  ::unlink(path);

  std::map<Key, Value> ref;
  std::mt19937_64 rng(20260705);
  PageId root = kNullPage;

  // ---- Phase 1: sequential ascending inserts (worst case for fill) ----
  {
    Pager pager(path);
    BTree tree(pager);
    for (Key k = 1; k <= 50000; ++k) {
      REQUIRE(tree.insert(k, k * 10));
      ref[k] = k * 10;
    }
    for (const auto& [k, v] : ref) {
      auto got = tree.search(k);
      REQUIRE(got && *got == v);
    }
    std::printf("PASS sequential: 50000 ascending keys, height=%d\n",
                tree.height());
    root = tree.root();
    check_structure(pager, root, ref.size());
    pager.sync();
  }

  // ---- Phase 2: reopen, then 500k randomized mixed operations ----
  {
    Pager pager(path);
    BTree tree(pager);
    // Persistence: everything from phase 1 is still here.
    for (const auto& [k, v] : ref) {
      auto got = tree.search(k);
      REQUIRE(got && *got == v);
    }
    std::printf("PASS reopen: all 50000 keys survived, root=%u\n", tree.root());

    std::uniform_int_distribution<Key> key_dist(1, 400000);
    int inserts = 0, updates = 0;
    for (int op = 0; op < 500000; ++op) {
      const Key k = key_dist(rng);
      const Value v = rng();
      const bool newly = tree.insert(k, v);
      auto it = ref.find(k);
      if (it == ref.end()) {
        REQUIRE(newly);
        ref[k] = v;
        ++inserts;
      } else {
        REQUIRE(!newly);  // duplicate must report "updated", not "inserted"
        it->second = v;
        ++updates;
      }
    }
    std::printf("PASS randomized: %d new inserts, %d updates\n", inserts,
                updates);

    // Full agreement with the reference model.
    for (const auto& [k, v] : ref) {
      auto got = tree.search(k);
      REQUIRE(got && *got == v);
    }
    // A pile of keys known to be absent must all miss.
    int misses = 0;
    for (Key k = 400001; k <= 401000; ++k)
      if (!tree.search(k)) ++misses;
    REQUIRE(misses == 1000);
    std::printf("PASS agreement: %zu keys match std::map, 1000 absent miss\n",
                ref.size());

    root = tree.root();
    check_structure(pager, root, ref.size());
    std::printf("final tree: height=%d, %u pages\n", tree.height(),
                pager.page_count());
  }

  ::unlink(path);
  std::printf("u02_btree_test: PASS\n");
  return 0;
}
