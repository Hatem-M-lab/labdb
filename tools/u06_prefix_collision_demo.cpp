// THE BUG, preserved for study: matching a catalog name by comparing bytes
// only up to the QUERY's length, without checking that the STORED name is
// exactly that long. `memcmp(stored, query, query.size())` is true whenever
// `stored` merely *starts with* `query` -- so a table that does not exist can
// appear to exist, as long as some other table's name happens to begin with
// it. No error, no exception: the catalog reports the wrong table's heap and
// tree as the answer for a name nobody ever created.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc tools/u06_prefix_collision_demo.cpp
//        src/io.cpp src/pager.cpp src/buffer_pool.cpp src/btree.cpp src/heap.cpp
//        src/catalog.cpp -o bin/u06_prefix_collision_demo
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {
// The buggy lookup: same directory scan as Catalog::find_slot, but the
// length check is missing -- it compares only query.size() bytes and never
// asks whether the stored name is exactly that long.
bool buggy_table_exists(BufferPool& pool, PageId cat_page, const std::string& query) {
  Page p;
  pool.read_page(cat_page, p);
  const std::uint16_t n = load_u16(p.data() + kCatalogCountOff);
  for (std::uint16_t i = 0; i < n; ++i) {
    const std::uint8_t* s = p.data() + kCatalogSlotsOff + i * kCatalogSlotSize;
    if (std::memcmp(s, query.data(), query.size()) == 0)  // no length check!
      return true;
  }
  return false;
}
}  // namespace

int main() {
  const char* path = "u06_prefix_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 32);
  Catalog cat(pool);

  // Only ONE table exists: "orders". "order" was never created.
  cat.create_table("orders", Schema{{{"id", ColType::kInt64, false}}});

  const bool correct = cat.table_exists("order");   // exact match: absent
  const bool buggy = buggy_table_exists(pool, pool.catalog_root(), "order");  // prefix match

  std::printf(
      "Only 'orders' was created. Querying for 'order' (which was never "
      "created):\n"
      "  correct table_exists(\"order\") = %s\n"
      "  buggy   table_exists(\"order\") = %s%s\n",
      correct ? "true" : "false", buggy ? "true" : "false",
      (correct == buggy) ? "" : "   <-- WRONG: matched 'orders' by prefix");

  std::printf(
      "\nThe buggy scan compares only the first 5 bytes of every stored name "
      "against \"order\", and \"orders\" begins with exactly those 5 bytes. "
      "It never checks that the stored name is ALSO only 5 bytes long, so a "
      "table that was never created is reported to exist -- silently, and "
      "with the real 'orders' table's heap and tree behind it.\n");

  ::unlink(path);
  return 0;
}
