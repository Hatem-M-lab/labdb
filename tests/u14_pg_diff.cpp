// Challenge 14.2: labdb against PostgreSQL.
//
// Thirteen units have checked the engine against itself: the batch path
// against the row path, one plan against another, a crash against no crash.
// This checks it against a database that is known to be right. The same three
// tables and the same rows are loaded into labdb and into a PostgreSQL
// server; then the same statements, generated from a seed in the SQL both
// understand -- queries of ten shapes, INSERTs, and statements both must
// refuse -- run in both, in the same order, and every answer is compared row
// by row. A query whose ORDER BY orders its rows completely is compared in
// order; any other as a set of rows. AVG, which PostgreSQL computes exactly as
// a numeric and labdb as a double, is compared to ten significant digits. A
// statement both refuse is counted as agreement; one that only one refuses is
// a difference.
//
//   u14_pg_diff [QUERIES] [--no-tie]
//
// --no-tie leaves out the one word that sorts after every other (see kWords),
// which the first version of this program did not have.
//
// Needs psql on the PATH and a PostgreSQL server it can reach (the usual
// PGHOST, PGPORT, PGUSER and PGDATABASE). scripts/u14_postgres_check.sh
// starts a private one, runs this, and stops it.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "error_text.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

const char* kPath = "u14_pg_diff.db";

// ---------------------------------------------------------------- the data

// Words that sort near each other: a case apart, a quote apart, a prefix
// apart -- among them words that share their first eight bytes, which is all
// of a text that an index key holds -- and words whose bytes are not ASCII.
// The last, ten bytes of Cyrillic, sorts after every other word, so that the
// largest texts of a table share their index key too.
const char* kWords[] = {
    "apple", "apricot", "Apple", "banana", "band", "bandana", "o''brien", "o''brian", "", "zebra",
    "a", "ab", "abc", "b", "the end", "x y z", "applesau", "applesauce", "applesauce9", "applesaucy",
    "äpfel", "zoë", "émile", "жук", "the quick brown fox jumps over the lazy dog", "яяяяя"};

bool g_no_tie = false;  // --no-tie: without the last word (scripts/u14_mutation_check.sh)

std::string text_lit(std::mt19937_64& r) {
  std::string w = kWords[r() % (std::size(kWords) - (g_no_tie ? 1 : 0))];
  if (r() % 3 == 0) w += std::to_string(r() % 50);
  return "'" + w + "'";
}

struct Table {
  std::string name, labdb_ddl, pg_ddl;
  std::vector<std::string> rows;  // "(v1, v2, ...)", the same text for both
};

std::vector<Table> make_tables() {
  std::mt19937_64 rng(1401);
  std::vector<Table> ts;
  {
    Table t{"t", "CREATE TABLE t (id INT64 NOT NULL, a INT32, b INT64, s TEXT, f BOOL)",
            "CREATE TABLE t (id BIGINT NOT NULL PRIMARY KEY, a INTEGER, b BIGINT, s TEXT, f BOOLEAN)", {}};
    std::vector<std::int64_t> ids(2000);
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = static_cast<std::int64_t>(i) * 3 - 1500;
    ids.push_back(INT64_MIN);  // and the two ends of the key's range
    ids.push_back(INT64_MAX);
    std::shuffle(ids.begin(), ids.end(), rng);  // out of key order, negative keys among them
    for (std::int64_t id : ids) {
      const std::string a = rng() % 10 == 0 ? "NULL" : std::to_string(static_cast<int>(rng() % 16) - 5);
      const std::string b = rng() % 20 == 0 ? "NULL" : std::to_string(static_cast<std::int64_t>(rng() % 2000001) - 1000000);
      const std::string s = rng() % 12 == 0 ? "NULL" : text_lit(rng);
      const std::string f = rng() % 8 == 0 ? "NULL" : rng() % 2 ? "TRUE" : "FALSE";
      t.rows.push_back("(" + std::to_string(id) + ", " + a + ", " + b + ", " + s + ", " + f + ")");
    }
    ts.push_back(t);
  }
  {
    Table u{"u", "CREATE TABLE u (id INT64 NOT NULL, t_id INT64, tag TEXT, w INT32)",
            "CREATE TABLE u (id BIGINT NOT NULL PRIMARY KEY, t_id BIGINT, tag TEXT, w INTEGER)", {}};
    for (int i = 0; i < 3000; ++i) {
      const std::string t_id = rng() % 15 == 0 ? "NULL" : std::to_string(static_cast<std::int64_t>(rng() % 2200) * 3 - 1500);
      u.rows.push_back("(" + std::to_string(i) + ", " + t_id + ", " + text_lit(rng) + ", " + std::to_string(rng() % 50) + ")");
    }
    ts.push_back(u);
  }
  {
    Table c{"c", "CREATE TABLE c (id INT64 NOT NULL, day INT64, v INT64)",
            "CREATE TABLE c (id BIGINT NOT NULL PRIMARY KEY, day BIGINT, v BIGINT)", {}};
    for (int i = 0; i < 5000; ++i)
      c.rows.push_back("(" + std::to_string(i) + ", " + std::to_string(i / 50) + ", " +
                       (rng() % 25 == 0 ? std::string("NULL") : std::to_string(rng() % 1000)) + ")");
    ts.push_back(c);
  }
  return ts;
}

const char* kIndexes[] = {"CREATE INDEX ON t (a)", "CREATE INDEX ON t (s)", "CREATE INDEX ON u (t_id)",
                          "CREATE INDEX ON c (day)"};

// ------------------------------------------------------------- the queries

const char* kOps[] = {"=", "<>", "<", "<=", ">", ">="};

std::string lit(std::mt19937_64& r, char kind) {
  switch (kind) {
    case 'i': {  // an integer for an INT32 column -- now and then far outside its range
      const int k = static_cast<int>(r() % 12);
      if (k == 0) return "3000000000";
      if (k == 1) return "-3000000000";
      if (k == 2) return "NULL";
      return std::to_string(static_cast<int>(r() % 16) - 5);
    }
    case 'b': {
      if (r() % 15 == 0) return "NULL";
      return std::to_string(static_cast<std::int64_t>(r() % 2000001) - 1000000);
    }
    case 'k': {  // a key -- now and then one of the two ends of the range
      const int k = static_cast<int>(r() % 20);
      if (k == 0) return std::to_string(INT64_MIN);
      if (k == 1) return std::to_string(INT64_MAX);
      return std::to_string(static_cast<std::int64_t>(r() % 6000) - 1500);
    }
    case 's':
      if (r() % 15 == 0) return "NULL";
      return text_lit(r);
    default:  // a boolean
      return r() % 2 ? "TRUE" : "FALSE";
  }
}

struct Col {
  const char* name;
  char kind;  // i: INT32, b: INT64, k: the key, s: TEXT, f: BOOL
};
const Col kT[] = {{"id", 'k'}, {"a", 'i'}, {"b", 'b'}, {"s", 's'}, {"f", 'f'}};

std::string term(std::mt19937_64& r, const std::string& q) {
  if (r() % 8 == 0) {
    const char* pairs[][2] = {{"a", "b"}, {"id", "b"}, {"b", "a"}, {"a", "id"}};
    const auto& p = pairs[r() % 4];
    return q + p[0] + " " + kOps[r() % 6] + " " + q + p[1];
  }
  const Col& c = kT[r() % 5];
  return q + c.name + " " + kOps[r() % 6] + " " + lit(r, c.kind);
}

std::string where(std::mt19937_64& r, const std::string& q, int max_terms) {
  std::string w;
  const int n = static_cast<int>(r() % static_cast<std::uint64_t>(max_terms + 1));
  for (int k = 0; k < n; ++k) w += (w.empty() ? " WHERE " : " AND ") + term(r, q);
  return w;
}

struct Query {
  std::string sql;
  bool ordered;  // its ORDER BY determines the order of every row
};

// The rest of a row of t after its key: ", a, b, s, f)".
std::string rest_of_row(std::mt19937_64& r) {
  const std::string a = r() % 10 == 0 ? "NULL" : std::to_string(static_cast<int>(r() % 16) - 5);
  const std::string b = std::to_string(static_cast<std::int64_t>(r() % 2000001) - 1000000);
  return ", " + a + ", " + b + ", " + text_lit(r) + ", " + (r() % 2 ? "TRUE" : "FALSE") + ")";
}

std::vector<Query> make_queries(int n) {
  std::mt19937_64 r(1402);
  std::vector<Query> qs;
  std::int64_t next_id = 10000000;  // keys no row has yet
  while (static_cast<int>(qs.size()) < n) {
    switch (r() % 11) {
      case 0:
      case 1:
      case 2: {  // one table: columns, terms, an order that ends in the key, a limit
        const char* cols[] = {"*", "id, a, s, b", "s, f, id", "b, a", "id"};
        std::string sql = std::string("SELECT ") + cols[r() % 5] + " FROM t" + where(r, "", 3);
        if (r() % 3) {
          const char* keys[] = {"a, ", "s DESC, ", "b, ", "f, a DESC, ", "s, ", "a DESC, s, ", ""};
          sql += std::string(" ORDER BY ") + keys[r() % 7] + (r() % 2 ? "id" : "id DESC");
          if (r() % 2) sql += " LIMIT " + std::to_string(r() % 3 == 0 ? 5000 : r() % 40);
          qs.push_back({sql, true});
        } else {
          qs.push_back({sql, false});
        }
        break;
      }
      case 3:
      case 4: {  // aggregates, with or without groups
        const char* groups[] = {"", "a", "s", "f", "a, f", "s, a"};
        const std::string g = groups[r() % 6];
        const char* aggs[] = {"COUNT(*)", "COUNT(s)", "SUM(b)", "AVG(a)", "MIN(s)", "MAX(b)",
                              "MIN(a)",   "MAX(s)",   "SUM(a)", "COUNT(f)", "AVG(b)", "MIN(b)"};
        std::string list = g;
        for (int k = 1 + static_cast<int>(r() % 3); k > 0; --k) list += (list.empty() ? "" : ", ") + std::string(aggs[r() % 12]);
        std::string sql = "SELECT " + list + " FROM t" + where(r, "", 2);
        if (!g.empty()) sql += " GROUP BY " + g;
        if (!g.empty() && r() % 3 == 0) sql += std::string(" HAVING COUNT(*) ") + kOps[r() % 6] + " " + std::to_string(r() % 300);
        bool ordered = g.empty();  // one row, or none
        if (!g.empty() && r() % 2) {
          sql += " ORDER BY " + g;
          ordered = true;
          if (r() % 3 == 0) sql += " LIMIT " + std::to_string(r() % 8);
        }
        qs.push_back({sql, ordered});
        break;
      }
      case 5:
      case 6: {  // the equi-join, terms on both sides, sometimes a residual ON term
        std::string sql = r() % 2 ? "SELECT t.id, u.id, t.s, u.tag FROM t JOIN u ON u.t_id = t.id"
                                  : "SELECT u.id, t.id, u.w, t.a FROM u JOIN t ON t.id = u.t_id";
        if (r() % 4 == 0) sql += std::string(" AND u.w ") + kOps[r() % 6] + " t.a";
        std::vector<std::string> terms;
        for (int k = static_cast<int>(r() % 3); k > 0; --k) terms.push_back(term(r, "t."));
        if (r() % 2) terms.push_back("u.w < " + std::to_string(r() % 50));
        for (std::size_t i = 0; i < terms.size(); ++i) sql += (i ? " AND " : " WHERE ") + terms[i];
        const bool ordered = r() % 2;
        if (ordered) sql += " ORDER BY t.id, u.id" + (r() % 2 ? " LIMIT " + std::to_string(r() % 60) : std::string());
        qs.push_back({sql, ordered});
        break;
      }
      case 7: {  // groups over a join; a join on text; a join on an inequality
        const int k = static_cast<int>(r() % 3);
        if (k == 0)
          qs.push_back({"SELECT t.a, COUNT(*), SUM(u.w), MAX(u.tag) FROM t JOIN u ON u.t_id = t.id" + where(r, "t.", 1) +
                            " GROUP BY t.a ORDER BY t.a",
                        true});
        else if (k == 1)
          qs.push_back({"SELECT u.id, t.id FROM u JOIN t ON t.s = u.tag WHERE u.w = " + std::to_string(r() % 50) +
                            " AND t.a < " + std::to_string(r() % 10),
                        false});
        else
          qs.push_back({"SELECT t.id, u.id FROM t JOIN u ON u.w " + std::string(kOps[r() % 6]) +
                            " t.a WHERE t.id < " + std::to_string(static_cast<int>(r() % 100) - 1500) +
                            " AND u.id < " + std::to_string(r() % 60) + " ORDER BY t.id, u.id",
                        true});
        break;
      }
      case 8: {  // the clustered table: ranges, groups, HAVING
        const std::int64_t d = static_cast<std::int64_t>(r() % 110);
        const int k = static_cast<int>(r() % 3);
        if (k == 0)
          qs.push_back({"SELECT day, COUNT(*), SUM(v), AVG(v) FROM c WHERE day >= " + std::to_string(d) +
                            " GROUP BY day HAVING COUNT(*) " + kOps[r() % 6] + " " + std::to_string(r() % 60) +
                            " ORDER BY day",
                        true});
        else if (k == 1)
          qs.push_back({"SELECT COUNT(*), SUM(v), MIN(v), MAX(v) FROM c WHERE day " + std::string(kOps[r() % 6]) + " " +
                            std::to_string(d) + " AND v " + kOps[r() % 6] + " " + std::to_string(r() % 1000),
                        true});
        else
          qs.push_back({"SELECT id, v FROM c WHERE day = " + std::to_string(d) + " ORDER BY v DESC, id LIMIT 7", true});
        break;
      }
      case 9: {  // statements that write, and statements both must refuse
        const std::string fresh = std::to_string(next_id);
        const std::string taken = std::to_string(static_cast<std::int64_t>(r() % 2000) * 3 - 1500);
        const char* refuse[] = {"SELECT nope FROM t",
                                "SELECT * FROM nope",
                                "SELECT id FROM t JOIN u ON u.t_id = t.id",
                                "SELECT a, COUNT(*) FROM t GROUP BY s",
                                "SELECT SUM(s) FROM t",
                                "SELECT AVG(f) FROM t",
                                "SELECT * FROM t WHERE s = 5",
                                "SELECT * FROM t WHERE f = 1",
                                "SELECT * FROM t WHERE a = 'x'",
                                "SELECT * FROM t LIMIT -1"};
        const int k = static_cast<int>(r() % 16);
        if (k < 4) {
          qs.push_back({"INSERT INTO t VALUES (" + fresh + rest_of_row(r), false});
          ++next_id;
        } else if (k == 4) {
          qs.push_back({"INSERT INTO t VALUES (" + taken + rest_of_row(r), false});  // a key that exists
        } else if (k == 5) {
          qs.push_back({"INSERT INTO t VALUES (" + fresh + ", 3000000000, 1, 'x', TRUE)", false});
        } else if (k == 6) {
          qs.push_back({"INSERT INTO t VALUES (NULL" + rest_of_row(r), false});
        } else if (k == 7) {
          qs.push_back({"INSERT INTO t VALUES (" + fresh + ", 'x', 1, 'x', TRUE)", false});
        } else if (k == 8) {
          // More values than columns. Not fewer: PostgreSQL then fills the
          // first N columns and leaves the rest to their defaults -- an
          // extension, which the standard disallows, as labdb and SQLite do.
          qs.push_back({"INSERT INTO t VALUES (" + fresh + ", 1, 2, 'x', TRUE, 6)", false});
        } else {
          qs.push_back({refuse[r() % std::size(refuse)], false});
        }
        break;
      }
      default: {  // three tables, and two joins
        qs.push_back({"SELECT x.id, y.id, t.id FROM u x JOIN t ON t.id = x.t_id JOIN u y ON y.t_id = t.id WHERE t.a = " +
                          std::to_string(static_cast<int>(r() % 16) - 5) + " ORDER BY x.id, y.id",
                      true});
        break;
      }
    }
  }
  return qs;
}

// ------------------------------------------------------------- the answers

struct Answer {
  bool error = false;
  std::string message;
  std::vector<std::string> rows;  // "v1|v2|..."
};

// Ten significant digits for anything with a decimal point that reads as a
// number: AVG, as PostgreSQL's numeric and as labdb's double.
std::string norm(const std::string& v) {
  if (v.find('.') == std::string::npos && v.find('e') == std::string::npos) return v;
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  if (end == v.c_str() || *end != '\0') return v;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.10g", d);
  return buf;
}

std::string norm_row(const std::string& line) {
  std::string out, cell;
  for (char c : line + "|") {
    if (c == '|') {
      out += norm(cell) + "|";
      cell.clear();
    } else {
      cell += c;
    }
  }
  return out;
}

std::string cell(const Field& f) {
  if (f.is_null) return "NULL";
  switch (f.type) {
    case ColType::kText: return f.text;
    case ColType::kBool: return f.i64 ? "t" : "f";
    case ColType::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.17g", f.f64);
      return buf;
    }
    default: return std::to_string(f.i64);
  }
}

Answer from_labdb(Executor& e, const std::string& sql) {
  Answer a;
  try {
    for (const auto& row : e.run_sql(sql).rows) {
      std::string line;
      for (std::size_t i = 0; i < row.size(); ++i) line += (i ? "|" : "") + cell(row[i]);
      a.rows.push_back(norm_row(line));
    }
  } catch (...) {
    a.error = true;
    a.message = test::error_text(std::current_exception());
  }
  return a;
}

// Run a script through psql. Its standard output holds "@@@ k" before query
// k's rows; its errors go to standard error as "psql:FILE:LINE: ERROR:  ...",
// and LINE -- the script line the query is on -- says whose error it is.
std::vector<Answer> from_postgres(const std::vector<Query>& qs, const std::string& setup) {
  const char* script = "u14_pg_diff.sql";
  std::FILE* f = std::fopen(script, "w");
  REQUIRE(f);
  std::string head = "\\set ON_ERROR_STOP 0\n\\pset null NULL\n" + setup;
  std::fputs(head.c_str(), f);
  long line = static_cast<long>(std::count(head.begin(), head.end(), '\n'));
  std::map<long, std::size_t> query_at;  // script line -> query
  for (std::size_t i = 0; i < qs.size(); ++i) {
    std::fprintf(f, "\\echo @@@ %zu\n%s;\n", i, qs[i].sql.c_str());
    line += 2;
    query_at[line] = i;
  }
  std::fclose(f);
  REQUIRE(std::system("psql -X -q -A -t -F '|' -f u14_pg_diff.sql > u14_pg_diff.out 2> u14_pg_diff.err") == 0);

  std::vector<Answer> out(qs.size());
  char buf[1 << 16];
  const auto chomp = [](std::string l) {
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    return l;
  };
  std::FILE* o = std::fopen("u14_pg_diff.out", "r");
  REQUIRE(o);
  long at = -1;
  while (std::fgets(buf, sizeof buf, o)) {
    const std::string l = chomp(buf);
    if (l.rfind("@@@ ", 0) == 0) at = std::atol(l.c_str() + 4);
    else if (at >= 0) out[static_cast<std::size_t>(at)].rows.push_back(norm_row(l));
  }
  std::fclose(o);
  std::FILE* e = std::fopen("u14_pg_diff.err", "r");
  REQUIRE(e);
  const std::string prefix = std::string("psql:") + script + ":";
  while (std::fgets(buf, sizeof buf, e)) {
    const std::string l = chomp(buf);
    const std::size_t err = l.find(": ERROR:  ");
    if (l.rfind(prefix, 0) != 0 || err == std::string::npos) continue;  // notices, LINE and caret lines
    const auto q = query_at.find(std::atol(l.c_str() + prefix.size()));
    if (q == query_at.end()) {
      std::printf("PostgreSQL refused the setup: %s\n", l.c_str());
      std::exit(2);
    }
    out[q->second].error = true;
    out[q->second].message = l.substr(err + 10);
  }
  std::fclose(e);
  for (const char* tmp : {"u14_pg_diff.sql", "u14_pg_diff.out", "u14_pg_diff.err"}) ::unlink(tmp);
  return out;
}

// A statement of the load must not fail; if it does, say which.
void load(Executor& e, const std::string& sql) {
  try {
    e.run_sql(sql);
  } catch (...) {
    std::printf("labdb refused the load: %s\n  %s\n", sql.c_str(), test::error_text(std::current_exception(), sql).c_str());
    std::exit(2);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
  g_no_tie = argc > 2 && std::string(argv[2]) == "--no-tie";
  const std::vector<Table> tables = make_tables();
  const std::vector<Query> queries = make_queries(n);

  // ---- labdb ----
  ::unlink(kPath);
  ::unlink((std::string(kPath) + "-wal").c_str());
  Pager pager(kPath);
  BufferPool pool(pager, 4096);
  Catalog cat(pool);
  Executor e(cat);
  std::string setup = "DROP TABLE IF EXISTS t, u, c;\n";
  std::size_t total_rows = 0;
  load(e, "BEGIN");
  for (const Table& t : tables) {
    load(e, t.labdb_ddl);
    setup += t.pg_ddl + ";\n";
    for (std::size_t i = 0; i < t.rows.size(); ++i) {
      load(e, "INSERT INTO " + t.name + " VALUES " + t.rows[i]);
      setup += (i % 500 == 0 ? std::string(i ? ";\n" : "") + "INSERT INTO " + t.name + " VALUES " : std::string(", ")) +
               t.rows[i];
    }
    setup += ";\n";
    total_rows += t.rows.size();
  }
  for (const char* ix : kIndexes) {
    load(e, ix);
    setup += std::string(ix) + ";\n";  // the same statement: PostgreSQL names an unnamed index itself
  }
  load(e, "COMMIT");
  setup += "ANALYZE;\n";

  // ---- PostgreSQL ----
  std::string version;
  if (std::FILE* p = ::popen("psql -X -q -A -t -c 'SHOW server_version' 2>&1", "r")) {
    char buf[256];
    if (std::fgets(buf, sizeof buf, p)) version = buf;
    ::pclose(p);
    while (!version.empty() && (version.back() == '\n' || version.back() == ' ')) version.pop_back();
  }
  const std::vector<Answer> pg = from_postgres(queries, setup);

  // ---- compare ----
  int same = 0, refused = 0, differ = 0;
  std::size_t rows_compared = 0;
  struct Refusal {
    int n = 0;
    std::string theirs;  // what PostgreSQL said, the first time
  };
  std::map<std::string, Refusal> refusals;  // by what labdb said, up to any " = "
  std::vector<std::size_t> diffs;
  std::vector<Answer> labdb_answers;  // in order: a statement may change what the next one sees
  for (const Query& q : queries) labdb_answers.push_back(from_labdb(e, q.sql));
  for (std::size_t i = 0; i < queries.size(); ++i) {
    Answer mine = labdb_answers[i];
    Answer theirs = pg[i];
    if (mine.error || theirs.error) {
      if (mine.error && theirs.error) {
        ++refused;
        Refusal& r = refusals[mine.message.substr(0, mine.message.find(" = "))];
        if (r.n++ == 0) r.theirs = theirs.message;
      } else {
        ++differ;
        diffs.push_back(i);
      }
      continue;
    }
    if (!queries[i].ordered) {
      std::sort(mine.rows.begin(), mine.rows.end());
      std::sort(theirs.rows.begin(), theirs.rows.end());
    }
    if (mine.rows == theirs.rows) {
      ++same;
      rows_compared += mine.rows.size();
    } else {
      ++differ;
      diffs.push_back(i);
    }
  }
  std::printf("labdb against PostgreSQL %s: 3 tables, %zu rows, %zu statements\n\n", version.c_str(), total_rows,
              queries.size());
  std::printf("  the same answer               %5d   (%zu rows, all compared)\n", same, rows_compared);
  std::printf("  refused by both               %5d\n", refused);
  std::printf("  different                     %5d\n", differ);
  for (const auto& [m, r] : refusals)
    std::printf("    %5d  labdb: %s\n           PostgreSQL: %s\n", r.n, m.c_str(), r.theirs.c_str());
  for (std::size_t k = 0; k < diffs.size() && k < 8; ++k) {
    const std::size_t i = diffs[k];
    const Answer& mine = labdb_answers[i];
    std::printf("\n  DIFFERENT: %s\n", queries[i].sql.c_str());
    if (mine.error) std::printf("    labdb:      error: %s\n", mine.message.c_str());
    else std::printf("    labdb:      %zu rows%s%s\n", mine.rows.size(), mine.rows.empty() ? "" : ", first ",
                     mine.rows.empty() ? "" : mine.rows[0].c_str());
    if (pg[i].error) std::printf("    PostgreSQL: error: %s\n", pg[i].message.c_str());
    else std::printf("    PostgreSQL: %zu rows%s%s\n", pg[i].rows.size(), pg[i].rows.empty() ? "" : ", first ",
                     pg[i].rows.empty() ? "" : pg[i].rows[0].c_str());
  }
  std::printf("\n%s\n", differ == 0 ? "u14_pg_diff: PASS" : "u14_pg_diff: DIFFERENCES");
  ::unlink(kPath);
  ::unlink((std::string(kPath) + "-wal").c_str());
  return differ == 0 ? 0 : 1;
}
