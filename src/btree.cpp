#include "btree.hpp"

namespace labdb {

BTree::BTree(Pager& pager) : pager_(pager) {
  root_ = pager_.btree_root();
  if (root_ == kNullPage) {
    // Plant a fresh tree: a single empty leaf that is also the root.
    root_ = pager_.allocate_page();
    Page p;
    LeafNode leaf(p);
    leaf.init(root_);
    pager_.write_page(root_, p);
    pager_.set_btree_root(root_);
  }
}

std::optional<Value> BTree::search(Key key) const {
  PageId node_id = root_;
  Page p;
  for (;;) {
    pager_.read_page(node_id, p);
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
    pager_.read_page(node_id, p);
    if (p.type() == PageType::kBTreeLeaf) return levels;
    InternalNode node(p);
    node_id = node.child_at(0);  // descend the leftmost spine
    ++levels;
  }
}

std::optional<BTree::Split> BTree::insert_rec(PageId node_id, Key key,
                                              Value value, bool& inserted) {
  Page p;
  pager_.read_page(node_id, p);

  // ---------------------------------------------------------------- leaf
  if (p.type() == PageType::kBTreeLeaf) {
    LeafNode leaf(p);
    const std::uint16_t i = leaf.lower_bound(key);
    if (i < leaf.count() && leaf.key_at(i) == key) {
      leaf.set_value_at(i, value);          // key exists: update in place
      pager_.write_page(node_id, p);
      inserted = false;
      return std::nullopt;
    }
    if (!leaf.is_full()) {
      leaf.insert_at(i, key, value);
      pager_.write_page(node_id, p);
      inserted = true;
      return std::nullopt;
    }

    // Full leaf: split, then insert into the correct half. Splitting
    // before inserting means no page ever has to hold one entry too many.
    Page rp;
    LeafNode right(rp);
    const PageId right_id = pager_.allocate_page();
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
    pager_.write_page(node_id, p);
    pager_.write_page(right_id, rp);
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
    pager_.write_page(node_id, p);
    return std::nullopt;
  }

  // Full internal node: split around the median, which MOVES up (internal
  // keys are pure routing, so the median need not stay below). Then insert
  // the incoming separator into whichever half now owns its range.
  Page rp;
  InternalNode right(rp);
  const PageId right_id = pager_.allocate_page();
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

  pager_.write_page(node_id, p);
  pager_.write_page(right_id, rp);
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
    const PageId new_root_id = pager_.allocate_page();
    new_root.init(new_root_id);
    new_root.set_key_at(0, split->sep_key);
    new_root.set_child_at(0, root_);
    new_root.set_child_at(1, split->right);
    new_root.set_count(1);
    pager_.write_page(new_root_id, rp);
    root_ = new_root_id;
    pager_.set_btree_root(root_);
  }
  return inserted;
}

}  // namespace labdb
