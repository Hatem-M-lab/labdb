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
  for (std::uint8_t i = 0; i < ncols; ++i) {
    const std::uint8_t* cd = s + kCatalogDescFixed + i * kCatalogColDescSize;
    Column c;
    c.name.assign(reinterpret_cast<const char*>(cd), strnlen(reinterpret_cast<const char*>(cd), kCatalogColNameLen));
    c.type = static_cast<ColType>(cd[kCatalogColNameLen]);
    c.nullable = cd[kCatalogColNameLen + 1] != 0;
    d.schema.columns.push_back(c);
  }
  return d;
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
  store_u32(slot_at(p, i) + 34, new_head);
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

RID Catalog::insert_row(const std::string& table, const Row& row) {
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
  tree.insert(static_cast<Key>(row[0].i64), rid_encode(rid));
  return rid;
}

std::optional<Row> Catalog::get_by_key(const std::string& table, Key key) {
  Page p;
  load_catalog_page(p);
  const std::size_t i = find_slot(p, table);
  check_that(i != npos, "no such table");
  const RawDesc d = read_desc(p, i);

  BTree tree(pool_, d.btree_root, nullptr);  // read path: root will not move
  const auto v = tree.search(key);
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

}  // namespace labdb
