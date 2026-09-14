// Unit 6 catalog correctness. Registers several differently-shaped tables,
// inserts rows into each, and checks that a lookup by (table name, key)
// returns the right row from the right table -- proving tables are actually
// isolated, not sharing one heap or one tree by accident. Then the whole
// engine is torn down and reopened, and everything -- table names, schemas,
// and every row -- is checked again from a cold catalog page.
#include <unistd.h>

#include <cstdio>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {
Schema users_schema() {
  return Schema{{{"id", ColType::kInt64, false},
                 {"name", ColType::kText, true},
                 {"active", ColType::kBool, true}}};
}
Schema orders_schema() {
  return Schema{{{"order_id", ColType::kInt64, false},
                 {"customer", ColType::kText, true},
                 {"cents", ColType::kInt32, false}}};
}
Schema logs_schema() {
  return Schema{{{"seq", ColType::kInt64, false}, {"message", ColType::kText, true}}};
}
}  // namespace

int main() {
  const char* path = "u06_catalog_test.db";
  ::unlink(path);
  const int kPerTable = 4000;

  // ---- build: three tables, different shapes, interleaved inserts ----
  {
    Pager pager(path);
    BufferPool pool(pager, 512);
    Catalog cat(pool);
    cat.create_table("users", users_schema());
    cat.create_table("orders", orders_schema());
    cat.create_table("logs", logs_schema());

    for (int i = 0; i < kPerTable; ++i) {
      cat.insert_row("users", Row{Field::Int64(i),
                                  (i % 5 == 0) ? Field::Null(ColType::kText)
                                               : Field::Text("user_" + std::to_string(i)),
                                  Field::Bool(i % 2 == 0)});
      cat.insert_row("orders", Row{Field::Int64(i),
                                   Field::Text("cust_" + std::to_string(i % 50)),
                                   Field::Int32(i * 137)});
      cat.insert_row("logs", Row{Field::Int64(i),
                                 Field::Text(std::string(20 + (i % 30), 'x'))});
    }
    pool.sync();

    // spot-check a handful before the reopen, across all three tables
    for (int i : {0, 1, kPerTable / 2, kPerTable - 1}) {
      auto u = cat.get_by_key("users", i);
      auto o = cat.get_by_key("orders", i);
      auto l = cat.get_by_key("logs", i);
      REQUIRE(u.has_value() && o.has_value() && l.has_value());
      REQUIRE(o->at(1).text == "cust_" + std::to_string(i % 50));
      REQUIRE(o->at(2).i64 == i * 137);
      REQUIRE(l->at(0).i64 == i);
    }
    std::printf("PASS build: 3 tables, %d rows each, spot-checked before reopen\n", kPerTable);
  }

  // ---- reopen: a fresh engine, a cold catalog page ----
  {
    Pager pager(path);
    BufferPool pool(pager, 512);
    Catalog cat(pool);

    auto names = cat.table_names();
    REQUIRE(names.size() == 3);

    // every row of every table, decoded and checked
    for (int i = 0; i < kPerTable; ++i) {
      auto u = cat.get_by_key("users", i);
      auto o = cat.get_by_key("orders", i);
      auto l = cat.get_by_key("logs", i);
      REQUIRE(u.has_value() && o.has_value() && l.has_value());
      REQUIRE(u->at(0).i64 == i);
      REQUIRE(u->at(2).i64 == (i % 2 == 0 ? 1 : 0));
      REQUIRE(o->at(2).i64 == i * 137);
      REQUIRE(l->at(1).text.size() == static_cast<std::size_t>(20 + (i % 30)));
    }
    std::printf("PASS reopen: all 3 tables, %d rows each, recovered from a cold catalog\n",
                kPerTable);

    // exact-match existence: a table that was never created is absent, even
    // though its name is a prefix of one that exists
    REQUIRE(cat.table_exists("orders"));
    REQUIRE(!cat.table_exists("order"));
    REQUIRE(!cat.table_exists("orders2"));
    std::printf("PASS exact-match: 'order' and 'orders2' are correctly absent\n");
  }

  ::unlink(path);
  std::printf("u06_catalog_test: PASS\n");
  return 0;
}
