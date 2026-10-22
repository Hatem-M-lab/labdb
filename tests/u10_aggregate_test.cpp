// Unit 10 correctness: aggregates, GROUP BY, HAVING, ORDER BY and LIMIT,
// checked against answers computed independently.
//
// The heart of it is a differential test, as in Unit 9. A table is generated
// from a seeded random source -- grouping columns drawn from small domains,
// NULLs sprinkled into every column but the key -- and the same values are
// kept in plain C++ vectors. Every expected answer is computed from those
// vectors with std::map, std::stable_sort and SQL's NULL rules written out by
// hand, sharing no code with the engine; then the engine must produce the
// same rows, cell for cell, in the same order.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"
#include "unparse.hpp"

using namespace labdb;

namespace {

using Table = std::vector<std::vector<std::string>>;

Statement parse(const std::string& sql) {
  Parser p(sql);
  return p.parse_single();
}

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
  REQUIRE(false);
}

void check_roundtrip(const std::string& sql, const std::string& expected) {
  const std::string once = unparse(parse(sql));
  if (once != expected) std::fprintf(stderr, "unparse: [%s]\n  want: [%s]\n", once.c_str(), expected.c_str());
  REQUIRE(once == expected);
  REQUIRE(unparse(parse(once)) == once);
}

// The engine's error for `sql` must contain `part`.
void expect_exec_error(Executor& e, const std::string& sql, const std::string& part) {
  try {
    e.run_sql(sql);
  } catch (const ExecError& x) {
    if (x.message.find(part) == std::string::npos)
      std::fprintf(stderr, "for [%s]: [%s], expected [%s]\n", sql.c_str(), x.message.c_str(),
                   part.c_str());
    REQUIRE(x.message.find(part) != std::string::npos);
    return;
  }
  std::fprintf(stderr, "no error for [%s]\n", sql.c_str());
  REQUIRE(false);
}

// A cell, exactly: a DOUBLE with all 17 significant digits.
std::string render(const Field& f) {
  if (f.is_null) return "NULL";
  switch (f.type) {
    case ColType::kText: return "'" + f.text + "'";
    case ColType::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", f.f64);
      return buf;
    }
    default: return std::to_string(f.i64);
  }
}

Table rows_of(const ResultSet& r) {
  Table t;
  for (const auto& row : r.rows) {
    t.emplace_back();
    for (const Field& f : row) t.back().push_back(render(f));
  }
  return t;
}

void require_same(const Table& got, const Table& want, const std::string& sql) {
  if (got != want) {
    std::fprintf(stderr, "MISMATCH for %s: %zu rows, expected %zu\n", sql.c_str(), got.size(),
                 want.size());
    for (std::size_t i = 0; i < std::max(got.size(), want.size()) && i < 12; ++i) {
      std::string g, w;
      if (i < got.size()) for (const auto& c : got[i]) g += c + " ";
      if (i < want.size()) for (const auto& c : want[i]) w += c + " ";
      std::fprintf(stderr, "  got [%s]  want [%s]\n", g.c_str(), w.c_str());
    }
  }
  REQUIRE(got == want);
}

// ------------------------------------------------- the reference, by hand

struct Gen {
  std::int64_t id;
  std::optional<std::int64_t> a;  // INT32, 0..4
  std::optional<std::string> b;   // TEXT, x / y / z
  std::optional<std::int64_t> v;  // INT64, 0..1000
  std::int64_t w;                 // INT32, 0..9, never NULL
};

std::string cell(const std::optional<std::int64_t>& x) { return x ? std::to_string(*x) : "NULL"; }
std::string cell(const std::optional<std::string>& x) { return x ? "'" + *x + "'" : "NULL"; }

// COUNT(*), COUNT(v), SUM(v), AVG(v), MIN(v), MAX(v) over some rows, by SQL's
// rules: NULLs are skipped by all but COUNT(*); with no values, SUM, AVG,
// MIN and MAX are NULL and COUNT is 0.
std::vector<std::string> six(const std::vector<const Gen*>& rows) {
  std::int64_t n = 0, sum = 0, lo = 0, hi = 0;
  for (const Gen* r : rows) {
    if (!r->v) continue;
    lo = n == 0 ? *r->v : std::min(lo, *r->v);
    hi = n == 0 ? *r->v : std::max(hi, *r->v);
    sum += *r->v;
    ++n;
  }
  char avg[64];
  std::snprintf(avg, sizeof avg, "%.17g", static_cast<double>(sum) / static_cast<double>(n));
  return {std::to_string(rows.size()), std::to_string(n), n ? std::to_string(sum) : "NULL",
          n ? std::string(avg) : "NULL", n ? std::to_string(lo) : "NULL",
          n ? std::to_string(hi) : "NULL"};
}

// GROUP BY `key`, groups in order of first appearance, each row the key's
// cells then `aggs` of the group's rows; only groups `keep` accepts.
Table group_ref(const std::vector<Gen>& data, const std::function<bool(const Gen&)>& where,
                const std::function<std::vector<std::string>(const Gen&)>& key,
                const std::function<std::vector<std::string>(const std::vector<const Gen*>&)>& aggs,
                const std::function<bool(const std::vector<const Gen*>&)>& keep = nullptr) {
  std::map<std::vector<std::string>, std::size_t> index;
  std::vector<std::vector<std::string>> keys;
  std::vector<std::vector<const Gen*>> members;
  for (const Gen& g : data) {
    if (!where(g)) continue;
    const auto k = key(g);
    auto it = index.find(k);
    if (it == index.end()) {
      it = index.emplace(k, keys.size()).first;
      keys.push_back(k);
      members.emplace_back();
    }
    members[it->second].push_back(&g);
  }
  Table t;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (keep && !keep(members[i])) continue;
    auto row = keys[i];
    for (auto& c : aggs(members[i])) row.push_back(c);
    t.push_back(row);
  }
  return t;
}

std::optional<std::int64_t> sum_of(const std::vector<const Gen*>& rows) {
  std::optional<std::int64_t> s;
  for (const Gen* r : rows)
    if (r->v) s = s.value_or(0) + *r->v;
  return s;
}

// ORDER BY, by SQL's rules as this engine documents them: a NULL sorts after
// every value ascending and before every value descending; a full tie keeps
// input order (here, id order).
template <class Key>
int cmp_nullable(const std::optional<Key>& x, const std::optional<Key>& y, bool desc) {
  if (!x && !y) return 0;
  if (!x) return desc ? -1 : 1;  // NULL: last ascending, first descending
  if (!y) return desc ? 1 : -1;
  const int c = (*x > *y) - (*x < *y);
  return desc ? -c : c;
}

}  // namespace

int main() {
  // ------------------------------------------------------------ grammar
  check_roundtrip(
      "select count(*), Sum(t.x) as total, max(y) from t where y >= 2.25 group by t.z, w "
      "having count(*) > 1 order by total desc, w asc limit 10",
      "SELECT COUNT(*), SUM(t.x) AS total, MAX(y) FROM t WHERE y >= 2.25 GROUP BY t.z, w "
      "HAVING COUNT(*) > 1 ORDER BY total DESC, w LIMIT 10");
  check_roundtrip("SELECT count FROM t GROUP BY count ORDER BY count",  // not reserved
                  "SELECT count FROM t GROUP BY count ORDER BY count");
  check_roundtrip("SELECT x FROM t WHERE x > 0.0000001 AND y < 2.0",
                  "SELECT x FROM t WHERE x > 0.0000001 AND y < 2.0");
  check_error_at("SELECT AVG(*) FROM t", 1, 12);            // only COUNT can take *
  check_error_at("SELECT FOO(x) FROM t", 1, 8);             // unknown function
  check_error_at("SELECT COUNT(x FROM t", 1, 16);           // ')' after the argument
  check_error_at("SELECT COUNT() FROM t", 1, 14);           // a column name
  check_error_at("SELECT x FROM t GROUP x", 1, 23);         // BY after GROUP
  check_error_at("SELECT x FROM t ORDER x", 1, 23);         // BY after ORDER
  check_error_at("SELECT x FROM t LIMIT", 1, 22);           // a row count after LIMIT
  check_error_at("SELECT x FROM t LIMIT 2.5", 1, 23);       // ... an integer one
  check_error_at("SELECT x AS FROM t", 1, 13);              // an alias after AS
  check_error_at("SELECT x FROM t LIMIT 3 WHERE x = 1", 1, 25);  // clauses in order
  check_error_at("SELECT id name FROM users", 1, 11);       // Unit 7's: an alias needs AS
  std::printf("PASS grammar: aggregates, aliases, the four clauses, decimals; 11 errors caret-located\n");

  const char* path = "u10_aggregate_test.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 256);
  Catalog cat(pool);
  Executor e(cat);

  // ------------------------------------------- whole-table aggregates, NULLs
  e.run_sql("CREATE TABLE r (id INT64 NOT NULL, product TEXT, stars INT32)");
  for (const char* v : {"(1, 'lamp', 5)", "(2, 'lamp', NULL)", "(3, 'lamp', 3)",
                        "(4, 'desk', 4)", "(5, 'desk', 4)", "(6, 'mug', NULL)",
                        "(7, NULL, 2)", "(8, NULL, NULL)", "(9, 'desk', 1)"})
    e.run_sql(std::string("INSERT INTO r VALUES ") + v);
  require_same(rows_of(e.run_sql("SELECT COUNT(*), COUNT(stars), SUM(stars), AVG(stars), "
                                 "MIN(stars), MAX(stars), MIN(product), MAX(product) FROM r")),
               {{"9", "6", "19", render(Field::Double(19.0 / 6.0)), "1", "5", "'desk'", "'mug'"}},
               "whole table");
  // all-NULL input: COUNT is 0, everything else NULL -- one row, not zero
  require_same(rows_of(e.run_sql("SELECT COUNT(*), COUNT(stars), SUM(stars), AVG(stars), "
                                 "MIN(stars), MAX(stars) FROM r WHERE product = 'mug'")),
               {{"1", "0", "NULL", "NULL", "NULL", "NULL"}}, "all NULL");
  require_same(rows_of(e.run_sql("SELECT COUNT(*), SUM(stars), MAX(product) FROM r WHERE id > 100")),
               {{"0", "NULL", "NULL"}}, "no rows, no GROUP BY: one row");
  require_same(rows_of(e.run_sql("SELECT product, COUNT(*) FROM r WHERE id > 100 GROUP BY product")),
               {}, "no rows, GROUP BY: no groups");
  // NULL keys form one group; groups come out in order of first appearance
  require_same(rows_of(e.run_sql("SELECT product, COUNT(*), COUNT(stars), AVG(stars) FROM r "
                                 "GROUP BY product")),
               {{"'lamp'", "3", "2", "4"}, {"'desk'", "3", "3", "3"},
                {"'mug'", "1", "0", "NULL"}, {"NULL", "2", "1", "2"}},
               "NULL group");
  REQUIRE(e.run_sql("SELECT AVG(stars) FROM r").rows[0][0].type == ColType::kDouble);
  std::printf("PASS aggregates: COUNT(*) counts rows, the rest skip NULL; empty and all-NULL inputs; NULL keys one group\n");

  // ----------------------------------- the differential test: GROUP BY, HAVING
  std::mt19937_64 rng(1010);
  std::vector<Gen> data;
  e.run_sql("CREATE TABLE g (id INT64 NOT NULL, a INT32, b TEXT, v INT64, w INT32)");
  const char* names[] = {"x", "y", "z"};
  for (std::int64_t i = 0; i < 3000; ++i) {
    Gen r{i, std::nullopt, std::nullopt, std::nullopt, static_cast<std::int64_t>(rng() % 10)};
    if (rng() % 10) r.a = static_cast<std::int64_t>(rng() % 5);
    if (rng() % 10) r.b = names[rng() % 3];
    if (rng() % 5) r.v = static_cast<std::int64_t>(rng() % 1001);
    data.push_back(r);
    e.run_sql("INSERT INTO g VALUES (" + std::to_string(i) + ", " + cell(r.a) + ", " + cell(r.b) +
              ", " + cell(r.v) + ", " + std::to_string(r.w) + ")");
  }
  e.run_sql("CREATE TABLE h (id INT64 NOT NULL, label TEXT)");
  for (int i = 0; i < 4; ++i)  // a = 4 has no label: those rows drop out of the join
    e.run_sql("INSERT INTO h VALUES (" + std::to_string(i) + ", 'label" + std::to_string(i % 3) + "')");

  const auto all = [](const Gen&) { return true; };
  const auto by_a = [](const Gen& r) { return std::vector<std::string>{cell(r.a)}; };
  const std::string SIX = "COUNT(*), COUNT(v), SUM(v), AVG(v), MIN(v), MAX(v)";
  struct Case {
    std::string sql;
    Table want;
  };
  std::vector<Case> cases;
  cases.push_back({"SELECT a, " + SIX + " FROM g GROUP BY a", group_ref(data, all, by_a, six)});
  cases.push_back({"SELECT b, a, " + SIX + " FROM g GROUP BY b, a",
                   group_ref(data, all, [](const Gen& r) { return std::vector<std::string>{cell(r.b), cell(r.a)}; },
                             six)});
  cases.push_back({"SELECT " + SIX + " FROM g",
                   group_ref(data, all, [](const Gen&) { return std::vector<std::string>{}; }, six)});
  cases.push_back({"SELECT a, " + SIX + " FROM g WHERE w < 5 GROUP BY a",
                   group_ref(data, [](const Gen& r) { return r.w < 5; }, by_a, six)});
  cases.push_back({"SELECT " + SIX + " FROM g WHERE w > 100",
                   {{"0", "0", "NULL", "NULL", "NULL", "NULL"}}});
  cases.push_back({"SELECT a, MIN(b), MAX(b), COUNT(b) FROM g GROUP BY a",
                   group_ref(data, all, by_a, [](const std::vector<const Gen*>& rows) {
                     std::optional<std::string> lo, hi;
                     std::int64_t n = 0;
                     for (const Gen* r : rows) {
                       if (!r->b) continue;
                       ++n;
                       if (!lo || *r->b < *lo) lo = r->b;
                       if (!hi || *r->b > *hi) hi = r->b;
                     }
                     return std::vector<std::string>{cell(lo), cell(hi), std::to_string(n)};
                   })});
  // HAVING: on an aggregate, on one not in the select list, on a group column
  const auto count_only = [](const std::vector<const Gen*>& rows) {
    return std::vector<std::string>{std::to_string(rows.size())};
  };
  cases.push_back({"SELECT a, COUNT(*) FROM g GROUP BY a HAVING COUNT(*) > 545",
                   group_ref(data, all, by_a, count_only,
                             [](const std::vector<const Gen*>& rows) { return rows.size() > 545; })});
  cases.push_back({"SELECT b, COUNT(*) FROM g GROUP BY b HAVING SUM(v) >= 300000",
                   group_ref(data, all, [](const Gen& r) { return std::vector<std::string>{cell(r.b)}; },
                             count_only, [](const std::vector<const Gen*>& rows) {
                               const auto s = sum_of(rows);
                               return s && *s >= 300000;
                             })});
  cases.push_back({"SELECT a, COUNT(*) FROM g GROUP BY a HAVING a >= 3 AND COUNT(v) > 1",
                   group_ref(data, all, by_a, count_only, [](const std::vector<const Gen*>& rows) {
                     std::size_t nv = 0;
                     for (const Gen* r : rows) nv += r->v.has_value();
                     return rows[0]->a && *rows[0]->a >= 3 && nv > 1;
                   })});
  cases.push_back({"SELECT a, AVG(v) FROM g GROUP BY a HAVING AVG(v) > 500.5",
                   group_ref(data, all, by_a,
                             [](const std::vector<const Gen*>& rows) { return std::vector<std::string>{six(rows)[3]}; },
                             [](const std::vector<const Gen*>& rows) {
                               std::int64_t n = 0, s = 0;
                               for (const Gen* r : rows)
                                 if (r->v) { ++n; s += *r->v; }
                               return n && static_cast<double>(s) / static_cast<double>(n) > 500.5;
                             })});
  // a join, then groups: rows whose a is NULL or 4 have no partner in h
  cases.push_back({"SELECT h.label, COUNT(*), SUM(g.v) FROM g JOIN h ON g.a = h.id GROUP BY h.label",
                   group_ref(data, [](const Gen& r) { return r.a && *r.a < 4; },
                             [](const Gen& r) { return std::vector<std::string>{"'label" + std::to_string(*r.a % 3) + "'"}; },
                             [](const std::vector<const Gen*>& rows) {
                               return std::vector<std::string>{std::to_string(rows.size()), cell(sum_of(rows))};
                             })});
  std::size_t groups = 0;
  for (const Case& c : cases) {
    require_same(rows_of(e.run_sql(c.sql)), c.want, c.sql);
    groups += c.want.size();
  }
  std::printf("PASS differential: %zu queries, %zu result rows, engine = an answer computed by hand\n",
              cases.size(), groups);

  // ------------------------------------------------- ORDER BY and top n
  std::vector<std::size_t> idx(data.size());
  for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  const auto sorted_ids = [&](const std::function<int(const Gen&, const Gen&)>& cmp) {
    std::vector<std::size_t> o = idx;
    std::stable_sort(o.begin(), o.end(), [&](std::size_t x, std::size_t y) { return cmp(data[x], data[y]) < 0; });
    Table t;
    for (std::size_t i : o) t.push_back({std::to_string(data[i].id)});
    return t;
  };
  struct SortCase {
    std::string order;
    std::function<int(const Gen&, const Gen&)> cmp;
  };
  const std::vector<SortCase> sorts = {
      {"v", [](const Gen& x, const Gen& y) { return cmp_nullable(x.v, y.v, false); }},
      {"v DESC", [](const Gen& x, const Gen& y) { return cmp_nullable(x.v, y.v, true); }},
      {"a, v DESC", [](const Gen& x, const Gen& y) {
         const int c = cmp_nullable(x.a, y.a, false);
         return c ? c : cmp_nullable(x.v, y.v, true);
       }},
      {"b DESC, w", [](const Gen& x, const Gen& y) {
         const int c = cmp_nullable(x.b, y.b, true);
         return c ? c : cmp_nullable(std::optional<std::int64_t>(x.w), std::optional<std::int64_t>(y.w), false);
       }},
  };
  std::size_t topn_checks = 0;
  for (const SortCase& s : sorts) {
    const Table full = sorted_ids(s.cmp);
    const std::string sql = "SELECT id FROM g ORDER BY " + s.order;
    require_same(rows_of(e.run_sql(sql)), full, sql);
    for (std::size_t n : {0, 1, 7, 100, 2999, 3000, 5000}) {
      const Table want(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(std::min(n, full.size())));
      const std::string q = sql + " LIMIT " + std::to_string(n);
      require_same(rows_of(e.run_sql(q)), want, q);
      const std::string held = "rows held: " + std::to_string(std::min(n, full.size())) + " of 3000 read";
      REQUIRE(e.explain_last().find(held) != std::string::npos);
      ++topn_checks;
    }
  }
  // ordering groups by an aggregate, through its alias
  {
    Table want = group_ref(data, all, by_a, [](const std::vector<const Gen*>& rows) {
      return std::vector<std::string>{cell(sum_of(rows))};
    });
    std::stable_sort(want.begin(), want.end(), [](const auto& x, const auto& y) {
      return std::stoll(x[1]) > std::stoll(y[1]);  // every group has a sum
    });
    require_same(rows_of(e.run_sql("SELECT a, SUM(v) AS total FROM g GROUP BY a ORDER BY total DESC")),
                 want, "ORDER BY an alias");
    want.resize(2);
    require_same(rows_of(e.run_sql("SELECT a, SUM(v) AS total FROM g GROUP BY a ORDER BY SUM(v) DESC LIMIT 2")),
                 want, "ORDER BY an aggregate, top 2");
  }
  // SELECT * keeps every column through the sort
  require_same(rows_of(e.run_sql("SELECT * FROM r ORDER BY stars DESC, id LIMIT 4")),
               {{"2", "'lamp'", "NULL"}, {"6", "'mug'", "NULL"}, {"8", "NULL", "NULL"},
                {"1", "'lamp'", "5"}},
               "SELECT * ORDER BY");
  std::printf("PASS order: %zu sorts and %zu top-n queries = std::stable_sort with NULLs last ascending, first descending\n",
              sorts.size() + 3, topn_checks + 1);

  // ------------------------------------------------- LIMIT stops reading
  {
    Executor::Plan p = e.plan(parse("SELECT id FROM g LIMIT 5").select);
    p.root->open();
    Row row;
    std::size_t n = 0;
    while (p.root->next(row)) ++n;
    p.root->close();
    const Operator* leaf = p.root.get();
    while (!leaf->children().empty()) leaf = leaf->children()[0];
    REQUIRE(n == 5 && leaf->rows_out() == 5);  // the scan produced 5 rows, not 3000
  }
  std::printf("PASS limit: LIMIT 5 without ORDER BY reads 5 rows from its scan, not 3000\n");

  // ------------------------------------------------------- decimals
  {
    Table want;
    for (const Gen& r : data)
      if (r.v && static_cast<double>(*r.v) > 500.5) want.push_back({std::to_string(r.id)});
    require_same(rows_of(e.run_sql("SELECT id FROM g WHERE v > 500.5")), want, "v > 500.5");
    require_same(rows_of(e.run_sql("SELECT id FROM g WHERE id = 2.0")), {{"2"}}, "id = 2.0");
    REQUIRE(e.explain_last().find("IndexLookup") == std::string::npos);  // a value, not a key
    require_same(rows_of(e.run_sql("SELECT id FROM g WHERE id = 2.5")), {}, "id = 2.5");
    require_same(rows_of(e.run_sql("SELECT id FROM g WHERE id = 2")), {{"2"}}, "id = 2");
    REQUIRE(e.explain_last().find("IndexLookup") != std::string::npos);  // unchanged
  }
  std::printf("PASS decimals: compared by value; a decimal key literal is filtered, not looked up\n");

  // ---------------------------------------------------- semantic errors
  expect_exec_error(e, "SELECT * FROM g GROUP BY a", "SELECT * cannot be used");
  expect_exec_error(e, "SELECT id, COUNT(*) FROM g", "column id must appear in GROUP BY");
  expect_exec_error(e, "SELECT a, v FROM g GROUP BY a", "column v must appear in GROUP BY");
  expect_exec_error(e, "SELECT a FROM g GROUP BY a HAVING v > 1", "column v must appear in GROUP BY");
  expect_exec_error(e, "SELECT a FROM g GROUP BY a ORDER BY v", "column v must appear in GROUP BY");
  expect_exec_error(e, "SELECT id FROM g WHERE COUNT(*) > 1", "not allowed in WHERE or ON");
  expect_exec_error(e, "SELECT g.id FROM g JOIN h ON MAX(g.a) = h.id", "not allowed in WHERE or ON");
  expect_exec_error(e, "SELECT SUM(b) FROM g", "SUM needs an integer column; b is TEXT");
  expect_exec_error(e, "SELECT AVG(nope) FROM g", "no such column: nope");
  e.run_sql("CREATE TABLE big (id INT64 NOT NULL, v INT64)");
  e.run_sql("INSERT INTO big VALUES (1, 9223372036854775807)");
  e.run_sql("INSERT INTO big VALUES (2, 1)");
  expect_exec_error(e, "SELECT SUM(v) FROM big", "integer overflow in SUM");
  require_same(rows_of(e.run_sql("SELECT MAX(v), COUNT(v) FROM big")), {{"9223372036854775807", "2"}}, "max");
  std::printf("PASS errors: 10 semantic errors raised, SUM overflow among them\n");

  // ------------------------------------------------------------ plans
  e.run_sql("SELECT a, SUM(v) AS total FROM g WHERE w < 5 GROUP BY a HAVING COUNT(*) > 1 "
            "ORDER BY total DESC LIMIT 3");
  const std::string plan = e.explain_last();
  for (const char* part : {"Project a, SUM(v) AS total", "Sort total DESC (top 3)",
                           "rows held: 3 of 6 read", "Filter COUNT(*) > 1",
                           "HashAggregate GROUP BY a: SUM(v), COUNT(*)", "6 groups in a table of 16 slots",
                           "Filter w < 5", "SeqScan g"})
    if (plan.find(part) == std::string::npos) {
      std::fprintf(stderr, "plan lacks [%s]:\n%s", part, plan.c_str());
      REQUIRE(false);
    }
  std::printf("PASS plans: Project <- Sort (top n) <- Filter (HAVING) <- HashAggregate <- Filter (WHERE) <- scan\n");

  ::unlink(path);
  std::printf("u10_aggregate_test: PASS\n");
  return 0;
}
