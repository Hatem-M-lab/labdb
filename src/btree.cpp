#include "btree.hpp"

namespace labdb {

BTree::BTree(BufferPool& pool) : pool_(pool) {
  root_ = pool_.btree_root();
  if (root_ == kNullPage) {
    // Plant a fresh tree: a single empty leaf that is also the root.
    root_ = pool_.allocate_page();
    Page p;
    LeafNode leaf(p);
    leaf.init(root_);
    pool_.write_page(root_, p);
    pool_.set_btree_root(root_);
  }
}

std::optional<Value> BTree::search(Key key) const {
  PageId node_id = root_;
  Page p;
  for (;;) {
    pool_.read_page(node_id, p);
    if (p.type() == PageType::kBTreeLeaf) {
      LeafNode leaf(p);
      const std::uint16_t i = leaf.lower_bound(key);
      if (i < leaf.count() && leaf.key_at(i) == key)
        return leaf.value_at(i);
      return std::nullopt;  // fell off the sorted run: key absent
    }
    InternalNode node(p);
    node_id = node.child_at(node.child_index_for(key));
  }
}

int BTree::height() const {
  int levels = 1;
  PageId node_id = root_;
  Page p;
  for (;;) {
    pool_.read_page(node_id, p);
    if (p.type() == PageType::kBTreeLeaf) return levels;
    InternalNode node(p);
    node_id = node.child_at(0);  // descend the leftmost spine
    ++levels;
  }
}

std::optional<BTree::Split> BTree::insert_rec(PageId node_id, Key key,
                                              Value value, bool& inserted) {
  Page p;
  pool_.read_page(node_id, p);

  // ---------------------------------------------------------------- leaf
  if (p.type() == PageType::kBTreeLeaf) {
    LeafNode leaf(p);
    const std::uint16_t i = leaf.lower_bound(key);
    if (i < leaf.count() && leaf.key_at(i) == key) {
      leaf.set_value_at(i, value);          // key exists: update in place
      pool_.write_page(node_id, p);
      inserted = false;
      return std::nullopt;
    }
    if (!leaf.is_full()) {
      leaf.insert_at(i, key, value);
      pool_.write_page(node_id, p);
      inserted = true;
      return std::nullopt;
    }

    // Full leaf: split, then insert into the correct half. Splitting
    // before inserting means no page ever has to hold one entry too many.
    Page rp;
    LeafNode right(rp);
    const PageId right_id = pool_.allocate_page();
    right.init(right_id);

    const std::uint16_t mid = static_cast<std::uint16_t>(leaf.count() / 2);
    leaf.move_suffix_to(right, mid);      // upper half -> right leaf
    right.set_next(leaf.next());          // splice into the sibling chain
    leaf.set_next(right_id);

    if (key < right.key_at(0))
      leaf.insert_at(leaf.lower_bound(key), key, value);
    else
      right.insert_at(right.lower_bound(key), key, value);

    // COPY-UP: the separator is the smallest key of the right leaf, and it
    // STAYS in the right leaf. (Moving it up instead -- the B-Tree reflex
    // -- is this unit's forensic trap.)
    const Key sep = right.key_at(0);
    pool_.write_page(node_id, p);
    pool_.write_page(right_id, rp);
    inserted = true;
    return Split{sep, right_id};
  }

  // ------------------------------------------------------------ internal
  InternalNode node(p);
  const std::uint16_t ci = node.child_index_for(key);
  const PageId child_id = node.child_at(ci);
  auto child_split = insert_rec(child_id, key, value, inserted);
  if (!child_split) return std::nullopt;  // child absorbed it

  if (!node.is_full()) {
    node.insert_child_at(ci, child_split->sep_key, child_split->right);
    pool_.write_page(node_id, p);
    return std::nullopt;
  }

  // Full internal node: split around the median, which MOVES up (internal
  // keys are pure routing, so the median need not stay below). Then insert
  // the incoming separator into whichever half now owns its range.
  Page rp;
  InternalNode right(rp);
  const PageId right_id = pool_.allocate_page();
  right.init(right_id);

  const std::uint16_t n = node.count();
  const std::uint16_t mid = static_cast<std::uint16_t>(n / 2);
  const Key median = node.key_at(mid);

  // right gets keys (mid+1 .. n-1) and children (mid+1 .. n).
  std::uint16_t r = 0;
  for (std::uint16_t k = static_cast<std::uint16_t>(mid + 1); k < n; ++k, ++r) {
    right.set_key_at(r, node.key_at(k));
    right.set_child_at(r, node.child_at(k));
  }
  right.set_child_at(r, node.child_at(n));  // last (rightmost) child
  right.set_count(static_cast<std::uint16_t>(n - mid - 1));
  node.set_count(mid);                       // left keeps keys [0, mid)

  // Route the incoming (sep_key, right_child) into the correct half.
  if (child_split->sep_key < median) {
    const std::uint16_t pos = node.child_index_for(child_split->sep_key);
    node.insert_child_at(pos, child_split->sep_key, child_split->right);
  } else {
    const std::uint16_t pos = right.child_index_for(child_split->sep_key);
    right.insert_child_at(pos, child_split->sep_key, child_split->right);
  }

  pool_.write_page(node_id, p);
  pool_.write_page(right_id, rp);
  return Split{median, right_id};  // median moves up to our parent
}

bool BTree::insert(Key key, Value value) {
  bool inserted = false;
  auto split = insert_rec(root_, key, value, inserted);
  if (split) {
    // The root split: grow a new internal root one level up. This is the
    // only place the tree gains height.
    Page rp;
    InternalNode new_root(rp);
    const PageId new_root_id = pool_.allocate_page();
    new_root.init(new_root_id);
    new_root.set_key_at(0, split->sep_key);
    new_root.set_child_at(0, root_);
    new_root.set_child_at(1, split->right);
    new_root.set_count(1);
    pool_.write_page(new_root_id, rp);
    root_ = new_root_id;
    pool_.set_btree_root(root_);
  }
  return inserted;
}

// ============================ deletion (Unit 3) ============================

bool BTree::erase(Key key) {
  bool underflow = false;
  const bool found = erase_rec(root_, key, underflow);
  if (!found) return false;

  // Root collapse: if the root is internal and deletion emptied it down to a
  // single child, that child becomes the new root. This is the only place
  // the tree loses height -- the inverse of insert's root growth.
  Page p;
  pool_.read_page(root_, p);
  if (p.type() == PageType::kBTreeInternal) {
    InternalNode root(p);
    if (root.count() == 0) {
      const PageId only = root.child_at(0);
      pool_.free_page(root_);
      root_ = only;
      pool_.set_btree_root(root_);
    }
  }
  return true;
}

bool BTree::erase_rec(PageId node_id, Key key, bool& underflow) {
  Page p;
  pool_.read_page(node_id, p);

  if (p.type() == PageType::kBTreeLeaf) {
    LeafNode leaf(p);
    const std::uint16_t i = leaf.lower_bound(key);
    if (i >= leaf.count() || leaf.key_at(i) != key) {
      underflow = false;
      return false;  // key absent
    }
    leaf.erase_at(i);
    pool_.write_page(node_id, p);
    underflow = leaf.is_underflow();
    return true;
  }

  InternalNode node(p);
  const std::uint16_t ci = node.child_index_for(key);
  bool child_underflow = false;
  const bool found = erase_rec(node.child_at(ci), key, child_underflow);
  if (!found) {
    underflow = node.is_underflow();
    return false;
  }
  if (child_underflow) fix_child_underflow(p, ci);  // may shrink this node
  pool_.write_page(node_id, p);
  underflow = InternalNode(p).is_underflow();
  return true;
}

// Repair an under-full child (index ci) of the parent held in parent_page.
// Prefer borrowing one entry from a sibling that can spare it; otherwise
// merge with a sibling. Mutates parent_page in memory (the caller writes it)
// and reads/writes the child and sibling pages directly.
void BTree::fix_child_underflow(Page& parent_page, std::uint16_t ci) {
  InternalNode parent(parent_page);
  const PageId child_id = parent.child_at(ci);
  Page cp;
  pool_.read_page(child_id, cp);
  const bool leaf = (cp.type() == PageType::kBTreeLeaf);

  // ---- borrow from the left sibling ----
  if (ci > 0) {
    const PageId left_id = parent.child_at(static_cast<std::uint16_t>(ci - 1));
    Page lp;
    pool_.read_page(left_id, lp);
    const bool spare = leaf ? (LeafNode(lp).count() > kLeafMinEntries)
                            : (InternalNode(lp).count() > kInternalMinKeys);
    if (spare) {
      if (leaf) {
        LeafNode L(lp), C(cp);
        const std::uint16_t li = static_cast<std::uint16_t>(L.count() - 1);
        C.insert_at(0, L.key_at(li), L.value_at(li));  // move L's last to C's front
        L.erase_at(li);
        parent.set_key_at(static_cast<std::uint16_t>(ci - 1), C.key_at(0));
      } else {
        InternalNode L(lp), C(cp);
        const std::uint16_t lc = L.count();
        const Key down = parent.key_at(static_cast<std::uint16_t>(ci - 1));
        const PageId moved = L.child_at(lc);                 // L's last child
        const Key up = L.key_at(static_cast<std::uint16_t>(lc - 1));
        C.prepend_child_key(down, moved);                    // rotate down->C
        L.set_count(static_cast<std::uint16_t>(lc - 1));     // drop L's last key+child
        parent.set_key_at(static_cast<std::uint16_t>(ci - 1), up);  // rotate up
      }
      pool_.write_page(left_id, lp);
      pool_.write_page(child_id, cp);
      return;
    }
  }

  // ---- borrow from the right sibling ----
  if (ci < parent.count()) {
    const PageId right_id = parent.child_at(static_cast<std::uint16_t>(ci + 1));
    Page rp;
    pool_.read_page(right_id, rp);
    const bool spare = leaf ? (LeafNode(rp).count() > kLeafMinEntries)
                            : (InternalNode(rp).count() > kInternalMinKeys);
    if (spare) {
      if (leaf) {
        LeafNode C(cp), R(rp);
        C.insert_at(C.count(), R.key_at(0), R.value_at(0));  // move R's first to C's end
        R.erase_at(0);
        parent.set_key_at(ci, R.key_at(0));
      } else {
        InternalNode C(cp), R(rp);
        const Key down = parent.key_at(ci);
        const PageId moved = R.child_at(0);                  // R's first child
        const Key up = R.key_at(0);
        C.append_child_key(down, moved);                     // rotate down->C
        R.erase_front();
        parent.set_key_at(ci, up);                           // rotate up
      }
      pool_.write_page(child_id, cp);
      pool_.write_page(right_id, rp);
      return;
    }
  }

  // ---- neither sibling can spare: merge ----
  if (ci > 0) {
    // Merge child ci into its left sibling; drop separator ci-1 and child ci.
    const std::uint16_t si = static_cast<std::uint16_t>(ci - 1);
    const PageId left_id = parent.child_at(si);
    Page lp;
    pool_.read_page(left_id, lp);
    if (leaf) {
      LeafNode L(lp), C(cp);
      L.append_from(C);
      L.set_next(C.next());  // splice the sibling chain (Unit 3's forensic trap)
    } else {
      InternalNode L(lp), C(cp);
      const std::uint16_t lc = L.count(), cc = C.count();
      L.set_key_at(lc, parent.key_at(si));  // pull separator down
      for (std::uint16_t i = 0; i < cc; ++i)
        L.set_key_at(static_cast<std::uint16_t>(lc + 1 + i), C.key_at(i));
      for (std::uint16_t i = 0; i <= cc; ++i)
        L.set_child_at(static_cast<std::uint16_t>(lc + 1 + i), C.child_at(i));
      L.set_count(static_cast<std::uint16_t>(lc + 1 + cc));
    }
    pool_.write_page(left_id, lp);
    pool_.free_page(child_id);
    parent.erase_key_child_at(si);
  } else {
    // Merge the right sibling into child ci; drop separator ci and child ci+1.
    const PageId right_id = parent.child_at(static_cast<std::uint16_t>(ci + 1));
    Page rp;
    pool_.read_page(right_id, rp);
    if (leaf) {
      LeafNode C(cp), R(rp);
      C.append_from(R);
      C.set_next(R.next());  // splice the sibling chain (Unit 3's forensic trap)
    } else {
      InternalNode C(cp), R(rp);
      const std::uint16_t cc = C.count(), rc = R.count();
      C.set_key_at(cc, parent.key_at(ci));  // pull separator down
      for (std::uint16_t i = 0; i < rc; ++i)
        C.set_key_at(static_cast<std::uint16_t>(cc + 1 + i), R.key_at(i));
      for (std::uint16_t i = 0; i <= rc; ++i)
        C.set_child_at(static_cast<std::uint16_t>(cc + 1 + i), R.child_at(i));
      C.set_count(static_cast<std::uint16_t>(cc + 1 + rc));
    }
    pool_.write_page(child_id, cp);
    pool_.free_page(right_id);
    parent.erase_key_child_at(ci);
  }
}

// ========================== range scans (Unit 3) ==========================

void Cursor::settle() {
  for (;;) {
    if (leaf_id_ == kNullPage) {
      valid_ = false;
      return;
    }
    LeafNode n(leaf_);
    if (idx_ < n.count()) {
      key_ = n.key_at(idx_);
      value_ = n.value_at(idx_);
      valid_ = true;
      return;
    }
    leaf_id_ = n.next();  // exhausted this leaf: walk the sibling chain
    if (leaf_id_ == kNullPage) {
      valid_ = false;
      return;
    }
    pool_->read_page(leaf_id_, leaf_);
    idx_ = 0;
  }
}

void Cursor::next() {
  ++idx_;
  settle();
}

Cursor BTree::seek(Key lo) const {
  Cursor c(pool_);
  PageId id = root_;
  Page p;
  for (;;) {  // descend to the leaf that would hold lo
    pool_.read_page(id, p);
    if (p.type() == PageType::kBTreeLeaf) break;
    InternalNode node(p);
    id = node.child_at(node.child_index_for(lo));
  }
  c.leaf_ = p;
  c.leaf_id_ = id;
  c.idx_ = LeafNode(p).lower_bound(lo);
  c.settle();
  return c;
}

}  // namespace labdb
