// The front end, measured. Parses a script of every supported statement kind
// into ASTs, prints each back to normalized SQL and confirms the round trip is
// stable, shows a caret-pointed error on a bad query, and times how fast the
// tokenizer-plus-parser turns text into trees.
#include <cstdio>
#include <string>
#include <vector>

#include "harness.hpp"
#include "parser.hpp"
#include "unparse.hpp"

using labdb::Parser;
using labdb::ParseError;
using labdb::LexError;
using labdb::Statement;
using labdb::caret_at;
using labdb::unparse;
using labdb::test::now_s;

int main() {
  const std::string script =
      "CREATE TABLE users (id INT64 NOT NULL, name TEXT, age INT32);\n"
      "CREATE TABLE orders (order_id INT64 NOT NULL, customer TEXT, cents INT32 NOT NULL);\n"
      "INSERT INTO users VALUES (1, 'Ada', 36);\n"
      "INSERT INTO users VALUES (2, 'O''Brien', 41);\n"
      "INSERT INTO orders VALUES (100, 'Ada', 4200);\n"
      "SELECT * FROM users;\n"
      "SELECT name, age FROM users WHERE age >= 40;\n"
      "SELECT customer FROM orders WHERE cents < 5000;\n"
      "SELECT id FROM users WHERE name <> 'admin';\n";

  // ---- parse the whole script, and round-trip every statement ----
  Parser p(script);
  std::vector<Statement> stmts = p.parse_script();
  std::printf("parsed a %zu-statement script; each statement, normalized:\n\n",
              stmts.size());
  for (const Statement& s : stmts) {
    const std::string out = unparse(s);
    // round-trip must be stable
    Parser again(out);
    REQUIRE(unparse(again.parse_statement()) == out);
    std::printf("  %s\n", out.c_str());
  }

  // ---- a caret-pointed error, shown in full ----
  const std::string bad = "SELECT name age FROM users WHERE age > 30";
  std::printf("\na malformed query and the error it produces:\n\n");
  try {
    Parser bp(bad);
    bp.parse_statement();
    REQUIRE(false);  // must not reach here
  } catch (const ParseError& e) {
    std::printf("%s\n", caret_at(bad, e.line, e.col, e.message).c_str());
  }

  // ---- throughput ----
  const int kIters = 200000;
  const double t0 = now_s();
  std::size_t sink = 0;
  for (int k = 0; k < kIters; ++k) {
    Parser pp(script);
    sink += pp.parse_script().size();
  }
  const double dt = now_s() - t0;
  REQUIRE(sink == static_cast<std::size_t>(kIters) * stmts.size());
  const double stmts_per_s = double(kIters) * stmts.size() / dt;
  std::printf(
      "\nparsed the %zu-statement script %d times (%zu statements) in %.2f s: "
      "%.0f statements/s, %.2f us per statement.\n",
      stmts.size(), kIters, static_cast<std::size_t>(kIters) * stmts.size(), dt,
      stmts_per_s, dt / (double(kIters) * stmts.size()) * 1e6);
  return 0;
}
