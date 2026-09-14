#pragma once
// A B+Tree over the pager: fixed 8-byte keys mapped to fixed 8-byte
// values, giving O(log n) point lookup -- the answer to Unit 1's scan
// problem. Each node is one page. Two node shapes share the 20-byte page
// header from Unit 1:
//
//   Leaf     (kBTreeLeaf):     sorted array of (key, value) pairs, plus a
//                              next-sibling link for the range scans of
//                              Unit 3. ALL keys live in leaves.
//   Internal (kBTreeInternal): sorted array of separator keys and one more
//                              child pointer than it has keys. Internal
//                              keys are routing copies, never data.
//
// Scope for this unit: keys are unique (re-inserting a key updates its
// value); keys and values are 8 bytes. Variable-length and duplicate keys
// are deliberately out of scope (see the introduction).

#include <cstdint>
#include <cstring>
#include <optional>

#include "page.hpp"
#include "pager.hpp"

namespace labdb {

using Key = std::uint64_t;
using Value = std::uint64_t;

// ---- Leaf layout ----------------------------------------------------
// [ header 20B ][ entry 0 ][ entry 1 ] ... ; entry = (u64 key, u64 value).
// next_page (header) points at the right sibling leaf (kNullPage if none).
inline constexpr std::size_t kLeafEntrySize = 16;  // 8 key + 8 value
inline constexpr std::uint16_t kLeafMaxEntries =
    static_cast<std::uint16_t>((kPageSize - kPageHeaderSize) / kLeafEntrySize);

// ---- Internal layout ------------------------------------------------
// Keys occupy a fixed-capacity region right after the header; the child
// array starts at a FIXED offset past that region, so inserting a key
// shifts keys and children independently. slot_count = number of keys;
// children = keys + 1.
inline constexpr std::uint16_t kInternalMaxKeys = 339;  // see the derivation in 2.2
inline constexpr std::size_t kInternalKeyBase = kPageHeaderSize;
inline constexpr std::size_t kInternalChildBase =
    kInternalKeyBase + std::size_t{kInternalMaxKeys} * 8;  // 2732
static_assert(kInternalChildBase +
                      (std::size_t{kInternalMaxKeys} + 1) * 4 <= kPageSize,
              "internal node must fit its keys and children in one page");

// ------------------------------------------------------------------ Leaf
class LeafNode {
 public:
  explicit LeafNode(Page& p) : p_(&p) {}

  void init(PageId id) {
    p_->set_id(id);
    p_->set_type(PageType::kBTreeLeaf);
    p_->set_slot_count(0);
    p_->set_next_page(kNullPage);
  }

  std::uint16_t count() const { return p_->slot_count(); }
  bool is_full() const { return count() >= kLeafMaxEntries; }
  PageId next() const { return p_->next_page(); }
  void set_next(PageId id) { p_->set_next_page(id); }

  Key key_at(std::uint16_t i) const { return load_u64(base(i)); }
  Value value_at(std::uint16_t i) const { return load_u64(base(i) + 8); }
  void set_value_at(std::uint16_t i, Value v) { store_u64(base(i) + 8, v); }

  // First index whose key is >= k (the insertion point for k).
  std::uint16_t lower_bound(Key k) const {
    std::uint16_t lo = 0, hi = count();
    while (lo < hi) {
      const std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
      if (key_at(mid) < k)
        lo = static_cast<std::uint16_t>(mid + 1);
      else
        hi = mid;
    }
    return lo;
  }

  // Insert (k,v) at index i, shifting later entries right. Caller ensures
  // the leaf is not full and i is the sorted position.
  void insert_at(std::uint16_t i, Key k, Value v) {
    const std::uint16_t n = count();
    std::memmove(base(static_cast<std::uint16_t>(i + 1)), base(i),
                 static_cast<std::size_t>(n - i) * kLeafEntrySize);
    store_u64(base(i), k);
    store_u64(base(i) + 8, v);
    p_->set_slot_count(static_cast<std::uint16_t>(n + 1));
  }

  // Move entries [from, count) into the front of R; shrink this leaf to
  // [0, from). Used by a split.
  void move_suffix_to(LeafNode& r, std::uint16_t from) {
    const std::uint16_t n = count();
    const std::uint16_t moved = static_cast<std::uint16_t>(n - from);
    std::memcpy(r.base(0), base(from), std::size_t{moved} * kLeafEntrySize);
    r.p_->set_slot_count(moved);
    p_->set_slot_count(from);
  }

  std::uint8_t* base(std::uint16_t i) {
    return p_->data() + kPageHeaderSize + std::size_t{i} * kLeafEntrySize;
  }
  const std::uint8_t* base(std::uint16_t i) const {
    return p_->data() + kPageHeaderSize + std::size_t{i} * kLeafEntrySize;
  }

 private:
  Page* p_;
};

// -------------------------------------------------------------- Internal
class InternalNode {
 public:
  explicit InternalNode(Page& p) : p_(&p) {}

  void init(PageId id) {
    p_->set_id(id);
    p_->set_type(PageType::kBTreeInternal);
    p_->set_slot_count(0);
    p_->set_next_page(kNullPage);
  }

  std::uint16_t count() const { return p_->slot_count(); }  // number of keys
  bool is_full() const { return count() >= kInternalMaxKeys; }

  Key key_at(std::uint16_t i) const {
    return load_u64(p_->data() + kInternalKeyBase + std::size_t{i} * 8);
  }
  void set_key_at(std::uint16_t i, Key k) {
    store_u64(p_->data() + kInternalKeyBase + std::size_t{i} * 8, k);
  }
  PageId child_at(std::uint16_t i) const {
    return load_u32(p_->data() + kInternalChildBase + std::size_t{i} * 4);
  }
  void set_child_at(std::uint16_t i, PageId c) {
    store_u32(p_->data() + kInternalChildBase + std::size_t{i} * 4, c);
  }

  // Index of the child to descend into for key k. With copy-up separators,
  // key_i is the smallest key in child_{i+1}, so we descend past every
  // separator that is <= k: that is upper_bound (first key strictly > k).
  std::uint16_t child_index_for(Key k) const {
    std::uint16_t lo = 0, hi = count();
    while (lo < hi) {
      const std::uint16_t mid = static_cast<std::uint16_t>((lo + hi) / 2);
      if (key_at(mid) <= k)
        lo = static_cast<std::uint16_t>(mid + 1);
      else
        hi = mid;
    }
    return lo;
  }

  // Insert separator key k at key-index ki and its right child c at
  // child-index ki+1, shifting to make room. Caller ensures not full.
  void insert_child_at(std::uint16_t ki, Key k, PageId c) {
    const std::uint16_t n = count();
    // shift keys [ki, n) right by one
    for (std::uint16_t i = n; i > ki; --i)
      set_key_at(i, key_at(static_cast<std::uint16_t>(i - 1)));
    // shift children [ki+1, n+1) right by one
    for (std::uint16_t i = static_cast<std::uint16_t>(n + 1);
         i > ki + 1; --i)
      set_child_at(i, child_at(static_cast<std::uint16_t>(i - 1)));
    set_key_at(ki, k);
    set_child_at(static_cast<std::uint16_t>(ki + 1), c);
    p_->set_slot_count(static_cast<std::uint16_t>(n + 1));
  }

  void set_count(std::uint16_t n) { p_->set_slot_count(n); }

 private:
  Page* p_;
};

// ------------------------------------------------------------------ Tree
class BTree {
 public:
  // Opens the tree recorded in the pager's meta page, planting a fresh
  // (empty leaf) root if there is none.
  explicit BTree(Pager& pager);

  // Insert or update. Returns true if the key was newly inserted, false
  // if an existing key's value was overwritten.
  bool insert(Key key, Value value);

  // Point lookup: the operation Unit 1 could only answer by scanning.
  std::optional<Value> search(Key key) const;

  PageId root() const { return root_; }

  // Height in levels: 1 for a lone leaf root, +1 per internal level. Equal
  // to the number of pages a root-to-leaf lookup must read.
  int height() const;

 private:
  struct Split {
    Key sep_key;     // separator to install in the parent
    PageId right;    // new right sibling created by the split
  };

  std::optional<Split> insert_rec(PageId node_id, Key key, Value value,
                                   bool& inserted);

  Pager& pager_;
  PageId root_ = kNullPage;
};

}  // namespace labdb
