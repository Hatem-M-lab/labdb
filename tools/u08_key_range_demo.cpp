// THE BUG, preserved for study: using the index point-lookup fast path for
// ANY comparison on the key column, not just equality. The executor is right
// to special-case `WHERE key = value` into a single B+Tree search instead of a
// full scan -- that is the whole point of having an index. The bug is applying
// that shortcut to `WHERE key > value` or `key <> value` too: a point lookup
// answers "is this one key present," which is the wrong question for a range.
// The result is silently, catastrophically incomplete -- a handful of rows,
// or none, where thousands should come back -- with no error at all.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc tools/u08_key_range_demo.cpp
//        src/io.cpp src/pager.cpp src/buffer_pool.cpp src/btree.cpp src/heap.cpp
//        src/catalog.cpp src/parser.cpp src/executor.cpp -o bin/u08_key_range_demo
#include <unistd.h>

#include <cstdio>
#include <optional>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "parser.hpp"

using namespace labdb;

namespace {

// The buggy planner: it takes the point-lookup fast path whenever the WHERE
// column is the key, ignoring which operator it is. For any operator other
// than '=', a single get_by_key is the wrong plan.
std::size_t buggy_count(Catalog& cat, const std::string& table,
                        const SelectStmt& s) {
  const Schema schema = cat.schema_of(table);
  // "optimize": WHERE on the key column -> one index lookup. (No operator check.)
  if (s.where && s.where->column == schema.columns[0].name) {
    auto row = cat.get_by_key(table, static_cast<Key>(s.where->literal.i64));
    return row ? 1 : 0;   // a point lookup can only ever return 0 or 1
  }
  // (a real scan would go here for the non-key case)
  std::size_t n = 0;
  cat.scan_table(table, [&](const Row&) { ++n; });
  return n;
}

}  // namespace

int main() {
  const char* path = "u08_keyrange_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 512);
  Catalog cat(pool);
  Executor e(cat);

  e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT32)");
  for (int i = 1; i <= 1000; ++i)
    e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " +
              std::to_string(i) + ")");

  const std::string q = "SELECT id FROM t WHERE id > 900";
  Parser p(q);
  const Statement stmt = p.parse_statement();

  const std::size_t correct = e.run_sql(q).row_count();          // real executor
  const std::size_t buggy = buggy_count(cat, "t", stmt.select);  // buggy planner

  std::printf("1000 rows with id 1..1000.  Query: %s\n\n", q.c_str());
  std::printf("  correct plan (scan + filter): %zu rows  (ids 901..1000)\n", correct);
  std::printf("  buggy plan (index point lookup): %zu rows%s\n", buggy,
              correct == buggy ? "" :
              "   <-- WRONG: a point lookup on key 900 answered a range query");

  std::printf(
      "\nThe buggy planner saw a WHERE on the key column and reached for the "
      "index, but `id > 900` is not a question a single lookup can answer. "
      "get_by_key(900) returns the one row whose key equals 900 -- so the "
      "query reports %zu row where %zu should come back, no error, just a "
      "silently truncated result. Equality is the ONLY operator the "
      "point-lookup shortcut is valid for; every other operator on the key "
      "must still scan.\n",
      buggy, correct);

  ::unlink(path);
  return 0;
}
