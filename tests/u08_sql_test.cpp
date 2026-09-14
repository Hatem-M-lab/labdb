// Unit 8 end-to-end: the whole engine answering SQL. Every operation here is
// a SQL string handed to the executor -- no direct catalog, heap, or tree
// calls. It builds tables, inserts rows, and runs SELECTs (projection, every
// WHERE operator, point-lookup vs full-scan, key ordering); then it reopens
// the database cold and queries it again to prove the effect was on disk; and
// it checks that semantic errors are raised rather than silently mishandled.
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"

using namespace labdb;

namespace {

ResultSet run(Executor& e, const std::string& sql) { return e.run_sql(sql); }

// A SELECT that must raise an ExecError.
void expect_exec_error(Executor& e, const std::string& sql) {
  bool threw = false;
  try {
    e.run_sql(sql);
  } catch (const ExecError&) {
    threw = true;
  }
  REQUIRE(threw);
}

std::int64_t as_int(const Field& f) { return f.i64; }

}  // namespace

int main() {
  const char* path = "u08_sql_test.db";
  ::unlink(path);

  // ---- build a database entirely through SQL ----
  {
    Pager pager(path);
    BufferPool pool(pager, 512);
    Catalog cat(pool);
    Executor e(cat);

    run(e, "CREATE TABLE users (id INT64 NOT NULL, name TEXT, age INT32)");
    run(e, "CREATE TABLE orders (oid INT64 NOT NULL, who TEXT, cents INT32 NOT NULL)");

    // insert out of key order on purpose; the scan must return key order
    run(e, "INSERT INTO users VALUES (3, 'Bob', 29)");
    run(e, "INSERT INTO users VALUES (1, 'Ada', 36)");
    run(e, "INSERT INTO users VALUES (2, 'O''Brien', 41)");
    run(e, "INSERT INTO users VALUES (4, 'Cy', 29)");
    for (int i = 0; i < 5000; ++i)
      run(e, "INSERT INTO orders VALUES (" + std::to_string(i) + ", 'c" +
                 std::to_string(i % 100) + "', " + std::to_string(i * 10) + ")");

    // SELECT * returns all columns, in key order despite insert order
    {
      ResultSet r = run(e, "SELECT * FROM users");
      REQUIRE(r.columns.size() == 3 && r.row_count() == 4);
      REQUIRE(as_int(r.rows[0][0]) == 1 && as_int(r.rows[1][0]) == 2 &&
              as_int(r.rows[2][0]) == 3 && as_int(r.rows[3][0]) == 4);
      REQUIRE(r.rows[1][1].text == "O'Brien");  // escaped quote survived SQL->disk->SQL
    }

    // projection + WHERE >= ; only the two ages >= 36
    {
      ResultSet r = run(e, "SELECT name, age FROM users WHERE age >= 36");
      REQUIRE(r.columns.size() == 2 && r.row_count() == 2);
      REQUIRE(r.rows[0][0].text == "Ada" && r.rows[1][0].text == "O'Brien");
    }

    // point lookup on the key column uses the index fast path
    {
      ResultSet r = run(e, "SELECT name FROM users WHERE id = 2");
      REQUIRE(r.row_count() == 1 && r.rows[0][0].text == "O'Brien");
      ResultSet none = run(e, "SELECT name FROM users WHERE id = 999");
      REQUIRE(none.row_count() == 0);
    }

    // every operator, checked against a 5000-row table
    REQUIRE(run(e, "SELECT oid FROM orders WHERE cents < 100").row_count() == 10);
    REQUIRE(run(e, "SELECT oid FROM orders WHERE cents <= 100").row_count() == 11);
    REQUIRE(run(e, "SELECT oid FROM orders WHERE oid = 4242").row_count() == 1);
    REQUIRE(run(e, "SELECT oid FROM orders WHERE oid <> 4242").row_count() == 4999);
    REQUIRE(run(e, "SELECT oid FROM orders WHERE who = 'c7'").row_count() == 50);

    pool.sync();
    std::printf("PASS build+query: 2 tables built and queried entirely through SQL\n");
  }

  // ---- reopen cold and query again ----
  {
    Pager pager(path);
    BufferPool pool(pager, 512);
    Catalog cat(pool);
    Executor e(cat);

    ResultSet r = run(e, "SELECT id, name, age FROM users WHERE age < 30");
    REQUIRE(r.row_count() == 2);  // Bob(29) and Cy(29)
    REQUIRE(run(e, "SELECT * FROM users").row_count() == 4);
    REQUIRE(run(e, "SELECT oid FROM orders").row_count() == 5000);
    REQUIRE(run(e, "SELECT who FROM orders WHERE oid = 1234").rows[0][0].text == "c34");
    std::printf("PASS reopen: the same SQL queries answer correctly from a cold database\n");
  }

  // ---- semantic errors are raised, not mishandled ----
  {
    Pager pager(path);
    BufferPool pool(pager, 512);
    Catalog cat(pool);
    Executor e(cat);

    expect_exec_error(e, "SELECT missing FROM users");           // unknown column
    expect_exec_error(e, "SELECT * FROM ghost");                 // unknown table
    expect_exec_error(e, "INSERT INTO users VALUES (9, 'x')");   // wrong arity
    expect_exec_error(e, "INSERT INTO users VALUES (9, 'x', 'notint')");  // type
    expect_exec_error(e, "CREATE TABLE users (id INT64)");       // duplicate table
    expect_exec_error(e, "INSERT INTO orders VALUES (9, 'x', NULL)");  // NULL in NOT NULL
    std::printf("PASS errors: 6 semantic errors correctly raised\n");
  }

  ::unlink(path);
  std::printf("u08_sql_test: PASS\n");
  return 0;
}
