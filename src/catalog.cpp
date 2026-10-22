#include "catalog.hpp"

#include "btree.hpp"
#include "heap.hpp"

namespace labdb {

Catalog::Catalog(BufferPool& pool) : pool_(pool), cat_page_(pool.catalog_root()) {
  if (cat_page_ == kNullPage) {
    cat_page_ = pool_.allocate_page();
    Page p;
    pool_.read_page(cat_page_, p);
    p.set_id(cat_page_);
    p.set_type(PageType::kCatalog);
    set_slot_count(p, 0);
    pool_.write_page(cat_page_, p);
    pool_.set_catalog_root(cat_page_);
  }
}

std::size_t Catalog::find_slot(const Page& p, const std::string& name) const {
  const std::uint16_t n = slot_count(p);
  for (std::uint16_t i = 0; i < n; ++i) {
    const std::uint8_t* s = slot_at(p, i);
    const std::uint8_t stored_len = s[kCatalogMaxNameLen];
    // EXACT match: length first, then bytes. A query can never match a
    // stored name that merely starts with it, or vice versa.
    if (stored_len == name.size() &&
        std::memcmp(s, name.data(), name.size()) == 0)
      return i;
  }
  return npos;
}

void Catalog::write_desc(Page& p, std::size_t slot, const std::string& name,
                         const Schema& schema, PageId heap_head, PageId btree_root) {
  check_that(name.size() <= kCatalogMaxNameLen, "table name too long");
  check_that(schema.size() <= kCatalogMaxColumns, "too many columns for the catalog");
  std::uint8_t* s = slot_at(p, slot);
  std::memset(s, 0, kCatalogSlotSize);
  std::memcpy(s, name.data(), name.size());
  s[kCatalogMaxNameLen] = static_cast<std::uint8_t>(name.size());
  s[kCatalogMaxNameLen + 1] = static_cast<std::uint8_t>(schema.size());
  store_u32(s + 34, heap_head);
  store_u32(s + 38, btree_root);
  // row count, heap pages and every index root start at zero (memset above)
  for (std::size_t i = 0; i < schema.size(); ++i) {
    const Column& c = schema.columns[i];
    check_that(c.name.size() <= kCatalogColNameLen, "column name too long");
    std::uint8_t* cd = s + kCatalogDescFixed + i * kCatalogColDescSize;
    std::memcpy(cd, c.name.data(), c.name.size());
    cd[kCatalogColNameLen] = static_cast<std::uint8_t>(c.type);
    cd[kCatalogColNameLen + 1] = c.nullable ? 1 : 0;
  }
}

Catalog::RawDesc Catalog::read_desc(const Page& p, std::size_t slot) const {
  const std::uint8_t* s = slot_at(p, slot);
  RawDesc d;
  d.name.assign(reinterpret_cast<const char*>(s), s[kCatalogMaxNameLen]);
  const std::uint8_t ncols = s[kCatalogMaxNameLen + 1];
  d.heap_head = load_u32(s + 34);
  d.btree_root = load_u32(s + 38);
  d.rows = load_u64(s + kCatalogRowCountOff);
  d.heap_pages = load_u32(s + kCatalogHeapPagesOff);
  for (std::uint8_t i = 0; i < ncols; ++i) {
    const std::uint8_t* cd = s + kCatalogDescFixed + i * kCatalogColDescSize;
    Column c;
    c.name.assign(reinterpret_cast<const char*>(cd), strnlen(reinterpret_cast<const char*>(cd), kCatalogColNameLen));
    c.type = static_cast<ColType>(cd[kCatalogColNameLen]);
    c.nullable = cd[kCatalogColNameLen + 1] != 0;
    d.schema.columns.push_back(c);
    d.index_roots[i] = load_u32(cd + kCatalogColNameLen + 2);
  }
  return d;
}

Catalog::RawDesc Catalog::desc_of(const std::string& table) const {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  return read_desc(p, i);
}

void Catalog::create_table(const std::string& name, const Schema& schema) {
  Page p;
  load_catalog_page(p);
  check_that(find_slot(p, name) == npos, "a table with this name already exists");
  const std::uint16_t n = slot_count(p);
  check_that(n < kCatalogMaxTables, "catalog page is full (one page limits table count)");

  // A tree always has a root, even empty, so plant one now; a heap does not
  // need a page until it holds a row, so its head starts null and is filled
  // in lazily by the first insert (see update_heap_head).
  BTree fresh_tree(pool_, kNullPage, nullptr);
  const PageId broot = fresh_tree.root();

  write_desc(p, n, name, schema, /*heap_head=*/kNullPage, broot);
  set_slot_count(p, n + 1);
  save_catalog_page(p);
}

bool Catalog::table_exists(const std::string& name) const {
  Page p;
  load_catalog_page(p);
  return find_slot(p, name) != npos;
}

Schema Catalog::schema_of(const std::string& name) const {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  check_that(i != npos, "no such table");
  return read_desc(p, i).schema;
}

std::optional<Schema> Catalog::find_schema(const std::string& name) const {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  if (i == npos) return std::nullopt;
  return read_desc(p, i).schema;
}

std::optional<TableInfo> Catalog::find_table(const std::string& name) const {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  if (i == npos) return std::nullopt;
  RawDesc d = read_desc(p, i);
  TableInfo info;
  info.rows = d.rows;
  info.heap_pages = d.heap_pages;
  for (std::size_t c = 0; c < d.schema.size(); ++c)
    info.indexed.push_back(c == 0 || d.index_roots[c] != kNullPage);
  info.schema = std::move(d.schema);
  return info;
}

std::vector<std::string> Catalog::table_names() const {
  Page p;
  load_catalog_page(p);
  std::vector<std::string> out;
  const std::uint16_t n = slot_count(p);
  for (std::uint16_t i = 0; i < n; ++i) out.push_back(read_desc(p, i).name);
  return out;
}

void Catalog::update_heap_head(const std::string& name, PageId new_head) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  check_that(i != npos, "no such table (heap head update)");
  std::uint8_t* s = slot_at(p, i);
  store_u32(s + 34, new_head);
  // The head moves only when the heap gains a page (its first, or a new one
  // prepended), so this is also where the table's page count goes up.
  store_u32(s + kCatalogHeapPagesOff, load_u32(s + kCatalogHeapPagesOff) + 1);
  save_catalog_page(p);
}

void Catalog::update_btree_root(const std::string& name, PageId new_root) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  check_that(i != npos, "no such table (btree root update)");
  store_u32(slot_at(p, i) + 38, new_root);
  save_catalog_page(p);
}

void Catalog::update_index_root(const std::string& name, std::size_t column, PageId new_root) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, name);
  check_that(i != npos, "no such table (index root update)");
  store_u32(slot_at(p, i) + kCatalogDescFixed + column * kCatalogColDescSize + kCatalogColNameLen + 2,
            new_root);
  save_catalog_page(p);
}

std::optional<RID> Catalog::insert_row(const std::string& table, const Row& row) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  const RawDesc d = read_desc(p, i);
  check_that(!row.empty() && row[0].type == ColType::kInt64 && !row[0].is_null,
             "row's key column (column 0) must be a non-null Int64");

  const auto bytes = encode(d.schema, row);
  Heap heap(pool_, d.heap_head,
           [this, table](PageId h) { update_heap_head(table, h); });
  const RID rid = heap.insert(bytes);

  BTree tree(pool_, d.btree_root,
            [this, table](PageId r) { update_btree_root(table, r); });
  if (!tree.insert_new(table_key(row[0].i64), rid_encode(rid))) {
    heap.erase(rid);  // the key is taken: take the row back out
    return std::nullopt;
  }

  // Unit 11: file the row in every index of the table. A NULL is not filed:
  // no comparison with NULL is true, so no search through the index needs it.
  for (std::size_t c = 1; c < d.schema.size(); ++c) {
    if (d.index_roots[c] == kNullPage || row[c].is_null) continue;
    BTree index(pool_, d.index_roots[c],
                [this, table, c](PageId r) { update_index_root(table, c, r); },
                /*unique=*/false);
    index.insert(order_key(row[c]), rid_encode(rid));
  }

  // ... and count it, in the catalog page's own frame: pinned, changed in
  // place, unpinned dirty (Unit 4's real interface), rather than copied out
  // and back in -- and so never from a copy the heap or a tree has since
  // rewritten, to record a new head or root.
  Page& frame = pool_.fetch_pin(cat_page_);
  std::uint8_t* s = slot_at(frame, i);
  store_u64(s + kCatalogRowCountOff, load_u64(s + kCatalogRowCountOff) + 1);
  pool_.unpin(cat_page_, /*dirty=*/true);
  return rid;
}

std::optional<Row> Catalog::get_by_key(const std::string& table, Key key) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  const RawDesc d = read_desc(p, i);

  BTree tree(pool_, d.btree_root, nullptr);  // read path: root will not move
  const auto v = tree.search(table_key(static_cast<std::int64_t>(key)));
  if (!v) return std::nullopt;
  const RID rid = rid_decode(*v);

  Heap heap(pool_, d.heap_head, nullptr);    // heap_head must be valid: a row exists
  const auto bytes = heap.get(rid);
  return decode(d.schema, bytes);
}

void Catalog::scan_table(const std::string& table,
                         const std::function<void(const Row&)>& visit) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  const RawDesc d = read_desc(p, i);

  BTree tree(pool_, d.btree_root, nullptr);
  Heap heap(pool_, d.heap_head, nullptr);
  for (Cursor c = tree.seek(0); c.valid(); c.next()) {
    const RID rid = rid_decode(c.value());
    visit(decode(d.schema, heap.get(rid)));
  }
}

TableCursor::TableCursor(BufferPool& pool, Schema schema, PageId heap_head,
                         PageId root)
    : schema_(std::move(schema)),
      heap_(pool, heap_head, nullptr),
      cur_(BTree(pool, root, nullptr).seek(0)) {}

bool TableCursor::next(Row& out) {
  if (!cur_.valid()) return false;
  out = decode(schema_, heap_.get(rid_decode(cur_.value())));
  cur_.next();
  return true;
}

TableCursor Catalog::open_cursor(const std::string& table) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  const RawDesc d = read_desc(p, i);
  return TableCursor(pool_, d.schema, d.heap_head, d.btree_root);
}

// ------------------------------------------------------------ indexes (Unit 11)

std::uint64_t Catalog::create_index(const std::string& table, std::size_t column) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table (create index)");
  const RawDesc d = read_desc(p, i);
  check_that(column > 0 && column < d.schema.size(), "index column out of range");
  check_that(d.index_roots[column] == kNullPage, "the column is already indexed");

  BTree fresh(pool_, kNullPage, nullptr, /*unique=*/false);  // plant an empty root
  store_u32(slot_at(p, i) + kCatalogDescFixed + column * kCatalogColDescSize + kCatalogColNameLen + 2,
            fresh.root());
  save_catalog_page(p);

  // File every row there is. The table's own tree gives each row's RID; Unit
  // 5's project() decodes the one column the index needs, and nothing else.
  BTree index(pool_, fresh.root(),
              [this, table, column](PageId r) { update_index_root(table, column, r); },
              /*unique=*/false);
  const BTree tree(pool_, d.btree_root, nullptr);
  const Heap heap(pool_, d.heap_head, nullptr);
  std::uint64_t filed = 0;
  for (Cursor c = tree.seek(0); c.valid(); c.next()) {
    const Field v = project(d.schema, heap.get(rid_decode(c.value())), column);
    if (v.is_null) continue;
    index.insert(order_key(v), c.value());
    ++filed;
  }
  return filed;
}

IndexCursor Catalog::open_index(const std::string& table, std::size_t column, ChildRule rule) {
  const RawDesc d = desc_of(table);
  const PageId root = index_root(d, column);
  check_that(root != kNullPage, "no index on this column");
  const bool unique = column == 0;
  return IndexCursor(pool_, d.schema, d.heap_head, root, unique,
                     rule ? rule : unique ? rightmost_child : leftmost_child);
}

std::uint64_t Catalog::count_index(const std::string& table, std::size_t column, Key lo,
                                   Key hi, std::uint64_t cap) {
  const RawDesc d = desc_of(table);
  const PageId root = index_root(d, column);
  check_that(root != kNullPage, "no index on this column");
  const BTree tree(pool_, root, nullptr, /*unique=*/column == 0);
  std::uint64_t n = 0;
  for (Cursor c = tree.seek(lo); c.valid() && c.key() <= hi && n < cap; c.next()) ++n;
  return n;
}

std::optional<std::pair<Key, Key>> Catalog::index_bounds(const std::string& table,
                                                         std::size_t column) {
  const RawDesc d = desc_of(table);
  const PageId root = index_root(d, column);
  check_that(root != kNullPage, "no index on this column");
  const BTree tree(pool_, root, nullptr, /*unique=*/column == 0);
  const Cursor first = tree.seek(0), last = tree.last();
  if (!first.valid()) return std::nullopt;
  return std::make_pair(first.key(), last.key());
}

IndexCursor::IndexCursor(BufferPool& pool, Schema schema, PageId heap_head, PageId root,
                         bool unique, ChildRule rule)
    : schema_(std::move(schema)),
      heap_(pool, heap_head, nullptr),
      tree_(pool, root, nullptr, unique),
      rule_(rule) {}

void IndexCursor::seek(Key lo, Key hi) {
  cur_ = tree_.seek(lo, rule_);
  hi_ = hi;
}

bool IndexCursor::next(Row& out) {
  if (!cur_ || !cur_->valid() || cur_->key() > hi_) return false;
  key_ = cur_->key();
  out = decode(schema_, heap_.get(rid_decode(cur_->value())));
  cur_->next();
  return true;
}

std::optional<Key> IndexCursor::max_key() const {
  const Cursor c = tree_.last();
  if (!c.valid()) return std::nullopt;
  return c.key();
}

}  // namespace labdb
