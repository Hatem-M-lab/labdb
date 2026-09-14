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

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "common.hpp"
#include "record.hpp"

namespace labdb {

inline constexpr std::size_t kCatalogMaxNameLen = 32;
inline constexpr std::size_t kCatalogMaxColumns = 8;
inline constexpr std::size_t kCatalogColNameLen = 16;

// Bytes one column's descriptor occupies inside a TableDesc slot.
inline constexpr std::size_t kCatalogColDescSize = kCatalogColNameLen + 2;  // name + type + nullable

// Bytes one TableDesc slot occupies in the catalog page:
//   [0 .. 32)  name, raw bytes, unused tail zeroed
//   [32]       name_len (u8)          -- the ACTUAL length; never trust a
//                                          scan of the padding to find it
//   [33]       ncols (u8)
//   [34..38)   heap_head (u32)
//   [38..42)   btree_root (u32)
//   [42 .. 42 + ncols*18)  column descriptors, each: name[16] + type(u8) + nullable(u8)
inline constexpr std::size_t kCatalogDescFixed = kCatalogMaxNameLen + 1 + 1 + 4 + 4;  // 42
inline constexpr std::size_t kCatalogSlotSize =
    kCatalogDescFixed + kCatalogMaxColumns * kCatalogColDescSize;  // 42 + 8*18 = 186

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
  std::vector<std::string> table_names() const;

  // Insert a row into a named table. Column 0 is that table's key (a
  // limitation stated plainly: no separate PRIMARY KEY declaration yet).
  RID insert_row(const std::string& table, const Row& row);

  // Look a row up by its key column, through that table's own index --
  // reached by name, not by a heap or tree handle the caller had to keep.
  std::optional<Row> get_by_key(const std::string& table, Key key);

 private:
  struct RawDesc {  // an in-memory read of one on-page slot
    std::string name;
    Schema schema;
    PageId heap_head;
    PageId btree_root;
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

  BufferPool& pool_;
  PageId cat_page_;
};

}  // namespace labdb
