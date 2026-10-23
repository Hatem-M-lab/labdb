// Unit 12's correctness test: the batch path against the row path.
//
// The row path is Units 9 to 11's, and their tests checked it against answers
// computed by hand. Here it is the reference. Every query is run a row at a
// time and in batches of 1, 2, 3, 7, 64 and 1,024 rows, and every run must
// return the same rows in the same order, through the same plan with the same
// row counts. Small batch sizes are where batches end in odd places: a bucket
// half walked, a group first seen in the last row of a batch, a filter that
// empties a batch.
//
//   1. the codec: decode_into and decode_to agree with decode, NULLs and all
//   2. the cursors: rows read in place equal rows copied out, and the switch
//      that keeps Unit 11's copies works
//   3. the differential test: generated queries over three tables, both ways
//   4. the contract: a plan drained with random budgets hands out batches of
//      one row to the budget, never more, never none, and the same rows
//   5. batches that empty: a clustered table where whole batches fail a
//      filter, a probe batch that matches nothing, a HAVING that drops whole
//      batches of groups
//   6. LIMIT: in a batch plan the scan below it still reads n rows
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "batch.hpp"
#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"

using namespace labdb;

namespace {

const std::size_t kSizes[] = {1, 2, 3, 7, 64, 1024};

// A value as a typed string, so that two results compare exactly: the type,
// and the value to the last bit of a double.
std::string cell(const Field& f) {
  if (f.is_null) return "N" + std::to_string(static_cast<int>(f.type));
  char buf[64];
  switch (f.type) {
    case ColType::kText: return "T" + f.text;
    case ColType::kDouble: std::snprintf(buf, sizeof buf, "D%.17g", f.f64); return buf;
    default: return "I" + std::to_string(static_cast<int>(f.type)) + ":" + std::to_string(f.i64);
  }
}

std::string render(const ResultSet& r) {
  std::string s;
  for (const std::string& c : r.columns) s += c + "|";
  s += "\n";
  for (const auto& row : r.rows) {
    for (const Field& f : row) s += cell(f) + "|";
    s += "\n";
  }
  return s;
}

void open_db(const char* path, std::size_t frames, const std::function<void(Catalog&, Executor&)>& f) {
  Pager pager(path);
  BufferPool pool(pager, frames);
  Catalog cat(pool);
  Executor e(cat);
  f(cat, e);
}

std::string lit(std::mt19937_64& rng, int col) {
  static const char* words[] = {"alpha", "alphabet", "alphabetical", "beta", "", "zz", "name_1",
                                "name_12", "name_123456789", "O'Brien", "gamma ray burst source", "b"};
  if (rng() % 30 == 0) return "NULL";
  switch (col) {
    case 0: return std::to_string(static_cast<std::int64_t>(rng() % 9100));
    case 1: return rng() % 8 == 0 ? std::to_string(rng() % 10) + ".5" : std::to_string(rng() % 11);
    case 2: return std::to_string(static_cast<std::int64_t>(rng() % 1000000));
    case 3: {
      std::string w = words[rng() % 12];
      std::string q;
      for (char ch : w) q += ch == '\'' ? std::string("''") : std::string(1, ch);
      return "'" + q + "'";
    }
    default: return rng() % 2 ? "TRUE" : "FALSE";
  }
}

const char* kT[] = {"id", "a", "b", "s", "f"};
const char* kOps[] = {"=", "<>", "<", "<=", ">", ">="};

std::string term(std::mt19937_64& rng, const std::string& q) {
  const int col = static_cast<int>(rng() % 5);
  if (rng() % 8 == 0) {  // a column against a column
    const char* pairs[][2] = {{"a", "b"}, {"id", "b"}, {"b", "a"}, {"a", "id"}};
    const auto& p = pairs[rng() % 4];
    return q + p[0] + " " + kOps[rng() % 6] + " " + q + p[1];
  }
  return q + kT[col] + " " + kOps[rng() % 6] + " " + lit(rng, col);
}

std::string where(std::mt19937_64& rng, const std::string& q, int max_terms) {
  std::string w;
  const int n = static_cast<int>(rng() % static_cast<std::uint64_t>(max_terms + 1));
  for (int k = 0; k < n; ++k) w += (w.empty() ? " WHERE " : " AND ") + term(rng, q);
  return w;
}

}  // namespace

int main() {
  const char* path = "u12_batch_test.db";
  ::unlink(path);
  std::mt19937_64 rng(1201);

  // ------------------------------------------------------------ 1. the codec
  {
    const Schema s{{{"id", ColType::kInt64, false}, {"a", ColType::kInt32, true}, {"s", ColType::kText, true},
                    {"f", ColType::kBool, true}, {"d", ColType::kDouble, true}, {"t", ColType::kText, true}}};
    const RecordLayout layout(s);
    const std::vector<ColType> types{ColType::kInt64, ColType::kInt32, ColType::kText,
                                     ColType::kBool, ColType::kDouble, ColType::kText};
    Batch b;
    b.format(types, 64);
    Row reused;
    std::size_t checked = 0;
    for (int i = 0; i < 2000; ++i) {
      Row r{Field::Int64(static_cast<std::int64_t>(rng()) / 3),
            rng() % 5 == 0 ? Field::Null(ColType::kInt32) : Field::Int32(static_cast<std::int32_t>(rng())),
            rng() % 5 == 0 ? Field::Null(ColType::kText) : Field::Text(std::string(rng() % 40, 'a' + rng() % 26)),
            rng() % 5 == 0 ? Field::Null(ColType::kBool) : Field::Bool(rng() % 2),
            rng() % 5 == 0 ? Field::Null(ColType::kDouble) : Field::Double(static_cast<double>(rng() % 1000) / 7.0),
            rng() % 3 == 0 ? Field::Null(ColType::kText) : Field::Text(std::string(rng() % 3, 'x'))};
      const auto bytes = encode(s, r);
      const Row want = decode(s, bytes);
      decode_into(s, bytes, reused);  // into a row that held the previous one
      REQUIRE(reused.size() == want.size());
      for (std::size_t c = 0; c < want.size(); ++c) REQUIRE(cell(reused[c]) == cell(want[c]));
      const std::size_t at = static_cast<std::size_t>(i) % 64;
      decode_to(layout, bytes, b, at);
      Row back;
      b.get_row(at, back);
      for (std::size_t c = 0; c < want.size(); ++c) REQUIRE(cell(back[c]) == cell(want[c]));
      ++checked;
    }
    std::printf("PASS codec: %zu rows of six types, NULLs among them: decode_into and decode_to = decode\n",
                checked);
  }

  // ------------------------------------------------------------- the tables
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, a INT32, b INT64, s TEXT, f BOOL)");
    e.run_sql("CREATE TABLE u (id INT64 NOT NULL, t_id INT64, tag TEXT, w INT32)");
    e.run_sql("CREATE TABLE c (id INT64 NOT NULL, day INT64, v INT64)");
    std::vector<std::int64_t> ids(3000);
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<std::int64_t>(i) * 3;
    std::shuffle(ids.begin(), ids.end(), rng);  // out of key order: the heap is not in key order
    for (std::int64_t id : ids) {
      std::mt19937_64 r(static_cast<std::uint64_t>(id) * 7919 + 1);
      const std::string s = lit(r, 3);
      e.run_sql("INSERT INTO t VALUES (" + std::to_string(id) + ", " +
                (r() % 10 == 0 ? "NULL" : std::to_string(r() % 10)) + ", " +
                (r() % 20 == 0 ? "NULL" : std::to_string(r() % 1000000)) + ", " + s + ", " +
                (r() % 8 == 0 ? "NULL" : r() % 2 ? "TRUE" : "FALSE") + ")");
    }
    for (int i = 0; i < 4000; ++i)
      e.run_sql("INSERT INTO u VALUES (" + std::to_string(i) + ", " +
                (rng() % 15 == 0 ? "NULL" : std::to_string((rng() % 3300) * 3)) + ", " + lit(rng, 3) + ", " +
                std::to_string(rng() % 50) + ")");
    for (int i = 0; i < 10000; ++i)  // clustered: day rises with id
      e.run_sql("INSERT INTO c VALUES (" + std::to_string(i) + ", " + std::to_string(i / 100) + ", " +
                std::to_string(rng() % 1000) + ")");
    e.run_sql("CREATE INDEX ON t (a)");
    e.run_sql("CREATE INDEX ON t (s)");
    e.run_sql("CREATE INDEX ON u (t_id)");
  });

  // ------------------------------------------------------------ 2. cursors
  open_db(path, 4096, [&](Catalog& cat, Executor&) {
    const auto all = [&](bool copy, bool through_index) {
      cat.set_copy_pages(copy);
      std::vector<std::string> out;
      Row r;
      if (through_index) {
        IndexCursor ic = cat.open_index("t", 3);
        ic.seek(0, ~Key{0});
        while (ic.next(r)) {
          std::string line;
          for (const Field& f : r) line += cell(f) + "|";
          out.push_back(line);
        }
      } else {
        TableCursor tc = cat.open_cursor("t");
        while (tc.next(r)) {
          std::string line;
          for (const Field& f : r) line += cell(f) + "|";
          out.push_back(line);
        }
      }
      return out;
    };
    REQUIRE(all(true, false) == all(false, false));
    REQUIRE(all(true, true) == all(false, true));
    REQUIRE(all(false, false).size() == 3000);
    cat.set_copy_pages(false);
    std::printf("PASS cursors: 3000 rows read in place = 3000 rows copied out, by scan and through an index\n");
  });

  // ------------------------------------------------- 3. the differential test
  std::vector<std::string> queries;
  for (int q = 0; q < 160; ++q) {  // one table: columns, terms, sometimes an order and a limit
    std::string sql = std::string("SELECT ") + (rng() % 4 == 0 ? "*" : "id, a, s, b") + " FROM t" + where(rng, "", 3);
    if (rng() % 2) {
      const char* keys[] = {"a", "s DESC", "b", "f, a DESC", "s, id", "a DESC, s", "id DESC"};
      sql += std::string(" ORDER BY ") + keys[rng() % 7];
      if (rng() % 2) sql += " LIMIT " + std::to_string(rng() % 3 == 0 ? 5000 : rng() % 40);
    } else if (rng() % 4 == 0) {
      sql += " LIMIT " + std::to_string(rng() % 50);
    }
    queries.push_back(sql);
  }
  for (int q = 0; q < 90; ++q) {  // aggregates
    const char* groups[] = {"", "a", "s", "f", "a, f", "s, a"};
    const std::string g = groups[rng() % 6];
    const char* aggs[] = {"COUNT(*)", "COUNT(s)", "SUM(b)", "AVG(a)", "MIN(s)", "MAX(b)", "MIN(a)", "MAX(s)",
                          "SUM(a)", "COUNT(f)"};
    std::string list = g;
    const int na = 1 + static_cast<int>(rng() % 3);
    for (int k = 0; k < na; ++k) list += (list.empty() ? "" : ", ") + std::string(aggs[rng() % 10]);
    std::string sql = "SELECT " + list + " FROM t" + where(rng, "", 2);
    if (!g.empty()) sql += " GROUP BY " + g;
    if (rng() % 3 == 0) sql += std::string(" HAVING COUNT(*) ") + kOps[rng() % 6] + " " + std::to_string(rng() % 300);
    if (!g.empty() && rng() % 2) sql += " ORDER BY " + std::string(rng() % 2 ? "COUNT(*) DESC, " : "") + g;
    if (!g.empty() && rng() % 3 == 0) sql += " LIMIT " + std::to_string(rng() % 8);
    queries.push_back(sql);
  }
  for (int q = 0; q < 70; ++q) {  // joins: either order, terms on either side, a residual ON term
    std::string sql = rng() % 2 ? "SELECT t.id, u.id, t.s, u.tag FROM t JOIN u ON u.t_id = t.id"
                                : "SELECT u.id, t.id, u.w, t.a FROM u JOIN t ON t.id = u.t_id";
    if (rng() % 4 == 0) sql += std::string(" AND u.w ") + kOps[rng() % 6] + " t.a";
    std::vector<std::string> terms;
    for (int n = static_cast<int>(rng() % 3); n > 0; --n) terms.push_back(term(rng, "t."));
    if (rng() % 2) terms.push_back("u.w < " + std::to_string(rng() % 50));
    for (std::size_t i = 0; i < terms.size(); ++i) sql += (i ? " AND " : " WHERE ") + terms[i];
    if (rng() % 3 == 0) sql += " ORDER BY t.id, u.id LIMIT " + std::to_string(rng() % 60);
    queries.push_back(sql);
  }
  for (int q = 0; q < 25; ++q) {  // aggregates over joins, and a TEXT join key
    if (q % 2)
      queries.push_back("SELECT t.a, COUNT(*), SUM(u.w), MAX(u.tag) FROM t JOIN u ON u.t_id = t.id" +
                        where(rng, "t.", 1) + " GROUP BY t.a ORDER BY t.a");
    else
      queries.push_back("SELECT u.id, t.id FROM u JOIN t ON t.s = u.tag WHERE u.w = " +
                        std::to_string(rng() % 50) + " AND t.a < " + std::to_string(rng() % 10));
  }
  queries.push_back("SELECT x.id, y.id FROM u x JOIN t ON t.id = x.t_id JOIN u y ON y.t_id = t.id WHERE t.a = 3");
  queries.push_back("SELECT a, s, a, id FROM t WHERE b < 500000");  // a column named twice
  queries.push_back("SELECT s, s FROM t WHERE a = 3 ORDER BY s DESC LIMIT 20");
  queries.push_back("SELECT MIN(a), MAX(a), MIN(s), MAX(s) FROM t");
  queries.push_back("SELECT * FROM c WHERE day >= 95");
  queries.push_back("SELECT COUNT(*), SUM(v) FROM c WHERE day >= 50 AND v < 10");
  queries.push_back("SELECT day, COUNT(*) FROM c WHERE v > 990 GROUP BY day HAVING COUNT(*) > 0");

  const Executor::JoinAlgorithm algos[] = {Executor::JoinAlgorithm::kAuto, Executor::JoinAlgorithm::kHash,
                                           Executor::JoinAlgorithm::kHashLeft,
                                           Executor::JoinAlgorithm::kIndexNestedLoop};
  const Executor::AccessPath paths[] = {Executor::AccessPath::kAuto, Executor::AccessPath::kScan,
                                        Executor::AccessPath::kIndex};
  std::size_t runs = 0, plans_compared = 0, rows_compared = 0;
  std::map<std::string, std::size_t> used;
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    for (const std::string& sql : queries) {
      const bool join = sql.find("JOIN") != std::string::npos;
      for (auto algo : algos) {
        if (!join && algo != Executor::JoinAlgorithm::kAuto) continue;
        e.set_join_algorithm(algo);
        for (auto path_kind : paths) {
          e.set_access_path(path_kind);
          e.set_batch_rows(0);
          std::string want, want_plan, err;
          try {
            want = render(e.run_sql(sql));
            want_plan = e.explain_last(true);
          } catch (const ExecError& x) {
            err = x.message;
          }
          for (std::size_t n : kSizes) {
            e.set_batch_rows(n);
            std::string got, got_plan, got_err;
            try {
              got = render(e.run_sql(sql));
              got_plan = e.explain_last(true);
            } catch (const ExecError& x) {
              got_err = x.message;
            }
            ++runs;
            if (got != want || got_err != err) {
              std::fprintf(stderr, "batch %zu disagrees on: %s\nrows:\n%s\nwant:\n%s\n%s|%s\n", n, sql.c_str(),
                           got.c_str(), want.c_str(), got_err.c_str(), err.c_str());
              REQUIRE(false);
            }
            // The same plan, with the same counts -- but under a LIMIT a join or a
            // nested loop reads its inputs a batch at a time, so their counts there
            // may run past where the row path stopped.
            const bool limited_join = join && sql.find("LIMIT") != std::string::npos;
            if (!limited_join && got_plan != want_plan) {
              std::fprintf(stderr, "batch %zu: another plan for %s\n%s\nwant:\n%s\n", n, sql.c_str(),
                           got_plan.c_str(), want_plan.c_str());
              REQUIRE(false);
            }
            plans_compared += !limited_join;
          }
          rows_compared += static_cast<std::size_t>(std::count(want.begin(), want.end(), '\n'));
          for (const char* op : {"SeqScan", "Filter", "IndexLookup", "IndexScan", "IndexMinMax",
                                 "NestedLoopJoin", "HashJoin", "IndexNestedLoopJoin", "HashAggregate", "Sort",
                                 "Limit", "Project"})
            if ((" " + want_plan).find(" " + std::string(op) + " ") != std::string::npos) ++used[op];
        }
      }
    }
    e.set_join_algorithm(Executor::JoinAlgorithm::kAuto);
    e.set_access_path(Executor::AccessPath::kAuto);
    // the nested loop: small inputs only
    for (int q = 0; q < 12; ++q) {
      const std::string sql = "SELECT t.id, u.id FROM t JOIN u ON u.w " + std::string(kOps[rng() % 6]) +
                              " t.a WHERE t.id < " + std::to_string(rng() % 300) + " AND u.id < " +
                              std::to_string(rng() % 200);
      e.set_batch_rows(0);
      const std::string want = render(e.run_sql(sql));
      const std::string want_plan = e.explain_last();
      for (std::size_t n : kSizes) {
        e.set_batch_rows(n);
        REQUIRE(render(e.run_sql(sql)) == want);
        REQUIRE(e.explain_last() == want_plan);
        ++runs;
      }
      ++used["NestedLoopJoin"];
    }
    e.set_batch_rows(Executor::kChoose);
  });
  std::printf("PASS differential: %zu queries, %zu runs in batches of 1 to 1024 rows = the row path: "
              "%zu result rows each way, %zu plans with the same counts\n",
              queries.size() + 12, runs, rows_compared, plans_compared);
  std::printf("     plans using each operator:");
  std::size_t k = 0;
  for (const auto& [op, n] : used) std::printf("%s %s %zu", k++ % 4 == 0 ? "\n      " : ",", op.c_str(), n);
  std::printf("\n");

  // ------------------------------------------------------------ 4. the contract
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    std::size_t batches = 0;
    for (const char* sql : {"SELECT id, s FROM t WHERE a < 5", "SELECT a, COUNT(*) FROM t GROUP BY a",
                            "SELECT t.id, u.id FROM t JOIN u ON u.t_id = t.id WHERE u.w < 9",
                            "SELECT id FROM t ORDER BY s, id", "SELECT * FROM c WHERE v < 3"}) {
      e.set_batch_rows(0);
      const ResultSet want = e.run_sql(sql);
      for (std::size_t n : {std::size_t{1}, std::size_t{5}, std::size_t{100}}) {
        Parser p(sql);
        Executor::Plan plan = e.plan(p.parse_single().select);
        plan.root->set_batch_rows(n);
        plan.root->open();
        Batch b;
        ResultSet got;
        std::size_t budget = 1 + rng() % (2 * n);
        while (plan.root->next_batch(b, budget)) {
          REQUIRE(b.count() >= 1 && b.count() <= budget && b.count() <= n);
          for (std::size_t j = 0; j < b.count(); ++j) {
            got.rows.emplace_back();
            b.get_row(b.row(j), got.rows.back());
          }
          ++batches;
          budget = 1 + rng() % (2 * n);
        }
        plan.root->close();
        got.columns = want.columns;
        REQUIRE(render(got) == render(want));
      }
    }
    e.set_batch_rows(Executor::kChoose);
    std::printf("PASS contract: %zu batches drained with random budgets, each 1 row to its budget; "
                "the same rows\n", batches);
  });

  // ------------------------------------------------------- 5. batches that empty
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    // c: day = id / 100, so in key order whole batches share their days and
    // fail a term on day together.
    std::size_t emptied = 0;
    for (std::size_t n : kSizes) {
      e.set_batch_rows(n);
      for (int d = 0; d <= 100; d += 7) {
        const ResultSet r = e.run_sql("SELECT COUNT(*) FROM c WHERE day >= " + std::to_string(d));
        REQUIRE(r.rows[0][0].i64 == std::max(0, 10000 - 100 * d));
        const ResultSet s = e.run_sql("SELECT COUNT(*) FROM c WHERE day < " + std::to_string(d) +
                                      " AND v >= 0");
        REQUIRE(s.rows[0][0].i64 == std::min(10000, 100 * d));
        emptied += 2;
      }
      // a probe batch that matches nothing: c's ids above 9000 have no t row
      const ResultSet j = e.run_sql("SELECT COUNT(*) FROM c JOIN t ON t.id = c.id WHERE c.id >= 0");
      std::size_t want = 0;
      for (std::int64_t id = 0; id < 10000; ++id) want += id % 3 == 0 && id < 9000;
      REQUIRE(static_cast<std::size_t>(j.rows[0][0].i64) == want);
      // HAVING that drops every group of most batches of groups
      const ResultSet h = e.run_sql("SELECT day, COUNT(*) FROM c GROUP BY day HAVING day >= 99");
      REQUIRE(h.row_count() == 1 && h.rows[0][0].i64 == 99 && h.rows[0][1].i64 == 100);
      emptied += 2;
    }
    e.set_batch_rows(Executor::kChoose);
    std::printf("PASS emptied batches: %zu runs in which whole batches fail a filter, find no match, "
                "or lose every group -- every count exact at every batch size\n", emptied);
  });

  // ------------------------------------------------------------ 6. LIMIT
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    for (std::size_t n : kSizes) {
      e.set_batch_rows(n);
      e.run_sql("SELECT id, v FROM c LIMIT 5");
      const std::string plan = e.explain_last();
      REQUIRE(plan.find("SeqScan c") != std::string::npos);
      REQUIRE(plan.find("rows=5\n", plan.find("SeqScan c")) != std::string::npos);
      e.run_sql("SELECT id FROM c WHERE v < 100 LIMIT 7");
      const std::string p2 = e.explain_last();
      e.set_batch_rows(0);
      e.run_sql("SELECT id FROM c WHERE v < 100 LIMIT 7");
      REQUIRE(e.explain_last() == p2);  // the scan stopped at the same row
    }
    e.set_batch_rows(Executor::kChoose);
    std::printf("PASS limit: below a LIMIT the scan reads exactly what the row path reads, at every batch size\n");
  });

  // --------------------------------------------- 7. the executor's own choice
  // By default each plan runs in batches only if some operator of it is
  // expected to handle cost::kBatchFrom rows or more (Challenge 12.6). c has
  // 10,000 rows, keyed 0 to 9999, so a key range's estimate is its exact count.
  open_db(path, 4096, [&](Catalog&, Executor& e) {
    REQUIRE(e.batch_rows() == Executor::kChoose);
    const std::size_t below = static_cast<std::size_t>(cost::kBatchFrom);
    const struct {
      std::string sql;
      std::size_t expect;  // what the executor should choose
    } cases[] = {
        {"SELECT * FROM c WHERE id = 5", 0},                                          // one row
        {"SELECT id, v FROM c WHERE id < " + std::to_string(below - 1), 0},          // just too few
        {"SELECT id, v FROM c WHERE id < " + std::to_string(below), kDefaultBatchRows},  // just enough
        {"SELECT COUNT(*) FROM c", kDefaultBatchRows},                                // a scan of 10,000
        {"SELECT day, COUNT(*) FROM c GROUP BY day", kDefaultBatchRows},
        {"SELECT MIN(id), MAX(id) FROM c", 0},                                        // the ends of an index
    };
    for (const auto& c : cases) {
      e.set_batch_rows(Executor::kChoose);
      const std::string chosen = render(e.run_sql(c.sql));
      REQUIRE(e.last_batch_rows() == c.expect);
      e.set_batch_rows(0);
      REQUIRE(render(e.run_sql(c.sql)) == chosen);
      REQUIRE(e.last_batch_rows() == 0);
      e.set_batch_rows(5);  // a size set by hand is used for every plan
      REQUIRE(render(e.run_sql(c.sql)) == chosen);
      REQUIRE(e.last_batch_rows() == 5);
    }
    e.set_batch_rows(Executor::kChoose);
    std::printf("PASS choice: under %zu estimated rows a plan runs a row at a time, from %zu in batches of %zu; "
                "a size set by hand overrides it; the same rows every way\n", below, below, kDefaultBatchRows);
  });
  ::unlink(path);
  std::printf("u12_batch_test: PASS\n");
  return 0;
}
