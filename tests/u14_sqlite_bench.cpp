// Challenge 14.4: labdb against SQLite.
//
// SQLite is the database this book's engine most resembles -- one file of
// 4 KiB pages, B-trees, a write-ahead log of whole pages, one process at a
// time writing -- and one of the most carefully measured programs there is.
// Here the two do the same work in the same process: SQLite through its C
// API, set up as labdb runs -- its write-ahead log synced at every commit
// (journal_mode = WAL, synchronous = FULL), 4 KiB pages, a cache of 32 MiB,
// the size of labdb's pool, sorting in memory -- and given ANALYZE's
// statistics, as labdb keeps its own. Every statement reaches both as SQL
// text, parsed and planned each time; SQLite's prepared statements, which
// labdb has no equivalent of, are timed beside them for the point lookups.
// Every answer is compared: the two engines must agree, row for row.
//
//   1. the load: Unit 10's shop, 220,000 INSERTs in one transaction, then
//      its three indexes
//   2. commits: 2,000 INSERTs, one transaction each, and 100 to a transaction
//   3. point lookups: a customer by key, 20,000 times
//   4. the twelve questions of the sessions of Units 10 to 12, and the last
//      of them asked again with one aggregate instead of four
//   5. where the bytes are: every page of the two files, by what it holds
//
//   u14_sqlite_bench [--plans]
//
// --plans times nothing: it prints the plan each engine chose for each
// question of 4.
//
// Times are medians of rounds in which the engines take turns, in an order
// shuffled every round (tests/interleave.hpp), with the fastest and slowest.
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "interleave.hpp"
#include "pager.hpp"
#include "shop.hpp"
#include "slotted_page.hpp"

using namespace labdb;
using labdb::test::now_s;
using labdb::test::Timing;

namespace {

const char* kLabdbPath = "u14_bench_labdb.db";
const char* kLitePath = "u14_bench_sqlite.db";
const char* kLabdbCommits = "u14_commits_labdb.db";  // part 2's databases, beside the shop's
const char* kLiteCommits = "u14_commits_sqlite.db";
const char* kLiteCustomers = "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT, city TEXT)";
const char* kLiteOrders = "CREATE TABLE orders (oid INTEGER PRIMARY KEY, customer INTEGER, cents INTEGER, day INTEGER)";
// labdb's CREATE INDEX names no index; SQLite's must.
const char* kIndexes[][2] = {{"CREATE INDEX ON orders (customer)", "CREATE INDEX o_customer ON orders (customer)"},
                             {"CREATE INDEX ON orders (cents)", "CREATE INDEX o_cents ON orders (cents)"},
                             {"CREATE INDEX ON customers (city)", "CREATE INDEX c_city ON customers (city)"}};

void remove_db(const char* path) {
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) ::unlink((std::string(path) + suffix).c_str());
}

std::uint64_t file_bytes(const char* path) {
  struct stat st;
  return ::stat(path, &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
}

// The bytes this process has handed to write() and pwrite() so far: the
// "wchar" line of /proc/self/io.
std::uint64_t bytes_written() {
  std::ifstream io("/proc/self/io");
  std::string key;
  std::uint64_t v = 0;
  while (io >> key >> v)
    if (key == "wchar:") return v;
  return 0;
}

// ---------------------------------------------------------------- engines

struct Labdb {
  Pager pager;
  BufferPool pool;
  Catalog cat;
  Executor e;
  explicit Labdb(const char* path) : pager(path), pool(pager, 8192), cat(pool), e(cat) {}
};

class Lite {
 public:
  explicit Lite(const char* path) {
    REQUIRE(sqlite3_open(path, &db_) == SQLITE_OK);
    for (const char* p : {"PRAGMA page_size = 4096", "PRAGMA journal_mode = WAL", "PRAGMA synchronous = FULL",
                          "PRAGMA cache_size = -32768", "PRAGMA temp_store = MEMORY"})
      exec(p);
  }
  ~Lite() { sqlite3_close(db_); }
  Lite(const Lite&) = delete;
  Lite& operator=(const Lite&) = delete;
  sqlite3* db() { return db_; }

  // A statement whose rows, if it has any, are not wanted.
  void exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
      std::fprintf(stderr, "SQLite refused %s: %s\n", sql.c_str(), err ? err : "?");
      std::abort();
    }
  }
  // A query as text, as labdb takes one: prepared, stepped to its end and
  // finalized, every value of every row read into a Field -- what labdb's
  // ResultSet holds.
  std::vector<Row> query(const std::string& sql) {
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
    std::vector<Row> rows = drain(st);
    sqlite3_finalize(st);
    return rows;
  }
  static std::vector<Row> drain(sqlite3_stmt* st) {
    std::vector<Row> rows;
    const int n = sqlite3_column_count(st);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
      Row row;
      row.reserve(static_cast<std::size_t>(n));
      for (int c = 0; c < n; ++c) row.push_back(field(st, c));
      rows.push_back(std::move(row));
    }
    REQUIRE(rc == SQLITE_DONE);
    return rows;
  }
  static Field field(sqlite3_stmt* st, int c) {
    switch (sqlite3_column_type(st, c)) {
      case SQLITE_INTEGER: return Field::Int64(sqlite3_column_int64(st, c));
      case SQLITE_FLOAT: return Field::Double(sqlite3_column_double(st, c));
      case SQLITE_NULL: return Field::Null(ColType::kInt64);
      default:
        return Field::Text(std::string(reinterpret_cast<const char*>(sqlite3_column_text(st, c)),
                                       static_cast<std::size_t>(sqlite3_column_bytes(st, c))));
    }
  }

 private:
  sqlite3* db_ = nullptr;
};

// An answer as text, to compare: AVG, a double in both engines, to ten
// significant digits; rows sorted unless the query's ORDER BY fixes their order.
std::string answer(const std::vector<Row>& rows, bool ordered) {
  std::vector<std::string> lines;
  for (const Row& r : rows) {
    std::string l;
    for (const Field& f : r) {
      if (f.is_null) l += "NULL";
      else if (f.type == ColType::kText) l += f.text;
      else if (f.type == ColType::kDouble) {
        char b[64];
        std::snprintf(b, sizeof b, "%.10g", f.f64);
        l += b;
      } else l += std::to_string(f.i64);
      l += "|";
    }
    lines.push_back(l);
  }
  if (!ordered) std::sort(lines.begin(), lines.end());
  std::string all;
  for (const std::string& l : lines) all += l + "\n";
  return all;
}

std::string range(const Timing& t, double scale, const char* fmt) {
  char b[96];
  std::snprintf(b, sizeof b, fmt, t.median * scale, t.lo * scale, t.hi * scale);
  return b;
}

// ---------------------------------------------------------------- 1. the load

void load() {
  std::printf("1. The load: Unit 10's shop -- 20,000 customers, 200,000 orders -- as 220,000 INSERTs in one\n"
              "   transaction, into a new database; then its three indexes, each a statement of its own.\n"
              "   Median (fastest-slowest) of 7 rounds, after one that only warms:\n\n");
  std::vector<double> t_load[2], t_index[2];
  std::mt19937 rng(14);
  for (int round = 0; round <= 7; ++round) {
    int order[2] = {0, 1};
    std::shuffle(order, order + 2, rng);
    for (int w : order) {
      double t0, t1, t2;
      if (w == 0) {
        remove_db(kLabdbPath);
        Labdb l(kLabdbPath);
        t0 = now_s();
        labdb::test::build_shop(l.e);
        t1 = now_s();
        for (const auto& ix : kIndexes) l.e.run_sql(ix[0]);
        t2 = now_s();
      } else {
        remove_db(kLitePath);
        Lite s(kLitePath);
        t0 = now_s();
        labdb::test::shop_statements([&](const std::string& q) { s.exec(q); }, kLiteCustomers, kLiteOrders);
        t1 = now_s();
        for (const auto& ix : kIndexes) s.exec(ix[1]);
        t2 = now_s();
      }
      if (round > 0) {
        t_load[w].push_back(t1 - t0);
        t_index[w].push_back(t2 - t1);
      }
    }
  }
  const auto med = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return Timing{v[v.size() / 2], v.front(), v.back()};
  };
  const char* names[2] = {"labdb", "SQLite"};
  const char* paths[2] = {kLabdbPath, kLitePath};
  std::printf("  %-8s %-26s %-14s %-24s %s\n", "", "220,000 INSERTs", "per INSERT", "3 indexes", "database file");
  for (int w = 0; w < 2; ++w) {
    const Timing l = med(t_load[w]), x = med(t_index[w]);
    char per[32];
    std::snprintf(per, sizeof per, "%.2f us", l.median * 1e6 / 220000);
    std::printf("  %-8s %-26s %-14s %-24s %.1f MiB\n", names[w], range(l, 1e3, "%.0f ms (%.0f-%.0f)").c_str(), per,
                range(x, 1e3, "%.0f ms (%.0f-%.0f)").c_str(), file_bytes(paths[w]) / 1048576.0);
  }
}

// ---------------------------------------------------------------- 2. commits

void commits() {
  std::printf("\n2. Commits: 2,000 INSERTs of one payment each into a new table of a new database, every one its\n"
              "   own transaction -- acknowledged only once its commit is on disk -- and then 100 to a\n"
              "   transaction. Median (fastest-slowest) of 5 rounds, after one that only warms:\n\n");
  struct Run {
    int engine;  // 0 labdb, 1 SQLite
    int per_txn;
    std::vector<double> t;
    std::uint64_t bytes = 0;
  };
  std::vector<Run> runs = {{0, 1, {}}, {1, 1, {}}, {0, 100, {}}, {1, 100, {}}};
  const auto insert = [](int i) {
    return "INSERT INTO payments VALUES (" + std::to_string(i) + ", " + std::to_string(100 + (i * 7919) % 99900) +
           ", 'payment " + std::to_string(i) + "')";
  };
  std::mt19937 rng(1402);
  for (int round = 0; round <= 5; ++round) {
    std::vector<std::size_t> order(runs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::shuffle(order.begin(), order.end(), rng);
    for (std::size_t k : order) {
      Run& r = runs[k];
      const char* path = r.engine == 0 ? kLabdbCommits : kLiteCommits;
      remove_db(path);
      std::function<void(const std::string&)> run;
      Labdb* l = nullptr;
      Lite* s = nullptr;
      if (r.engine == 0) {
        l = new Labdb(path);
        run = [l](const std::string& q) { l->e.run_sql(q); };
        run("CREATE TABLE payments (id INT64 NOT NULL, cents INT64, note TEXT)");
      } else {
        s = new Lite(path);
        run = [s](const std::string& q) { s->exec(q); };
        run("CREATE TABLE payments (id INTEGER PRIMARY KEY, cents INTEGER, note TEXT)");
      }
      const std::uint64_t b0 = bytes_written();
      const double t0 = now_s();
      for (int i = 0; i < 2000; ++i) {
        if (r.per_txn > 1 && i % r.per_txn == 0) run("BEGIN");
        run(insert(i));
        if (r.per_txn > 1 && (i + 1) % r.per_txn == 0) run("COMMIT");
      }
      const double t1 = now_s();
      if (round > 0) {
        r.t.push_back(t1 - t0);
        r.bytes = bytes_written() - b0;
      }
      delete l;
      delete s;
    }
  }
  std::printf("  %-8s %-14s %-30s %-14s %s\n", "", "per txn", "per INSERT", "INSERTs/s", "bytes written per INSERT");
  for (Run& r : runs) {
    std::sort(r.t.begin(), r.t.end());
    const Timing t{r.t[r.t.size() / 2], r.t.front(), r.t.back()};
    std::printf("  %-8s %-14d %-30s %-14.0f %.0f\n", r.engine == 0 ? "labdb" : "SQLite", r.per_txn,
                range(t, 1e6 / 2000, "%.1f us (%.1f-%.1f)").c_str(), 2000 / t.median, r.bytes / 2000.0);
  }
}

// ---------------------------------------------------------------- 3. lookups

void lookups(Labdb& l, Lite& s) {
  std::printf("\n3. Point lookups: SELECT name, city FROM customers WHERE id = K, for 20,000 keys K drawn at\n"
              "   random -- every page in memory. labdb and SQLite each take the query as text; SQLite once more\n"
              "   with the statement prepared once and K bound to it. Median (fastest-slowest) of 5 rounds:\n\n");
  std::mt19937_64 rng(1403);
  std::vector<std::string> sql;
  std::vector<std::int64_t> keys;
  for (int i = 0; i < 20000; ++i) {
    keys.push_back(static_cast<std::int64_t>(rng() % 20000));
    sql.push_back("SELECT name, city FROM customers WHERE id = " + std::to_string(keys.back()));
  }
  sqlite3_stmt* prepared = nullptr;
  REQUIRE(sqlite3_prepare_v2(s.db(), "SELECT name, city FROM customers WHERE id = ?", -1, &prepared, nullptr) ==
          SQLITE_OK);
  std::string seen[3];
  std::size_t rows[3] = {0, 0, 0};
  const auto note = [&](int w, const std::vector<Row>& r) {
    rows[w] += r.size();
    if (seen[w].size() < (1u << 20)) seen[w] += answer(r, true);
  };
  const std::vector<std::function<void()>> fns = {
      [&] {
        for (const std::string& q : sql) note(0, l.e.run_sql(q).rows);
      },
      [&] {
        for (const std::string& q : sql) note(1, s.query(q));
      },
      [&] {
        for (std::int64_t k : keys) {
          sqlite3_bind_int64(prepared, 1, k);
          note(2, Lite::drain(prepared));
          sqlite3_reset(prepared);
        }
      }};
  const std::vector<Timing> t = labdb::test::interleaved(fns, 5);
  sqlite3_finalize(prepared);
  REQUIRE(seen[0] == seen[1] && seen[1] == seen[2] && rows[0] == rows[1] && rows[1] == rows[2]);
  const char* names[3] = {"labdb, as text", "SQLite, as text", "SQLite, prepared"};
  for (int w = 0; w < 3; ++w)
    std::printf("  %-18s %-28s %9.0f lookups/s\n", names[w], range(t[static_cast<std::size_t>(w)], 1e6 / 20000,
                                                                  "%.2f us (%.2f-%.2f)").c_str(),
                20000 / t[static_cast<std::size_t>(w)].median);
  std::printf("  (the same %zu rows from all three, compared)\n", rows[0] / 6);
}

// ---------------------------------------------------------------- 4. sessions

struct Question {
  const char* label;
  const char* sql;
  bool ordered;  // the ORDER BY fixes the order of every row (or there is one row)
};

const Question kQuestions[] = {
    {"1. the orders, summarized", "SELECT COUNT(*), SUM(cents), AVG(cents), MIN(cents), MAX(cents) FROM orders", true},
    {"2. the five biggest spenders",
     "SELECT c.name, COUNT(*) AS orders, SUM(o.cents) AS spent FROM orders o JOIN customers c ON o.customer = c.id "
     "GROUP BY c.id, c.name ORDER BY spent DESC LIMIT 5",
     true},
    {"3. revenue by city",
     "SELECT c.city, COUNT(*) AS orders, SUM(o.cents) AS revenue, AVG(o.cents) AS avg_order FROM orders o "
     "JOIN customers c ON o.customer = c.id GROUP BY c.city ORDER BY revenue DESC",
     true},
    {"4. the three busiest days", "SELECT day, COUNT(*) AS n FROM orders GROUP BY day ORDER BY n DESC, day LIMIT 3",
     true},
    {"5. customers with 250 orders or more",
     "SELECT customer, COUNT(*) AS n FROM orders GROUP BY customer HAVING COUNT(*) >= 250 ORDER BY n DESC", false},
    {"6. one customer's orders",
     "SELECT o.oid, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE c.id = 7 "
     "ORDER BY o.cents DESC LIMIT 3",
     true},
    {"7. whoever placed one order",
     "SELECT c.name, c.city, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.oid = 123456", true},
    {"8. a small city's revenue",
     "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id WHERE c.city = 'Zagreb'",
     true},
    {"9. the largest city's revenue",
     "SELECT COUNT(*), SUM(o.cents) FROM orders o JOIN customers c ON o.customer = c.id WHERE c.city = 'Cairo'", true},
    {"10. the biggest orders, and their buyers",
     "SELECT c.name, o.cents FROM orders o JOIN customers c ON o.customer = c.id WHERE o.cents > 99000", false},
    {"11. all orders of one order's customer",
     "SELECT COUNT(*) FROM orders a JOIN orders b ON a.customer = b.customer WHERE a.oid = 4242", true},
    {"12. the extremes", "SELECT MIN(oid), MAX(oid), MIN(cents), MAX(cents) FROM orders", true},
    {"12'. one extreme, alone", "SELECT MAX(cents) FROM orders", true},
};

void sessions(Labdb& l, Lite& s) {
  std::printf("\n4. The questions of the sessions of Units 10 to 12, over the shop and its indexes, each as\n"
              "   SQL text -- and the last asked again, of one extreme alone. Median (fastest-slowest) of 7\n"
              "   rounds, after one that only warms:\n\n");
  std::printf("  %-40s %-26s %-26s %s\n", "question", "labdb, ms", "SQLite, ms", "SQLite's time / labdb's");
  std::vector<double> ratio;
  for (const Question& q : kQuestions) {
    std::string got[2];
    const std::vector<std::function<void()>> fns = {
        [&] {
          const std::string a = answer(l.e.run_sql(q.sql).rows, q.ordered);
          if (got[0].empty()) got[0] = a;
          REQUIRE(a == got[0]);
        },
        [&] {
          const std::string a = answer(s.query(q.sql), q.ordered);
          if (got[1].empty()) got[1] = a;
          REQUIRE(a == got[1]);
        }};
    const std::vector<Timing> t = labdb::test::interleaved(fns, 7);
    if (got[0] != got[1]) {
      std::printf("  %s: THE ANSWERS DIFFER\n  labdb:\n%s  SQLite:\n%s", q.label, got[0].c_str(), got[1].c_str());
      std::exit(1);
    }
    const char* fmt = t[0].median < 1e-3 ? "%.3f (%.3f-%.3f)" : "%.2f (%.2f-%.2f)";
    const char* fmt1 = t[1].median < 1e-3 ? "%.3f (%.3f-%.3f)" : "%.2f (%.2f-%.2f)";
    ratio.push_back(t[1].median / t[0].median);
    std::printf("  %-40s %-26s %-26s %6.2f\n", q.label, range(t[0], 1e3, fmt).c_str(), range(t[1], 1e3, fmt1).c_str(),
                ratio.back());
  }
  double g = 0;
  for (double r : ratio) g += std::log(r);
  std::printf("  (the same answers from both, compared; geometric mean of the ratios %.2f)\n",
              std::exp(g / static_cast<double>(ratio.size())));
}

// ---------------------------------------------------------------- 5. the files

struct Tally {
  std::uint64_t pages = 0, used = 0;
};

void print_tally(const std::string& what, const Tally& t) {
  std::printf("    %-40s %7llu %8.1f %8.0f%%\n", what.c_str(), static_cast<unsigned long long>(t.pages),
              t.pages * 4096.0 / 1048576.0, t.pages ? 100.0 * static_cast<double>(t.used) / (t.pages * 4096.0) : 0.0);
}

void census() {
  std::printf("\n5. Where the bytes are: every page of the two shops of 1, by what it holds, and how much of\n"
              "   its 4,096 bytes is in use.\n\n");
  std::printf("    %-40s %7s %8s %9s\n", "", "pages", "MiB", "in use");
  std::map<std::string, Tally> mine;
  std::uint64_t heap_used = 0;
  {
    Pager pager(kLabdbPath);
    BufferPool pool(pager, 8192);
    Catalog cat(pool);
    std::vector<bool> own(pager.page_count());  // a page of a table's own tree, not of a secondary index
    std::function<void(PageId)> walk = [&](PageId id) {
      own[id] = true;
      Page p;
      pool.read_page(id, p);
      if (p.type() != PageType::kBTreeInternal) return;
      InternalNode n(p);
      for (std::uint16_t i = 0; i <= n.count(); ++i) walk(n.child_at(i));
    };
    for (const std::string& t : cat.table_names()) walk(cat.find_table(t)->btree_root);
    mine["meta, catalog and free pages"].pages = 1;  // page 0, the meta page, which only the pager reads
    for (PageId id = 1; id < pager.page_count(); ++id) {
      Page p;
      pool.read_page(id, p);
      std::string what = "meta, catalog and free pages";
      std::uint64_t used = 0;
      if (p.type() == PageType::kSlotted) {
        what = "heap pages: the rows";
        used = kPageSize - SlottedPage(p).total_free();
        heap_used += used;
      } else if (p.type() == PageType::kBTreeLeaf) {
        what = own[id] ? "the tables' own trees: leaves" : "the three secondary indexes: leaves";
        used = kPageHeaderSize + std::uint64_t{LeafNode(p).count()} * kLeafEntrySize;
      } else if (p.type() == PageType::kBTreeInternal) {
        what = own[id] ? "the tables' own trees: inner nodes" : "the three secondary indexes: inner nodes";
        const std::uint64_t k = InternalNode(p).count();
        used = kPageHeaderSize + k * 8 + (k + 1) * 4;
      }
      mine[what].pages += 1;
      mine[what].used += used;
    }
  }
  std::printf("  labdb, %.1f MiB:\n", file_bytes(kLabdbPath) / 1048576.0);
  for (const char* k : {"heap pages: the rows", "the tables' own trees: leaves", "the tables' own trees: inner nodes",
                        "the three secondary indexes: leaves", "the three secondary indexes: inner nodes",
                        "meta, catalog and free pages"})
    print_tally(k, mine[k]);
  std::map<std::string, Tally> theirs;
  std::uint64_t table_used = 0;
  {
    Lite s(kLitePath);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(s.db(), "SELECT name, COUNT(*), SUM(pgsize - unused) FROM dbstat GROUP BY name", -1,
                               &st, nullptr) == SQLITE_OK);
    while (sqlite3_step(st) == SQLITE_ROW) {
      const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
      const std::string what = name == "orders" || name == "customers" ? "the tables' trees, rows inside"
                               : name.rfind("sqlite_", 0) == 0          ? "SQLite's own tables"
                                                                        : "the three secondary indexes";
      theirs[what].pages += static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
      theirs[what].used += static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
      if (what == "the tables' trees, rows inside") table_used += static_cast<std::uint64_t>(sqlite3_column_int64(st, 2));
    }
    sqlite3_finalize(st);
  }
  std::printf("  SQLite, %.1f MiB:\n", file_bytes(kLitePath) / 1048576.0);
  for (const char* k : {"the tables' trees, rows inside", "the three secondary indexes", "SQLite's own tables"})
    print_tally(k, theirs[k]);
  std::printf("\n  bytes in use per row, the 220,000 rows: labdb's heap %.1f; SQLite's tables %.1f, keys and all\n",
              static_cast<double>(heap_used) / 220000, static_cast<double>(table_used) / 220000);
}

}  // namespace

// --plans: no timing -- the shop built once in each engine, and each question
// with the plan each engine chose for it: labdb's explain_last(), SQLite's
// EXPLAIN QUERY PLAN.
int plans() {
  remove_db(kLabdbPath);
  remove_db(kLitePath);
  {
    Labdb l(kLabdbPath);
    Lite s(kLitePath);
    labdb::test::build_shop(l.e);
    labdb::test::shop_statements([&](const std::string& q) { s.exec(q); }, kLiteCustomers, kLiteOrders);
    for (const auto& ix : kIndexes) {
      l.e.run_sql(ix[0]);
      s.exec(ix[1]);
    }
    s.exec("ANALYZE");
    for (const Question& q : kQuestions) {
      std::printf("%s\n  SQL> %s\n  labdb:\n", q.label, q.sql);
      l.e.run_sql(q.sql);
      std::string text = l.e.explain_last(false), line;
      for (char c : text) {
        if (c != '\n') {
          line += c;
          continue;
        }
        std::printf("    %s\n", line.c_str());
        line.clear();
      }
      std::printf("  SQLite:\n");
      for (const Row& r : s.query(std::string("EXPLAIN QUERY PLAN ") + q.sql))
        std::printf("    %s\n", r[3].text.c_str());
      std::printf("\n");
    }
  }
  remove_db(kLabdbPath);
  remove_db(kLitePath);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--plans") return plans();
  std::printf("labdb against SQLite %s, in one process. SQLite: journal_mode = WAL, synchronous = FULL,\n"
              "4 KiB pages, a 32 MiB cache, temp_store = MEMORY. labdb: durability kFull, a pool of 8,192 frames.\n\n",
              sqlite3_libversion());
  load();
  census();
  commits();
  {
    Labdb l(kLabdbPath);  // the shop of the last round of 1, with its indexes
    Lite s(kLitePath);
    s.exec("ANALYZE");
    lookups(l, s);
    sessions(l, s);
  }
  for (const char* p : {kLabdbPath, kLitePath, kLabdbCommits, kLiteCommits}) remove_db(p);
  return 0;
}
