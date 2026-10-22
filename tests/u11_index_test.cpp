// Unit 11 correctness: indexes, and a planner that chooses.
//
// The heart of it is a differential test across plans. A planner is allowed
// to choose only among plans that return the same rows, so the test runs
// every query under seven configurations -- Unit 10's literal plans, every
// table scanned with each join algorithm forced, indexes forced, and the
// planner's own choice -- and every one of them must return exactly the rows
// of an answer computed by hand from plain C++ vectors, with SQL's NULL rules
// written out, sharing no code with the engine.
//
// Around it: the non-unique B+Tree on its own (runs of equal keys that
// straddle leaves), the TEXT key's 8-byte prefix and the recheck it needs,
// NULLs (not in any index), MIN and MAX from the ends of the indexes, ORDER BY
// the key without a sort, a duplicate key refused, the grammar and its
// errors, the plans the planner picks for queries whose best plan is not in
// doubt, and all of it again after the database is closed and reopened.
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "parser.hpp"
#include "unparse.hpp"

using namespace labdb;

namespace {

using Algo = Executor::JoinAlgorithm;
using Access = Executor::AccessPath;

void check_error_at(const std::string& sql, std::size_t line, std::size_t col) {
  try {
    Parser p(sql);
    p.parse_single();
  } catch (const ParseError& e) {
    if (e.line != line || e.col != col)
      std::fprintf(stderr, "error for [%s] at %zu:%zu, expected %zu:%zu (%s)\n", sql.c_str(),
                   e.line, e.col, line, col, e.message.c_str());
    REQUIRE(e.line == line && e.col == col);
    return;
  }
  REQUIRE(false);
}

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

std::string render(const Field& f) {
  if (f.is_null) return "NULL";
  if (f.type == ColType::kText) return "'" + f.text + "'";
  return std::to_string(f.i64);
}

using Table = std::vector<std::vector<std::string>>;

Table rows_of(const ResultSet& r) {
  Table t;
  for (const auto& row : r.rows) {
    t.emplace_back();
    for (const Field& f : row) t.back().push_back(render(f));
  }
  return t;
}

void require_same(Table got, Table want, const std::string& what, bool ordered) {
  if (!ordered) {
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
  }
  if (got != want) {
    std::fprintf(stderr, "MISMATCH for %s: %zu rows, expected %zu\n", what.c_str(), got.size(),
                 want.size());
    for (std::size_t i = 0; i < std::max(got.size(), want.size()) && i < 10; ++i) {
      std::string g, w;
      if (i < got.size()) for (const auto& c : got[i]) g += c + " ";
      if (i < want.size()) for (const auto& c : want[i]) w += c + " ";
      std::fprintf(stderr, "  got [%s]  want [%s]\n", g.c_str(), w.c_str());
    }
  }
  REQUIRE(got == want);
}

// ------------------------------------------------- the reference, by hand

// A value as the reference sees it: absent (NULL), an integer, or a string.
struct Val {
  bool null = true;
  std::int64_t i = 0;
  std::string s;
  bool text = false;
};
Val vnull() { return {}; }
Val vint(std::int64_t x) { Val v; v.null = false; v.i = x; return v; }
Val vtext(std::string x) { Val v; v.null = false; v.s = std::move(x); v.text = true; return v; }

// A cell as rows_of renders the engine's: what the reference's rows hold.
std::string cell_of(const Val& v) {
  if (v.null) return "NULL";
  if (v.text) return "'" + v.s + "'";
  return std::to_string(v.i);
}

std::string sql_of(const Val& v, bool as_bool = false) {
  if (v.null) return "NULL";
  if (v.text) {
    std::string out = "'";
    for (char c : v.s) { if (c == '\'') out += '\''; out += c; }
    return out + "'";
  }
  if (as_bool) return v.i ? "TRUE" : "FALSE";
  return std::to_string(v.i);
}

// SQL's comparison: NULL on either side is never true.
bool holds(const Val& x, const std::string& op, const Val& y) {
  if (x.null || y.null) return false;
  const int c = x.text ? (x.s < y.s ? -1 : x.s > y.s) : (x.i < y.i ? -1 : x.i > y.i);
  if (op == "=") return c == 0;
  if (op == "<>") return c != 0;
  if (op == "<") return c < 0;
  if (op == "<=") return c <= 0;
  if (op == ">") return c > 0;
  return c >= 0;
}

// t (id, a INT32, b INT64, s TEXT, f BOOL) and u (id, t_id, tag TEXT, w INT32)
const char* kTCols[] = {"id", "a", "b", "s", "f"};
const char* kUCols[] = {"id", "t_id", "tag", "w"};
using GenRow = std::vector<Val>;

struct Term {
  int col;
  std::string op;
  Val lit;
  bool decimal = false;  // the literal is written as a decimal, e.g. 2.5 (compared, never looked up)
  double dec = 0;
};

bool term_holds(const GenRow& r, const Term& t) {
  const Val& x = r[static_cast<std::size_t>(t.col)];
  if (t.decimal) {
    if (x.null) return false;
    const double a = static_cast<double>(x.i), b = t.dec;
    const int c = a < b ? -1 : a > b;
    if (t.op == "=") return c == 0;
    if (t.op == "<>") return c != 0;
    if (t.op == "<") return c < 0;
    if (t.op == "<=") return c <= 0;
    if (t.op == ">") return c > 0;
    return c >= 0;
  }
  return holds(x, t.op, t.lit);
}

std::string term_sql(const std::string& label, const char* const* cols, const Term& t, bool is_bool) {
  std::string lit;
  if (t.decimal) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", t.dec);
    lit = buf;
  } else {
    lit = sql_of(t.lit, is_bool);
  }
  return label + cols[t.col] + " " + t.op + " " + lit;
}

// --------------------------------------------------- the configurations

struct Config {
  const char* name;
  bool literal;
  Access access;
  Algo join;
};
const Config kConfigs[] = {
    {"Unit 10's literal plans", true, Access::kAuto, Algo::kAuto},
    {"scan + nested loop", false, Access::kScan, Algo::kNestedLoop},
    {"scan + hash (build right)", false, Access::kScan, Algo::kHash},
    {"scan + hash (build left)", false, Access::kScan, Algo::kHashLeft},
    {"index + index nested loop", false, Access::kIndex, Algo::kIndexNestedLoop},
    {"index + chosen join", false, Access::kIndex, Algo::kAuto},
    {"the planner's choice", false, Access::kAuto, Algo::kAuto},
};

// How many runs used each operator: the differential test is only as good as
// the plans it reaches.
std::map<std::string, std::size_t> g_used;

void tally(const std::string& plan) {
  std::vector<std::string> seen;
  std::size_t at = 0;
  while (at < plan.size()) {
    std::size_t end = plan.find('\n', at);
    if (end == std::string::npos) end = plan.size();
    const std::string line = plan.substr(at, end - at);
    at = end + 1;
    const std::size_t b = line.find_first_not_of(' ');
    if (b == std::string::npos) continue;
    std::string word = line.substr(b, line.find(' ', b) - b);
    if (word == "hash") word = line.find("(left") != std::string::npos ? "HashJoin(build left)" : "HashJoin(build right)";
    else if (word == "HashJoin" || word == "rows" || word == "index" || word == "one" ||
             std::isdigit(static_cast<unsigned char>(word[0])))
      continue;
    if (std::find(seen.begin(), seen.end(), word) == seen.end()) seen.push_back(word);
  }
  for (const std::string& w : seen) ++g_used[w];
}

void configure(Executor& e, const Config& c) {
  e.set_literal_planning(c.literal);
  e.set_access_path(c.access);
  e.set_join_algorithm(c.join);
}

// Run `sql` under every configuration; each must equal `want`. Returns how
// many runs were made (a forced index nested loop is skipped when no index can
// drive the join).
std::size_t agree(Executor& e, const std::string& sql, const Table& want, bool ordered) {
  std::size_t runs = 0;
  for (const Config& c : kConfigs) {
    configure(e, c);
    ResultSet r;
    try {
      r = e.run_sql(sql);
    } catch (const ExecError& x) {
      if (c.join == Algo::kIndexNestedLoop && x.message.find("no index to search") != std::string::npos)
        continue;
      std::fprintf(stderr, "[%s] under %s: %s\n", sql.c_str(), c.name, x.message.c_str());
      REQUIRE(false);
    }
    require_same(rows_of(r), want, std::string(c.name) + ": " + sql, ordered);
    tally(e.explain_last());
    ++runs;
  }
  configure(e, kConfigs[6]);
  return runs;
}

void open_all(const char* path, const std::function<void(Catalog&, Executor&)>& body) {
  Pager pager(path);
  BufferPool pool(pager, 512);
  Catalog cat(pool);
  Executor e(cat);
  body(cat, e);
}

}  // namespace

int main() {
  // ------------------------------------------------- the non-unique tree
  {
    const char* path = "u11_index_test_tree.db";
    ::unlink(path);
    Pager pager(path);
    BufferPool pool(pager, 256);
    BTree tree(pool, kNullPage, nullptr, /*unique=*/false);
    // 60 keys; key k appears k * 17 times, so the longest runs span several
    // leaves (a leaf holds 255 entries); inserted in a shuffled order, each
    // entry's value its insertion number.
    std::vector<std::pair<Key, Value>> entries;
    for (Key k = 0; k < 60; ++k)
      for (Key j = 0; j < k * 17; ++j) entries.push_back({k * 1000, 0});
    std::mt19937_64 rng(1101);
    std::shuffle(entries.begin(), entries.end(), rng);
    std::map<Key, std::vector<Value>> order;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      REQUIRE(tree.insert(entries[i].first, i));
      order[entries[i].first].push_back(i);
    }
    REQUIRE(tree.height() >= 2);
    std::size_t straddling = 0;
    for (Key k = 0; k < 60; ++k) {
      std::vector<Value> got;
      for (Cursor c = tree.seek(k * 1000); c.valid() && c.key() == k * 1000; c.next())
        got.push_back(c.value());
      REQUIRE(got == order[k * 1000]);  // every entry with the key, in insertion order
      // (a seek between keys lands on the next key)
      const Cursor mid = tree.seek(k * 1000 + 1);
      REQUIRE(k == 59 ? !mid.valid() : mid.valid() && mid.key() == (k + 1) * 1000);
      // how many runs straddle a separator, entries equal to it on both sides:
      // there, Unit 2's descent rule starts the walk too far right
      Cursor right = tree.seek(k * 1000, rightmost_child);
      std::size_t found = 0;
      for (; right.valid() && right.key() == k * 1000; right.next()) ++found;
      straddling += found < got.size();
    }
    const Cursor last = tree.last();
    REQUIRE(last.valid() && last.key() == 59 * 1000 && last.value() == order[59000].back());
    REQUIRE(straddling > 0);  // the data really has runs across separators
    BTree empty(pool, kNullPage, nullptr, false);
    REQUIRE(!empty.last().valid() && !empty.seek(0).valid());
    std::printf("PASS non-unique tree: %zu entries, 59 runs up to 1003 long, all found in insertion "
                "order (%zu straddle a separator)\n",
                entries.size(), straddling);
    ::unlink(path);
  }

  const char* path = "u11_index_test.db";
  ::unlink(path);

  // ------------------------------------------------------- the data
  std::mt19937_64 rng(1111);
  const std::vector<std::string> words = {"name_1",      "name_12",   "name_123", "name_1234",
                                          "name_12345",  "name_123456", "name_1235", "alpha",
                                          "alphabetic",  "alphabetical", "",        "zz",
                                          "O'Brien",     "name_2"};
  const auto pick_word = [&] { return words[rng() % words.size()]; };
  std::vector<GenRow> T, U;
  std::vector<std::int64_t> ids(3000);
  for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<std::int64_t>(i) * 3;
  std::shuffle(ids.begin(), ids.end(), rng);  // inserted out of key order
  for (std::int64_t id : ids) {
    GenRow r;
    r.push_back(vint(id));
    r.push_back(rng() % 10 == 0 ? vnull() : vint(static_cast<std::int64_t>(rng() % 10)));
    r.push_back(rng() % 20 == 0 ? vnull() : vint(static_cast<std::int64_t>(rng() % 1000000)));
    r.push_back(rng() % 10 == 0 ? vnull() : vtext(pick_word()));
    r.push_back(rng() % 8 == 0 ? vnull() : vint(static_cast<std::int64_t>(rng() % 2)));
    T.push_back(r);
  }
  for (std::int64_t i = 0; i < 4000; ++i) {
    GenRow r;
    r.push_back(vint(i));
    r.push_back(rng() % 15 == 0 ? vnull() : vint(static_cast<std::int64_t>(rng() % 3300) * 3));
    r.push_back(rng() % 10 == 0 ? vnull() : vtext(pick_word()));
    r.push_back(vint(static_cast<std::int64_t>(rng() % 50)));
    U.push_back(r);
  }
  const auto insert_t = [&](Executor& e, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) {
      const GenRow& r = T[i];
      e.run_sql("INSERT INTO t VALUES (" + sql_of(r[0]) + ", " + sql_of(r[1]) + ", " + sql_of(r[2]) +
                ", " + sql_of(r[3]) + ", " + sql_of(r[4], true) + ")");
    }
  };

  // ------------------------------------------- grammar, errors, building
  {
    Parser p("create index on orders (customer)");
    const Statement s = p.parse_single();
    REQUIRE(s.kind == Statement::Kind::kCreateIndex && s.create_index.table == "orders" &&
            s.create_index.column == "customer");
    REQUIRE(unparse(s) == "CREATE INDEX ON orders (customer)");
    check_error_at("CREATE INDEX idx ON t (a)", 1, 14);   // an index has no name
    check_error_at("CREATE INDEX ON t (a, b)", 1, 21);    // one column
    check_error_at("CREATE INDEX ON t", 1, 18);           // no column
    check_error_at("CREATE INDEXES ON t (a)", 1, 8);      // TABLE or INDEX
    std::printf("PASS grammar: CREATE INDEX ON t (c), round-tripped; 4 errors caret-located\n");
  }
  open_all(path, [&](Catalog& cat, Executor& e) {
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, a INT32, b INT64, s TEXT, f BOOL)");
    e.run_sql("CREATE TABLE u (id INT64 NOT NULL, t_id INT64, tag TEXT, w INT32)");
    insert_t(e, 0, 1500);
    e.run_sql("CREATE INDEX ON t (a)");  // built over 1,500 rows, then kept up by 1,500 inserts
    e.run_sql("CREATE INDEX ON t (s)");
    insert_t(e, 1500, T.size());
    e.run_sql("CREATE INDEX ON t (b)");  // built over all 3,000
    e.run_sql("CREATE INDEX ON t (f)");
    e.run_sql("CREATE INDEX ON u (t_id)");
    for (const GenRow& r : U)
      e.run_sql("INSERT INTO u VALUES (" + sql_of(r[0]) + ", " + sql_of(r[1]) + ", " + sql_of(r[2]) +
                ", " + sql_of(r[3]) + ")");
    expect_exec_error(e, "CREATE INDEX ON nope (a)", "no such table: nope");
    expect_exec_error(e, "CREATE INDEX ON t (zz)", "no such column: t.zz");
    expect_exec_error(e, "CREATE INDEX ON t (id)", "is the key of t");
    expect_exec_error(e, "CREATE INDEX ON t (a)", "already has an index");
    expect_exec_error(e, "INSERT INTO t VALUES (3, 1, 1, 'x', TRUE)", "duplicate key: t already has a row with id = 3");
    const TableInfo info = *cat.find_table("t");
    REQUIRE(info.rows == T.size());  // the refused row was not counted
    REQUIRE(info.indexed == (std::vector<bool>{true, true, true, true, true}));
    REQUIRE(!cat.find_table("u")->indexed[2]);
    std::size_t with_a = 0;
    for (const GenRow& r : T) with_a += !r[1].null;
    REQUIRE(cat.count_index("t", 1, 0, ~Key{0}, ~std::uint64_t{0}) == with_a);  // no NULL is filed
    std::printf("PASS building: indexes built over existing rows and kept up by inserts; 5 errors; "
                "a duplicate key refused, uncounted; %zu of %zu values of a filed (no NULLs)\n",
                with_a, T.size());
  });

  // -------------------------------------------- the differential test
  const auto random_term = [&](int col) {
    Term t;
    t.col = col;
    const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
    t.op = ops[rng() % 6];
    if (rng() % 25 == 0) {  // NULL: never true
      t.lit = vnull();
      return t;
    }
    switch (col) {
      case 0: {
        const std::int64_t edges[] = {0, 3, 8997, 9000, 9223372036854775807LL};
        t.lit = vint(rng() % 6 == 0 ? edges[rng() % 5] : static_cast<std::int64_t>(rng() % 9100));
        break;
      }
      case 1: {
        if (rng() % 10 == 0) { t.decimal = true; t.dec = static_cast<double>(rng() % 10) + 0.5; break; }
        const std::int64_t edges[] = {0, 9, 2147483647LL, 3000000000LL};
        t.lit = vint(rng() % 6 == 0 ? edges[rng() % 4] : static_cast<std::int64_t>(rng() % 11));
        break;
      }
      case 2: {
        const GenRow& r = T[rng() % T.size()];
        t.lit = !r[2].null && rng() % 2 ? r[2] : vint(static_cast<std::int64_t>(rng() % 1000000));
        break;
      }
      case 3: {
        const std::vector<std::string> probes = {"name_12", "name_13", "alph", "alphabet", "", "zzz", "name_1234"};
        t.lit = vtext(rng() % 3 == 0 ? probes[rng() % probes.size()] : pick_word());
        break;
      }
      default:
        t.lit = vint(static_cast<std::int64_t>(rng() % 2));
        break;
    }
    return t;
  };

  std::size_t queries = 0, runs = 0;
  open_all(path, [&](Catalog&, Executor& e) {
    // one table: one or two terms, on any column
    for (int q = 0; q < 220; ++q) {
      std::vector<Term> terms;
      const int n = 1 + static_cast<int>(rng() % 3);
      const int col = static_cast<int>(rng() % 5);
      for (int k = 0; k < n; ++k) terms.push_back(random_term(k == 0 || rng() % 2 ? col : static_cast<int>(rng() % 5)));
      std::string where;
      for (const Term& t : terms) where += (where.empty() ? "" : " AND ") + term_sql("", kTCols, t, t.col == 4);
      Table want;
      for (const GenRow& r : T)
        if (std::all_of(terms.begin(), terms.end(), [&](const Term& t) { return term_holds(r, t); }))
          want.push_back({cell_of(r[0]), cell_of(r[1]), cell_of(r[3])});
      runs += agree(e, "SELECT id, a, s FROM t WHERE " + where, want, false);
      ++queries;
    }
    // ORDER BY the key, with a LIMIT: the exact rows, in order
    for (int q = 0; q < 40; ++q) {
      const Term t = random_term(static_cast<int>(rng() % 5));
      const std::size_t limit = rng() % 3 == 0 ? 5000 : rng() % 40;
      std::vector<std::int64_t> keep;
      for (const GenRow& r : T)
        if (term_holds(r, t)) keep.push_back(r[0].i);
      std::sort(keep.begin(), keep.end());
      Table want;
      for (std::size_t i = 0; i < keep.size() && i < limit; ++i) want.push_back({std::to_string(keep[i])});
      runs += agree(e, "SELECT id FROM t WHERE " + term_sql("", kTCols, t, t.col == 4) +
                           " ORDER BY id LIMIT " + std::to_string(limit), want, true);
      ++queries;
    }
    // joins, written either way round, with terms on either table
    for (int q = 0; q < 60; ++q) {
      const Term tt = random_term(static_cast<int>(rng() % 5));
      Term ut;
      ut.col = 3;
      const char* ops[] = {"<", ">=", "="};
      ut.op = ops[rng() % 3];
      ut.lit = vint(static_cast<std::int64_t>(rng() % 50));
      const bool t_first = rng() % 2;
      const bool with_u = rng() % 2;
      std::string sql = t_first ? "SELECT t.id, u.id FROM t JOIN u ON u.t_id = t.id"
                                : "SELECT t.id, u.id FROM u JOIN t ON t.id = u.t_id";
      sql += " WHERE " + term_sql("t.", kTCols, tt, tt.col == 4);
      if (with_u) sql += " AND " + term_sql("u.", kUCols, ut, false);
      Table want;
      for (const GenRow& a : T) {
        if (!term_holds(a, tt)) continue;
        for (const GenRow& b : U)
          if (holds(b[1], "=", a[0]) && (!with_u || holds(b[3], ut.op, ut.lit)))
            want.push_back({cell_of(a[0]), cell_of(b[0])});
      }
      runs += agree(e, sql, want, false);
      ++queries;
    }
    // a three-way join, and one through a TEXT key
    {
      Table want;
      for (const GenRow& b : T) {
        if (!holds(b[1], "=", vint(3))) continue;
        std::vector<const GenRow*> xs;
        for (const GenRow& a : U)
          if (holds(a[1], "=", b[0])) xs.push_back(&a);
        for (const GenRow* x : xs)
          for (const GenRow* y : xs) want.push_back({cell_of((*x)[0]), cell_of((*y)[0])});
      }
      runs += agree(e, "SELECT x.id, y.id FROM u x JOIN t ON t.id = x.t_id JOIN u y ON y.t_id = t.id WHERE t.a = 3",
                    want, false);
      Table want2;
      for (const GenRow& a : U)
        for (const GenRow& b : T)
          if (holds(a[2], "=", b[3]) && a[3].i == 7 && holds(b[1], "<", vint(2)))
            want2.push_back({cell_of(a[0]), cell_of(b[0])});
      runs += agree(e, "SELECT u.id, t.id FROM u JOIN t ON t.s = u.tag WHERE u.w = 7 AND t.a < 2",
                    want2, false);
      queries += 2;
    }
    // MIN and MAX of every column
    for (int c = 0; c < 5; ++c) {
      Val lo, hi;
      for (const GenRow& r : T) {
        const Val& v = r[static_cast<std::size_t>(c)];
        if (v.null) continue;
        if (lo.null || holds(v, "<", lo)) lo = v;
        if (hi.null || holds(v, ">", hi)) hi = v;
      }
      const std::string col = kTCols[c];
      const Table want = {{cell_of(lo), cell_of(hi)}};
      runs += agree(e, "SELECT MIN(" + col + "), MAX(" + col + ") FROM t", want, true);
      ++queries;
    }
  });
  std::printf("PASS differential: %zu queries x 7 plans (%zu runs), every plan = an answer computed by hand\n",
              queries, runs);
  std::printf("     runs using each operator:");
  std::size_t k = 0;
  for (const char* op : {"SeqScan", "Filter", "IndexLookup", "IndexScan", "IndexMinMax", "NestedLoopJoin",
                         "HashJoin(build right)", "HashJoin(build left)", "IndexNestedLoopJoin", "Sort",
                         "Limit", "HashAggregate"})
    std::printf("%s %s %zu", k++ % 4 == 0 ? "\n      " : ",", op, g_used[op]);
  std::printf("\n");

  // ------------------------------------------------------- the plans
  open_all(path, [&](Catalog&, Executor& e) {
    const auto plan_of = [&](const std::string& sql) {
      e.run_sql(sql);
      return e.explain_last();
    };
    const auto has = [](const std::string& plan, const std::string& part) {
      if (plan.find(part) == std::string::npos) {
        std::fprintf(stderr, "plan lacks [%s]:\n%s", part.c_str(), plan.c_str());
        REQUIRE(false);
      }
    };
    has(plan_of("SELECT a FROM t WHERE id = 300"), "IndexLookup t key = 300");
    has(plan_of("SELECT a FROM t WHERE id >= 300 AND id < 330"), "IndexScan t (id): id >= 300 AND id < 330");
    has(plan_of("SELECT a FROM t WHERE id > 10 AND id < 5"), "rows=0");
    has(plan_of("SELECT id FROM t WHERE b = 123456"), "IndexScan t (b): b = 123456");
    const std::string text = plan_of("SELECT id FROM t WHERE s = 'name_123'");
    has(text, "Filter s = 'name_123'");  // a TEXT key is a prefix: checked again
    has(text, "IndexScan t (s): s = 'name_123'");
    has(plan_of("SELECT id FROM t WHERE b >= 0"), "SeqScan t");  // every b: a scan is cheaper
    has(plan_of("SELECT id FROM t WHERE f = TRUE AND a = 7"), "IndexScan t (a): a = 7");
    has(plan_of("SELECT id FROM t ORDER BY id LIMIT 3"), "Limit 3");
    REQUIRE(plan_of("SELECT id FROM t ORDER BY id LIMIT 3").find("Sort") == std::string::npos);
    has(plan_of("SELECT id FROM t ORDER BY id DESC LIMIT 3"), "Sort id DESC (top 3)");
    has(plan_of("SELECT MIN(b), MAX(s) FROM t"), "IndexMinMax MIN(b), MAX(s)");
    has(plan_of("SELECT MIN(tag) FROM u"), "HashAggregate MIN(tag)");  // no index on tag
    has(plan_of("SELECT t.id, u.id FROM t JOIN u ON u.t_id = t.id WHERE t.id = 300"),
        "IndexNestedLoopJoin ON u.t_id = t.id (index u(t_id))");
    has(plan_of("SELECT t.id, u.id FROM u JOIN t ON t.id = u.t_id WHERE u.w = 7"),
        "IndexNestedLoopJoin ON t.id = u.t_id (index t(id))");
    const std::string hash = plan_of("SELECT t.id, u.id FROM t JOIN u ON u.tag = t.s");
    has(hash, "HashJoin ON u.tag = t.s");
    const auto with_s = std::count_if(T.begin(), T.end(), [](const GenRow& r) { return !r[3].null; });
    has(hash, "hash table (left input): " + std::to_string(with_s) + " rows");  // t, the smaller side
    std::printf("PASS plans: point, range and empty key lookups; secondary index or scan by cost; ORDER BY "
                "the key unsorted; MIN/MAX from the index ends; index and hash joins, smaller side built\n");
  });

  // ------------------------------------------------ negative keys, reopened
  open_all(path, [&](Catalog& cat, Executor& e) {
    // SQL has no negative literal yet, but the catalog takes any INT64 key: its
    // tree must order them as numbers, not as unsigned bit patterns.
    e.run_sql("CREATE TABLE neg (id INT64 NOT NULL, v INT32)");
    for (std::int64_t k : std::initializer_list<std::int64_t>{5, -3, 0, INT64_MIN, 42, -1})
      REQUIRE(cat.insert_row("neg", Row{Field::Int64(k), Field::Int32(1)}));
    require_same(rows_of(e.run_sql("SELECT id FROM neg")),
                 {{"-9223372036854775808"}, {"-3"}, {"-1"}, {"0"}, {"5"}, {"42"}}, "neg scan", true);
    require_same(rows_of(e.run_sql("SELECT id FROM neg WHERE id < 2")),
                 {{"-9223372036854775808"}, {"-3"}, {"-1"}, {"0"}}, "neg range", true);
    require_same(rows_of(e.run_sql("SELECT MIN(id), MAX(id) FROM neg")),
                 {{"-9223372036854775808", "42"}}, "neg min/max", true);
    REQUIRE(e.explain_last().find("IndexMinMax") != std::string::npos);
    // everything built above survived the reopen: counts, indexes, plans
    REQUIRE(cat.find_table("t")->rows == 3000 && cat.find_table("u")->rows == 4000);
    REQUIRE(e.run_sql("SELECT id FROM t WHERE b = 123456").row_count() ==
            static_cast<std::size_t>(std::count_if(T.begin(), T.end(), [](const GenRow& r) {
              return holds(r[2], "=", vint(123456));
            })));
    REQUIRE(e.explain_last().find("IndexScan t (b)") != std::string::npos);
    std::printf("PASS keys: negative keys in numeric order through scan, range and MIN/MAX; counts and "
                "indexes survive a reopen\n");
  });

  ::unlink(path);
  std::printf("u11_index_test: PASS\n");
  return 0;
}
