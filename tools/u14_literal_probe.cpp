// Unit 14's forensic trap: the numbers the engine could not write.
//
// A handful of statements with numbers in them -- negative ones, the two ends
// of INT64, one too large for 64 bits -- run through run_sql against a new
// database, each with what came of it: rows, or the error, by its type. It
// uses nothing but run_sql, so it builds against any unit's engine from Unit 8
// on: scripts/u14_before_after.sh builds it against Unit 13's as well as this
// one. With --check it exits non-zero unless every statement does what this
// engine must.
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "lexer.hpp"
#include "pager.hpp"
#include "parser.hpp"

using namespace labdb;

namespace {

struct Probe {
  const char* sql;
  const char* expect;  // what this engine says
};

const Probe kProbes[] = {
    {"CREATE TABLE t (id INT64 NOT NULL, a INT32, s TEXT)", "ok"},
    {"INSERT INTO t VALUES (-1500, -3, 'minus')", "ok"},
    {"INSERT INTO t VALUES (-9223372036854775808, 0, 'the smallest key')", "ok"},
    {"INSERT INTO t VALUES (9223372036854775807, 1, 'the largest key')", "ok"},
    {"SELECT id, a, s FROM t WHERE id < -1", "-9223372036854775808|0|the smallest key ; -1500|-3|minus"},
    {"SELECT COUNT(*), MIN(a), SUM(a) FROM t WHERE a > -5", "3|-3|-2"},
    {"SELECT id FROM t WHERE id = 99999999999999999999",
     "lex error: integer literal out of range: 99999999999999999999"},
    {"SELECT id FROM t LIMIT -1", "parse error: LIMIT must not be negative"},
    {"SELECT id FROM t WHERE a = - 3", "lex error: unexpected character '-'"},
    {"INSERT INTO t VALUES (8, 1, 'x', 4)", "error: INSERT has 4 values but table t has 3 columns"},
};

std::string outcome(Executor& e, const char* sql) {
  try {
    const ResultSet r = e.run_sql(sql);
    if (r.columns.empty()) return "ok";
    std::string out;
    for (const auto& row : r.rows) {
      if (!out.empty()) out += " ; ";
      for (std::size_t i = 0; i < row.size(); ++i) {
        const Field& f = row[i];
        out += (i ? "|" : "") + (f.is_null ? std::string("NULL") : f.type == ColType::kText ? f.text
                                                                   : std::to_string(f.i64));
      }
    }
    return out.empty() ? "no rows" : out;
  } catch (const LexError& x) {
    return "lex error: " + x.message;
  } catch (const ParseError& x) {
    return "parse error: " + x.message;
  } catch (const ExecError& x) {
    return "error: " + x.message;
  } catch (const std::out_of_range& x) {
    return std::string("std::out_of_range, from the standard library: ") + x.what();
  } catch (const std::exception& x) {
    return std::string("std::exception: ") + x.what();
  }
}

}  // namespace

int main(int argc, char** argv) {
  const bool check = argc > 1 && std::strcmp(argv[1], "--check") == 0;
  const char* path = "u14_literal_probe.db";
  ::unlink(path);
  ::unlink("u14_literal_probe.db-wal");
  int wrong = 0;
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    for (const Probe& p : kProbes) {
      const std::string got = outcome(e, p.sql);
      const bool ok = got == p.expect;
      wrong += !ok;
      std::printf("  %-66s %s%s\n", p.sql, got.c_str(), check && !ok ? "   <-- WRONG" : "");
    }
  }
  ::unlink(path);
  ::unlink("u14_literal_probe.db-wal");
  if (check) std::printf("\n%s\n", wrong ? "u14_literal_probe: FAIL" : "u14_literal_probe: PASS");
  return check && wrong ? 1 : 0;
}
