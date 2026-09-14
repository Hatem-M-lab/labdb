// THE BUG, preserved for study: splitting a B+Tree leaf as if it were a
// B-Tree. In a B-Tree the median of a split moves UP into the parent and
// leaves the node. In a B+Tree every key must remain in a leaf; the
// parent gets only a COPY. Get this wrong and the median key silently
// disappears from the data level -- present as a separator, absent from
// every leaf, unfindable by lookup.
//
// This program builds the identical little tree two ways -- once with the
// buggy move-up split, once with the correct copy-up split -- and then
// searches for the median key in each. No pager, no disk: three Page
// objects in memory so the evidence is easy to dump.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Itests
//        tools/u02_median_bug_demo.cpp -o bin/u02_median_bug_demo
#include <cstdio>
#include <optional>

#include "btree.hpp"
#include "page.hpp"

using namespace labdb;

namespace {

constexpr std::uint16_t kFill = 254;   // a full leaf: keys 0..253
constexpr std::uint16_t kMid = kFill / 2;  // 127 -- the boundary key

// Search this hand-built 2-level tree (root internal + two leaves) using
// exactly the routing rule the engine uses.
std::optional<Value> search(Page& root, Page& left, Page& right, Key k) {
  InternalNode r(root);
  const std::uint16_t ci = r.child_index_for(k);
  Page& leaf = (r.child_at(ci) == 1) ? left : right;
  LeafNode n(leaf);
  const std::uint16_t i = n.lower_bound(k);
  if (i < n.count() && n.key_at(i) == k) return n.value_at(i);
  return std::nullopt;
}

void dump(const char* label, Page& root, Page& left, Page& right) {
  InternalNode r(root);
  LeafNode L(left), R(right);
  std::printf("  %s\n", label);
  std::printf("    internal separators: ");
  for (std::uint16_t i = 0; i < r.count(); ++i)
    std::printf("%llu ", (unsigned long long)r.key_at(i));
  std::printf("\n    left  leaf keys: %llu..%llu (%u keys)\n",
              (unsigned long long)L.key_at(0),
              (unsigned long long)L.key_at(static_cast<std::uint16_t>(L.count() - 1)),
              L.count());
  std::printf("    right leaf keys: %llu..%llu (%u keys)\n",
              (unsigned long long)R.key_at(0),
              (unsigned long long)R.key_at(static_cast<std::uint16_t>(R.count() - 1)),
              R.count());
}

}  // namespace

int main() {
  std::printf("A full leaf holds keys 0..%u. We insert one more, forcing a "
              "split. The boundary key is %u.\n\n",
              kFill - 1, kMid);

  // ================= THE BUGGY WAY: move the median up =================
  Page b_root, b_left, b_right;
  {
    InternalNode root(b_root);
    LeafNode left(b_left), right(b_right);
    root.init(0);
    left.init(1);
    right.init(2);
    // left gets [0, kMid); the median key kMid MOVES UP; right gets
    // (kMid, kFill). The median is now in NO leaf.
    for (std::uint16_t i = 0; i < kMid; ++i)
      left.insert_at(i, i, i * 100);
    std::uint16_t r = 0;
    for (std::uint16_t i = static_cast<std::uint16_t>(kMid + 1); i < kFill; ++i)
      right.insert_at(r++, i, i * 100);
    root.set_child_at(0, 1);
    root.set_key_at(0, kMid);   // separator = median, but it left the leaves
    root.set_child_at(1, 2);
    root.set_count(1);
  }
  dump("buggy split (median moved up):", b_root, b_left, b_right);
  auto buggy = search(b_root, b_left, b_right, kMid);
  std::printf("    search(%u) -> %s\n\n", kMid,
              buggy ? "found" : "NOT FOUND  <-- the median vanished");

  // ================= THE CORRECT WAY: copy the median up ===============
  Page g_root, g_left, g_right;
  {
    InternalNode root(g_root);
    LeafNode left(g_left), right(g_right);
    root.init(0);
    left.init(1);
    right.init(2);
    // left gets [0, kMid); right gets [kMid, kFill) -- the median STAYS in
    // the right leaf; the parent gets a copy as the separator.
    for (std::uint16_t i = 0; i < kMid; ++i)
      left.insert_at(i, i, i * 100);
    std::uint16_t r = 0;
    for (std::uint16_t i = kMid; i < kFill; ++i)
      right.insert_at(r++, i, i * 100);
    root.set_child_at(0, 1);
    root.set_key_at(0, kMid);   // separator = copy of right leaf's first key
    root.set_child_at(1, 2);
    root.set_count(1);
  }
  dump("correct split (median copied up, stays in right leaf):", g_root,
       g_left, g_right);
  auto good = search(g_root, g_left, g_right, kMid);
  std::printf("    search(%u) -> %s\n", kMid, good ? "found" : "NOT FOUND");

  return 0;
}
