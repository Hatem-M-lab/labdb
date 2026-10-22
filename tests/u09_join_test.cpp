// Unit 9 correctness: the join grammar, the operator pipeline, and both join
// algorithms checked against an answer computed independently.
//
// The heart of it is a differential test. Two tables are generated from a
// seeded random source -- join keys drawn from a small domain so that most
// keys repeat on both sides (many-to-many matches), with NULLs sprinkled in.
// The same values are kept in plain C++ vectors, and the expected join is
// computed from those vectors with two nested for-loops and SQL's NULL rule,
// sharing no code with the engine. Then the same query runs through the
// engine as a nested-loop join and as a hash join, and all three answers
// must agree row for row.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"
#include "unparse.hpp"

using namespace labdb;
using Algo = Executor::JoinAlgorithm;

namespace {

Statement parse(const std::string& sql) {
  Parser p(sql);
  return p.parse_single();
}

// The parse error must be raised, at exactly this line and column.
void check_error_at(const std::string& sql, std::size_t line, std::size_t col) {
  try {
    parse(sql);
  } catch (const ParseError& e) {
    if (e.line != line || e.col != col)
      std::fprintf(stderr, "error for [%s] at %zu:%zu, expected %zu:%zu (%s)\n",
                   sql.c_str(), e.line, e.col, line, col, e.message.c_str());
    REQUIRE(e.line == line && e.col == col);
    return;
  }
  REQUIRE(false);  // no error at all
}

void check_roundtrip(const std::string& sql) {
  const std::string once = unparse(parse(sql));
  const std::string twice = unparse(parse(once));
  REQUIRE(once == twice);
}

void expect_exec_error(Executor& e, const std::string& sql) {
  bool threw = false;
  try {
    e.run_sql(sql);
  } catch (const ExecError&) {
    threw = true;
  }
  REQUIRE(threw);
}

// One generated row, kept on the C++ side to compute the expected answer.
struct Gen {
  std::int64_t id;
  bool k_null;
  std::int32_t k, v;
  std::string tag;
};

std::vector<Gen> generate(Executor& e, const std::string& table, int n, int domain,
                          std::mt19937_64& rng) {
  static const char* tags[] = {"red", "green", "blue"};
  std::vector<Gen> rows;
  for (int i = 0; i < n; ++i) {
    Gen g;
    g.id = i;
    g.k_null = rng() % 20 == 0;  // about 5% NULL join keys
    g.k = static_cast<std::int32_t>(rng() % domain);
    g.v = static_cast<std::int32_t>(rng() % 100);
    g.tag = tags[rng() % 3];
    rows.push_back(g);
    e.run_sql("INSERT INTO " + table + " VALUES (" + std::to_string(g.id) + ", " +
              (g.k_null ? std::string("NULL") : std::to_string(g.k)) + ", " +
              std::to_string(g.v) + ", '" + g.tag + "')");
  }
  return rows;
}

using Pairs = std::vector<std::pair<std::int64_t, std::int64_t>>;

Pairs id_pairs(const ResultSet& r) {
  Pairs out;
  for (const auto& row : r.rows) out.emplace_back(row[0].i64, row[1].i64);
  std::sort(out.begin(), out.end());
  return out;
}

// The query under every algorithm must equal the expected pairs exactly.
void agree(Executor& e, const std::string& sql, Pairs expected, bool hash_ok = true) {
  std::sort(expected.begin(), expected.end());
  e.set_join_algorithm(Algo::kNestedLoop);
  REQUIRE(id_pairs(e.run_sql(sql)) == expected);
  e.set_join_algorithm(Algo::kAuto);
  REQUIRE(id_pairs(e.run_sql(sql)) == expected);
  e.set_join_algorithm(Algo::kHash);
  if (hash_ok) {
    REQUIRE(id_pairs(e.run_sql(sql)) == expected);
  } else {
    expect_exec_error(e, sql);  // no equality to hash on
  }
  e.set_join_algorithm(Algo::kAuto);
}

}  // namespace

int main() {
  // ---- 1. the grammar: qualified columns, aliases, JOIN ... ON, AND ----
  {
    Statement s = parse(
        "SELECT o.oid, c.name FROM orders o JOIN customers AS c "
        "ON o.cust = c.id AND o.cents > 100 WHERE c.vip = TRUE AND o.oid < 50");
    const SelectStmt& q = s.select;
    REQUIRE(q.columns.size() == 2 && q.columns[0].table == "o" && q.columns[0].column == "oid");
    REQUIRE(q.from.name == "orders" && q.from.alias == "o");
    REQUIRE(q.joins.size() == 1 && q.joins[0].table.name == "customers" &&
            q.joins[0].table.alias == "c");
    REQUIRE(q.joins[0].on.size() == 2);
    REQUIRE(q.joins[0].on[0].right && q.joins[0].on[0].right->table == "c");  // column = column
    REQUIRE(!q.joins[0].on[1].right && q.joins[0].on[1].literal.i64 == 100);  // column > literal
    REQUIRE(q.where.size() == 2 && q.where[1].left.column == "oid");
    REQUIRE(parse("SELECT * FROM a INNER JOIN b ON a.x = b.y").select.joins.size() == 1);

    const std::vector<std::string> batch = {
        "SELECT a.id, b.w FROM a JOIN b ON a.k = b.k AND a.v < b.w WHERE b.w >= 50",
        "SELECT * FROM orders o JOIN customers c ON o.cust = c.id",
        "SELECT x.id FROM a x JOIN a y ON x.k = y.k JOIN b ON b.k = y.k "
        "WHERE x.id < 10 AND y.tag = 'q'",
        "SELECT id FROM t AS u WHERE u.id <> 3",
    };
    for (const auto& sql : batch) check_roundtrip(sql);

    check_error_at("SELECT * FROM a JOIN b WHERE a.k = 1", 1, 24);        // no ON
    check_error_at("SELECT * FROM a JOIN b ON a.k = b.k AND", 1, 40);      // dangling AND
    check_error_at("SELECT a. FROM a", 1, 11);                             // '.' then no column
    check_error_at("SELECT * FROM a WHERE a.k = 1 b.k = 2", 1, 31);        // a missing AND
    std::printf("PASS grammar: join ASTs, %zu round-trips stable, 4 errors caret-located\n",
                batch.size());
  }

  const char* path = "u09_join_test.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 2048);
  Catalog cat(pool);
  Executor e(cat);

  // ---- 2. the pipeline in isolation: RowsScan -> Filter -> Project ----
  {
    const OutSchema schema = {{"m", {"id", ColType::kInt64, false}},
                              {"m", {"x", ColType::kInt32, true}}};
    std::vector<Row> rows;
    for (int i = 0; i < 10; ++i)
      rows.push_back({Field::Int64(i), i == 4 ? Field::Null(ColType::kInt32) : Field::Int32(i * i)});
    BoundComparison gt;  // x > 10
    gt.left = 1;
    gt.op = TokenType::kGt;
    gt.literal = Field::Int64(10);
    Project p(std::make_unique<Filter>(std::make_unique<RowsScan>(schema, rows),
                                       std::vector<BoundComparison>{gt}, "x > 10"),
              {0}, "id");
    p.open();
    std::vector<std::int64_t> got;
    Row r;
    while (p.next(r)) got.push_back(r[0].i64);
    p.close();
    REQUIRE((got == std::vector<std::int64_t>{5, 6, 7, 8, 9}));  // 4 is NULL: never > 10
    REQUIRE(p.rows_out() == 5);
    std::printf("PASS pipeline: RowsScan -> Filter -> Project yields exactly the 5 qualifying rows\n");
  }

  // ---- 3. the differential test: nested loop == hash join == by hand ----
  std::size_t checked_pairs = 0, queries = 0;
  const int domains[] = {1, 7, 50};
  for (int round = 0; round < 3; ++round) {
    const std::string A = "a" + std::to_string(round), B = "b" + std::to_string(round);
    e.run_sql("CREATE TABLE " + A + " (id INT64 NOT NULL, k INT32, v INT32, tag TEXT)");
    e.run_sql("CREATE TABLE " + B + " (id INT64 NOT NULL, k INT32, v INT32, tag TEXT)");
    std::mt19937_64 rng(900 + round);
    const auto a = generate(e, A, 300, domains[round], rng);
    const auto b = generate(e, B, 200, domains[round], rng);

    Pairs eq, eq_res, two_keys, nonequi, eq_where;
    for (const Gen& x : a)
      for (const Gen& y : b) {
        const bool keys_match = !x.k_null && !y.k_null && x.k == y.k;  // NULL matches nothing
        if (keys_match) eq.emplace_back(x.id, y.id);
        if (keys_match && x.v < y.v) eq_res.emplace_back(x.id, y.id);
        if (keys_match && x.tag == y.tag) two_keys.emplace_back(x.id, y.id);
        if (x.v < y.v && !x.k_null && x.k == 3) nonequi.emplace_back(x.id, y.id);
        if (keys_match && y.v >= 50) eq_where.emplace_back(x.id, y.id);
      }
    const std::string sel = "SELECT " + A + ".id, " + B + ".id FROM " + A + " JOIN " + B + " ON ";
    agree(e, sel + A + ".k = " + B + ".k", eq);
    agree(e, sel + A + ".k = " + B + ".k AND " + A + ".v < " + B + ".v", eq_res);
    agree(e, sel + A + ".tag = " + B + ".tag AND " + A + ".k = " + B + ".k", two_keys);
    agree(e, sel + A + ".v < " + B + ".v WHERE " + A + ".k = 3", nonequi, /*hash_ok=*/false);
    agree(e, sel + B + ".k = " + A + ".k WHERE " + B + ".v >= 50", eq_where);  // keys reversed
    checked_pairs += eq.size() + eq_res.size() + two_keys.size() + nonequi.size() + eq_where.size();
    queries += 5;

    // a self-join needs aliases, and they keep the two sides apart
    Pairs self;
    for (const Gen& x : a)
      for (const Gen& y : a)
        if (!x.k_null && !y.k_null && x.k == y.k && x.id < y.id) self.emplace_back(x.id, y.id);
    agree(e, "SELECT x.id, y.id FROM " + A + " x JOIN " + A + " y ON x.k = y.k AND x.id < y.id", self);
    checked_pairs += self.size();
    ++queries;
  }
  std::printf("PASS differential: %zu queries x 3 plans, %zu joined rows, nested loop == hash join == by hand\n",
              queries, checked_pairs);

  // ---- 4. three tables, two joins ----
  {
    e.run_sql("CREATE TABLE customers (id INT64 NOT NULL, name TEXT)");
    e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, cust INT64 NOT NULL)");
    e.run_sql("CREATE TABLE items (iid INT64 NOT NULL, oid INT64 NOT NULL, sku TEXT)");
    for (int c = 0; c < 20; ++c)
      e.run_sql("INSERT INTO customers VALUES (" + std::to_string(c) + ", 'cust" + std::to_string(c) + "')");
    for (int o = 0; o < 100; ++o)  // customer o % 25: customers 20..24 do not exist
      e.run_sql("INSERT INTO orders VALUES (" + std::to_string(o) + ", " + std::to_string(o % 25) + ")");
    std::size_t expected = 0;
    for (int i = 0; i < 300; ++i) {  // item i belongs to order i % 120: orders 100..119 do not exist
      e.run_sql("INSERT INTO items VALUES (" + std::to_string(i) + ", " + std::to_string(i % 120) + ", 'sku" + std::to_string(i) + "')");
      const int o = i % 120;
      if (o < 100 && o % 25 < 20) ++expected;
    }
    const std::string q =
        "SELECT c.name, o.oid, i.sku FROM customers c JOIN orders o ON o.cust = c.id "
        "JOIN items i ON i.oid = o.oid";
    REQUIRE(e.run_sql(q).row_count() == expected);
    e.set_join_algorithm(Algo::kNestedLoop);
    REQUIRE(e.run_sql(q).row_count() == expected);
    e.set_join_algorithm(Algo::kAuto);
    std::printf("PASS three-way join: customers x orders x items = %zu rows, both algorithms\n", expected);
  }

  // ---- 5. semantic errors a join makes possible ----
  expect_exec_error(e, "SELECT id FROM a0 JOIN b0 ON a0.k = b0.k");        // ambiguous: both have id
  expect_exec_error(e, "SELECT a0.id FROM a0 JOIN b0 ON a0.k = c.k");      // no table c
  expect_exec_error(e, "SELECT * FROM a0 JOIN b0 ON a0.tag = b0.k");       // TEXT vs INT32
  expect_exec_error(e, "SELECT * FROM a0 JOIN a0 ON a0.k = a0.k");         // a0 named twice
  expect_exec_error(e, "SELECT * FROM a0 o JOIN b0 ON a0.k = b0.k");       // alias hides the name
  expect_exec_error(e, "SELECT * FROM a0 WHERE tag = 5");                  // TEXT vs integer
  std::printf("PASS errors: 6 join-time semantic errors correctly raised\n");

  // ---- 6. regressions for two Unit 8 bugs found while rebuilding SELECT ----
  {
    e.run_sql("CREATE TABLE p (id INT64 NOT NULL, age INT32)");
    e.run_sql("INSERT INTO p VALUES (1, 0)");
    e.run_sql("INSERT INTO p VALUES (2, 0)");
    e.run_sql("INSERT INTO p VALUES (3, 41)");
    e.run_sql("INSERT INTO p VALUES (4, NULL)");
    REQUIRE(e.run_sql("SELECT id FROM p WHERE age = NULL").row_count() == 0);   // Unit 8: 2
    REQUIRE(e.run_sql("SELECT id FROM p WHERE age <> NULL").row_count() == 0);  // Unit 8: 1
    REQUIRE(e.run_sql("SELECT id FROM p WHERE age = 0").row_count() == 2);
    bool threw = false;
    try {
      e.run_sql("SELECT id FROM p WHERE age = 0 id = 1");  // forgot AND
    } catch (const ParseError&) {
      threw = true;
    }
    REQUIRE(threw);  // Unit 8 silently ignored everything after `age = 0`
    std::printf("PASS regressions: comparing with NULL matches nothing; trailing tokens are an error\n");
  }

  // ---- 7. the plan is visible ----
  // (Unit 9's plans, that is: since Unit 11 the planner chooses for itself,
  // and Unit 11's own test checks what it chooses. Literal planning is Unit
  // 9's planner, kept.)
  {
    e.set_literal_planning(true);
    e.run_sql("SELECT o.oid, c.name FROM orders o JOIN customers c ON o.cust = c.id WHERE o.oid < 10");
    const std::string plan = e.explain_last();
    REQUIRE(plan.find("Project") != std::string::npos);
    REQUIRE(plan.find("HashJoin ON o.cust = c.id") != std::string::npos);
    REQUIRE(plan.find("SeqScan orders AS o") != std::string::npos);
    e.run_sql("SELECT name FROM customers WHERE id = 7");
    REQUIRE(e.explain_last().find("IndexLookup customers key = 7") != std::string::npos);
    e.set_literal_planning(false);
    std::printf("PASS plans: hash join chosen for an equality, index lookup kept for key = literal\n");
  }

  // ---- 8. regression for Challenge 9.5: structured keys must still spread ----
  // Snowflake-style ids -- (milliseconds << 22) | (worker << 12) | sequence --
  // from one worker at low traffic: the low 22 bits never change.
  {
    const OutSchema ks = {{"s", {"id", ColType::kInt64, false}}};
    std::vector<Row> ids;
    std::mt19937_64 rng(95);
    std::uint64_t ms = 120'000'000'000ULL;  // milliseconds since the service's epoch
    for (int i = 0; i < 20000; ++i) {
      ms += 1 + rng() % 50;
      ids.push_back({Field::Int64(static_cast<std::int64_t>((ms << 22) | (1u << 12)))});
    }
    const auto build_and_count = [&](JoinHashTable::MixFn mix, std::size_t& longest) {
      HashJoin hj(std::make_unique<RowsScan>(ks, ids), std::make_unique<RowsScan>(ks, ids),
                  {{0, 0}}, {}, "p.id = s.id", mix);
      hj.open();
      std::size_t n = 0;
      Row r;
      while (hj.next(r)) ++n;
      hj.close();
      longest = hj.table().occupancy().longest;
      return n;
    };
    std::size_t longest_mixed = 0, longest_identity = 0;
    REQUIRE(build_and_count(mix64, longest_mixed) == ids.size());
    REQUIRE(longest_mixed <= 16);  // spread across buckets
    REQUIRE(build_and_count([](std::uint64_t x) { return x; }, longest_identity) == ids.size());
    REQUIRE(longest_identity == ids.size());  // the trap: every key in one bucket
    std::printf("PASS hash spread: 20000 snowflake ids, longest chain %zu with mix64 (%zu without)\n",
                longest_mixed, longest_identity);
  }

  ::unlink(path);
  std::printf("u09_join_test: PASS\n");
  return 0;
}
