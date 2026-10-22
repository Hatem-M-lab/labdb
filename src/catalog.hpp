#pragma once
// Catalog -- a directory of named tables. Unit 6.
//
// Through Unit 5 the engine had exactly one table, reached through one
// global heap and one global tree, both pinned to fixed slots in the meta
// page. That was enough to prove the record layer worked; it is not a
// database. A database has *names*: you ask for "orders", not for "the
// heap at page 185." This is where the engine learns its own tables exist.
//
// The catalog is one fixed-format page: a directory of TableDesc slots, each
// holding a table's name, its schema, and the two page ids that are that
// table's own heap head and B+Tree root. It is deliberately NOT a slotted
// page -- every slot is the same fixed size, direct-addressed by index, for
// the same reason Unit 5 gave every column a fixed slot: a directory this
// small does not need slotted's variable-length machinery, and fixed offsets
// keep "find table by name" and "update table's root in place" both simple,
// single-page operations.
//
// A table's heap and tree are opened through Heap's and BTree's per-table
// constructors (Unit 6's small addition to both): given a table's last known
// root and a callback, they report back whenever that root moves, and the
// catalog writes the new value into that table's slot. Neither the heap nor
// the tree needs to know a catalog exists; they just report an address change.
//
// Unit 11 gives a table secondary indexes -- at most one per column, made by
// CREATE INDEX -- and two numbers a planner needs: how many rows the table
// holds and how many pages its heap spans, both kept exact on every insert.
// An index is a non-unique B+Tree from a column's order_key to the RID of
// each row holding that value (NULLs are not indexed: no comparison with NULL
// is ever true, so no search needs them). Its root lives in the column's own
// descriptor, beside the column's name and type.

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "common.hpp"
#include "heap.hpp"
#include "record.hpp"

namespace labdb {

// A pull-based scan of one table in key order (Unit 9) -- the
// iterator-shaped twin of Catalog::scan_table. scan_table *pushes* every row
// into a callback and returns when the table is done, which is all a
// one-table SELECT needed. An operator pipeline must instead *pull* one row,
// hand it upward, and resume later; so the scan's state -- the tree cursor and
// the open heap -- lives in this object between calls.
class TableCursor {
 public:
  // Decode the next row into `out`. False once the table is exhausted.
  bool next(Row& out);
  const Schema& schema() const { return schema_; }

 private:
  friend class Catalog;
  TableCursor(BufferPool& pool, Schema schema, PageId heap_head, PageId root);

  Schema schema_;
  Heap heap_;
  Cursor cur_;
};

// Unit 11: the key a table's own index files a row under -- the order_key of
// its INT64 key column, so that the tree's unsigned order is the keys' signed
// order (through Unit 10 it was the raw bits, and a negative key would have
// sorted after every positive one).
inline Key table_key(std::int64_t id) { return static_cast<Key>(id) ^ (Key{1} << 63); }

// Unit 11: what a planner needs to know about a table, read in one go.
struct TableInfo {
  Schema schema;
  std::uint64_t rows = 0;        // exact: counted by every insert
  std::uint32_t heap_pages = 0;  // pages in the table's heap
  std::vector<bool> indexed;     // per column: column 0 always (the key); others by CREATE INDEX
};

// Unit 11: a pull-based walk through one index, the twin of TableCursor: the
// rows whose index key lies in [lo, hi], in key order. For the key column
// (column 0) the index is the table's own B+Tree; for any other column it is
// that column's secondary index. Each entry's value is a RID, and the row is
// fetched from the heap by it -- the same fetch a scan makes, in another order.
class IndexCursor {
 public:
  // Position before the first entry with key >= lo; next() stops after hi.
  void seek(Key lo, Key hi);
  // Decode the row of the next entry in range into `out`; false past hi.
  bool next(Row& out);
  Key key() const { return key_; }  // the key of the entry next() last returned
  // The largest key in the index, if it holds any entry.
  std::optional<Key> max_key() const;
  const Schema& schema() const { return schema_; }

 private:
  friend class Catalog;
  IndexCursor(BufferPool& pool, Schema schema, PageId heap_head, PageId root,
              bool unique, ChildRule rule);

  Schema schema_;
  Heap heap_;
  BTree tree_;
  ChildRule rule_;
  std::optional<Cursor> cur_;
  Key hi_ = 0;
  Key key_ = 0;
};

inline constexpr std::size_t kCatalogMaxNameLen = 32;
inline constexpr std::size_t kCatalogMaxColumns = 8;
inline constexpr std::size_t kCatalogColNameLen = 16;

// Bytes one column's descriptor occupies inside a TableDesc slot: name, type,
// nullable, and (Unit 11) the root of the column's secondary index, or 0.
inline constexpr std::size_t kCatalogColDescSize = kCatalogColNameLen + 2 + 4;

// Bytes one TableDesc slot occupies in the catalog page:
//   [0 .. 32)  name, raw bytes, unused tail zeroed
//   [32]       name_len (u8)          -- the ACTUAL length; never trust a
//                                          scan of the padding to find it
//   [33]       ncols (u8)
//   [34..38)   heap_head (u32)
//   [38..42)   btree_root (u32)
//   [42..50)   row_count (u64)        -- Unit 11
//   [50..54)   heap_pages (u32)       -- Unit 11
//   [54 .. 54 + ncols*22)  column descriptors, each:
//              name[16] + type(u8) + nullable(u8) + index_root(u32, Unit 11)
inline constexpr std::size_t kCatalogRowCountOff = 42;
inline constexpr std::size_t kCatalogHeapPagesOff = 50;
inline constexpr std::size_t kCatalogDescFixed = kCatalogMaxNameLen + 1 + 1 + 4 + 4 + 8 + 4;  // 54
inline constexpr std::size_t kCatalogSlotSize =
    kCatalogDescFixed + kCatalogMaxColumns * kCatalogColDescSize;  // 54 + 8*22 = 230

// Directory header at the top of the catalog page's body (after the 20-byte
// page header): a 2-byte slot count.
inline constexpr std::size_t kCatalogCountOff = kPageHeaderSize;      // 20
inline constexpr std::size_t kCatalogSlotsOff = kCatalogCountOff + 4; // 24 (2 pad)
inline constexpr std::size_t kCatalogMaxTables =
    (kPageSize - kCatalogSlotsOff) / kCatalogSlotSize;  // capacity of one page

class Catalog {
 public:
  explicit Catalog(BufferPool& pool);

  // Register a new table. Fails if the name already exists (checked with an
  // EXACT match -- see Challenge 6.4), the schema has more than
  // kCatalogMaxColumns columns, or a name/column name is too long.
  void create_table(const std::string& name, const Schema& schema);

  // Exact-match existence check and schema lookup.
  bool table_exists(const std::string& name) const;
  Schema schema_of(const std::string& name) const;
  // Both at once, in one read of the catalog page: the schema, or nothing if
  // there is no such table -- an expected outcome, so not an exception (Unit 9).
  std::optional<Schema> find_schema(const std::string& name) const;
  std::vector<std::string> table_names() const;

  // Unit 11: the schema, the row and page counts, and which columns are
  // indexed, in one read of the catalog page; nothing if there is no table.
  std::optional<TableInfo> find_table(const std::string& name) const;

  // Unit 11: CREATE INDEX -- plant an empty non-unique tree for `column`, record
  // its root in the column's descriptor, and file every existing row with a
  // non-NULL value in it. Returns the number of entries filed. The executor
  // checks the column exists, is not the key, and has no index yet.
  std::uint64_t create_index(const std::string& table, std::size_t column);

  // Unit 11: open a walk through the index of `column` (0: the table's own
  // tree). `rule` overrides how the walk descends (Challenge 11.5); nullptr
  // means the tree's own rule.
  IndexCursor open_index(const std::string& table, std::size_t column,
                         ChildRule rule = nullptr);

  // Unit 11: how many entries of that index have keys in [lo, hi], counted
  // through its leaves without fetching a row -- but no more than `cap`: past
  // that, the count stops, and `cap` is returned.
  std::uint64_t count_index(const std::string& table, std::size_t column, Key lo,
                            Key hi, std::uint64_t cap);

  // Unit 11: the first and last keys of that index, if it holds any entry --
  // one descent to each end.
  std::optional<std::pair<Key, Key>> index_bounds(const std::string& table, std::size_t column);

  // The buffer pool's size, in pages: what a planner needs to guess how often
  // a page it asks for is already in memory.
  std::size_t pool_frames() const { return pool_.capacity(); }

  // Insert a row into a named table. Column 0 is that table's key (a
  // limitation stated plainly: no separate PRIMARY KEY declaration yet).
  // Since Unit 11 the row is also filed in every index of the table, and the
  // table's row count goes up by one -- and a row whose key the table already
  // holds is refused: nothing is written, and the result is empty. (Through
  // Unit 10 it silently replaced the old row in the table's own tree.)
  std::optional<RID> insert_row(const std::string& table, const Row& row);

  // Look a row up by its key column, through that table's own index --
  // reached by name, not by a heap or tree handle the caller had to keep.
  std::optional<Row> get_by_key(const std::string& table, Key key);

  // Visit every row of a named table in key order, decoded. The executor
  // uses this for a SELECT that is not a point lookup on the key column: it
  // walks the table's own tree cursor, fetches each row from the table's
  // heap, and hands the decoded Row to `visit`. Returning rows in key order
  // falls out of the B+Tree leaf chain for free (Unit 3's Cursor).
  void scan_table(const std::string& table,
                  const std::function<void(const Row&)>& visit);

  // Open a pull-based scan of a table, positioned before its first row
  // (Unit 9: the operator pipeline's SeqScan reads through one of these).
  TableCursor open_cursor(const std::string& table);

 private:
  struct RawDesc {  // an in-memory read of one on-page slot
    std::string name;
    Schema schema;
    PageId heap_head;
    PageId btree_root;
    std::uint64_t rows;                 // Unit 11
    std::uint32_t heap_pages;           // Unit 11
    std::array<PageId, kCatalogMaxColumns> index_roots{};  // Unit 11: per column, kNullPage if none
  };

  void load_catalog_page(Page& out) const { pool_.read_page(cat_page_, out); }
  void save_catalog_page(Page& in) { pool_.write_page(cat_page_, in); }

  std::uint16_t slot_count(const Page& p) const { return load_u16(p.data() + kCatalogCountOff); }
  void set_slot_count(Page& p, std::uint16_t n) { store_u16(p.data() + kCatalogCountOff, n); }
  std::uint8_t* slot_at(Page& p, std::size_t i) const {
    return p.data() + kCatalogSlotsOff + i * kCatalogSlotSize;
  }
  const std::uint8_t* slot_at(const Page& p, std::size_t i) const {
    return p.data() + kCatalogSlotsOff + i * kCatalogSlotSize;
  }

  // EXACT-match slot lookup by name. Returns the slot index or npos. This is
  // Challenge 6.4's fix embodied: it compares the stored length AND the
  // bytes, so "orders" can never match a query for "order".
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);
  std::size_t find_slot(const Page& p, const std::string& name) const;

  void write_desc(Page& p, std::size_t slot, const std::string& name,
                  const Schema& schema, PageId heap_head, PageId btree_root);
  RawDesc read_desc(const Page& p, std::size_t slot) const;
  void update_heap_head(const std::string& name, PageId new_head);
  void update_btree_root(const std::string& name, PageId new_root);
  void update_index_root(const std::string& name, std::size_t column, PageId new_root);
  // The root of the tree that indexes `column`: the table's own for column 0.
  static PageId index_root(const RawDesc& d, std::size_t column) {
    return column == 0 ? d.btree_root : d.index_roots[column];
  }
  RawDesc desc_of(const std::string& table) const;  // read the slot, or fail

  BufferPool& pool_;
  PageId cat_page_;
};

}  // namespace labdb
