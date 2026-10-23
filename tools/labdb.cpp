// labdb -- the engine as a program: SQL in, results out. Unit 14.
//
//   labdb [options] DATABASE            statements from standard input
//   labdb [options] DATABASE -c "SQL"   these statements, then exit
//
//   --plain             one row a line, fields separated by '|', NULL as NULL
//   --durability MODE   full (the default), normal or off (src/wal.hpp)
//
// Statements end with ';'. Each one's rows print as a table -- or plain -- and
// an error prints to standard error, after which the next statement runs (an
// error inside BEGIN ... COMMIT rolls the transaction back, as the executor
// always does). Three commands of the shell's own, alone on a line:
//
//   .tables    the tables, their row counts and indexed columns
//   .explain   the plan of the last SELECT, with the rows each operator
//              produced and the planner's estimates
//   .quit
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "pager.hpp"
#include "parser.hpp"
#include "error_text.hpp"
#include "show.hpp"

using namespace labdb;

namespace {

std::string plain_cell(const Field& f) {
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

void print(const ResultSet& r, bool plain) {
  if (r.columns.empty()) return;  // CREATE, INSERT, BEGIN, ...: nothing to show
  if (!plain) {
    labdb::test::print_table(r, 0, stdout);
    std::printf("(%zu row%s)\n\n", r.row_count(), r.row_count() == 1 ? "" : "s");
    return;
  }
  for (const auto& row : r.rows) {
    for (std::size_t i = 0; i < row.size(); ++i) std::printf("%s%s", i ? "|" : "", plain_cell(row[i]).c_str());
    std::printf("\n");
  }
}

// Split a script at the semicolons that end statements -- not those inside a
// '...' string, where '' is a quote and not its end.
std::vector<std::string> statements(const std::string& text) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : text) {
    if (c == '\'') quoted = !quoted;  // '' toggles twice: still inside
    if (c == ';' && !quoted) {
      out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (cur.find_first_not_of(" \t\r\n") != std::string::npos) out.push_back(cur);
  return out;
}

int usage() {
  std::fprintf(stderr, "usage: labdb [--plain] [--durability full|normal|off] DATABASE [-c SQL]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  bool plain = false;
  Durability durability = Durability::kFull;
  std::string path, command;
  bool have_command = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plain") plain = true;
    else if (a == "--durability" && i + 1 < argc) {
      const std::string m = argv[++i];
      if (m == "full") durability = Durability::kFull;
      else if (m == "normal") durability = Durability::kNormal;
      else if (m == "off") durability = Durability::kOff;
      else return usage();
    } else if (a == "-c" && i + 1 < argc) {
      command = argv[++i];
      have_command = true;
    } else if (path.empty() && !a.empty() && a[0] != '-') {
      path = a;
    } else {
      return usage();
    }
  }
  if (path.empty()) return usage();

  Pager pager(path);
  pager.set_durability(durability);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  Executor e(cat);
  if (pager.recovery().transactions > 0)
    std::fprintf(stderr, "recovered: %llu transactions from the log\n",
                 static_cast<unsigned long long>(pager.recovery().transactions));

  int errors = 0;
  const auto run = [&](std::string sql) {
    sql.erase(0, sql.find_first_not_of(" \t\r\n"));  // so that a caret's line is the statement's first
    try {
      print(e.run_sql(sql), plain);
    } catch (...) {
      std::fflush(stdout);  // what came before the error, before it
      std::fprintf(stderr, "error: %s\n", labdb::test::error_text(std::current_exception(), sql).c_str());
      ++errors;
    }
    std::fflush(stdout);
  };
  if (have_command) {
    for (const std::string& s : statements(command)) run(s);
    return errors ? 1 : 0;
  }

  const bool interactive = ::isatty(0);
  std::string pending, line;
  if (interactive) std::printf("labdb -- %s (statements end with ';', .quit to leave)\n", path.c_str());
  for (;;) {
    if (interactive) std::printf(pending.empty() ? "labdb> " : "   ...> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    if (pending.empty() && !line.empty() && line[0] == '.') {
      if (line == ".quit") break;
      if (line == ".explain") std::printf("%s", e.explain_last(true).c_str());
      else if (line == ".tables") {
        for (const std::string& t : cat.table_names()) {
          const TableInfo info = *cat.find_table(t);
          std::printf("%s: %llu rows", t.c_str(), static_cast<unsigned long long>(info.rows));
          for (std::size_t c = 1; c < info.indexed.size(); ++c)
            if (info.indexed[c]) std::printf(", index on %s", info.schema.columns[c].name.c_str());
          std::printf("\n");
        }
      } else {
        std::fprintf(stderr, "unknown command: %s\n", line.c_str());
      }
      continue;
    }
    pending += line + "\n";
    const std::vector<std::string> parts = statements(pending);
    // run every complete statement; keep an unfinished one for the next line
    const bool complete = pending.find_last_not_of(" \t\r\n") != std::string::npos &&
                          pending[pending.find_last_not_of(" \t\r\n")] == ';';
    const std::size_t n = complete ? parts.size() : parts.size() - (parts.empty() ? 0 : 1);
    for (std::size_t i = 0; i < n; ++i)
      if (parts[i].find_first_not_of(" \t\r\n") != std::string::npos) run(parts[i]);
    pending = complete || parts.empty() ? "" : parts.back();
  }
  if (pending.find_first_not_of(" \t\r\n") != std::string::npos) run(pending);
  if (e.in_transaction()) std::fprintf(stderr, "a transaction was still open: rolled back\n");
  return errors ? 1 : 0;
}
