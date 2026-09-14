// Unit 7 front-end correctness. Checks three things across the SQL subset:
// (1) each statement type parses into the right AST; (2) unparse is a faithful
// inverse -- parsing a statement, printing it, and parsing THAT gives the same
// normalized text, so the round trip is stable; and (3) malformed input raises
// a ParseError at the right position rather than parsing into something wrong.
#include <cstdio>
#include <string>
#include <vector>

#include "harness.hpp"
#include "parser.hpp"
#include "unparse.hpp"

using namespace labdb;

namespace {

Statement parse_one(const std::string& sql) {
  Parser p(sql);
  return p.parse_statement();
}

// unparse(parse(sql)) is stable: printing then re-parsing then re-printing
// yields the same string. That is the round-trip guarantee.
void check_roundtrip(const std::string& sql) {
  const std::string once = unparse(parse_one(sql));
  const std::string twice = unparse(parse_one(once));
  REQUIRE(once == twice);
}

// A statement that must fail to parse, at a given 1-based (line,col).
void check_error_at(const std::string& sql, std::size_t line, std::size_t col) {
  bool threw = false;
  try {
    Parser p(sql);
    p.parse_statement();
  } catch (const ParseError& e) {
    threw = true;
    REQUIRE(e.line == line && e.col == col);
  } catch (const LexError& e) {
    threw = true;
    REQUIRE(e.line == line && e.col == col);
  }
  REQUIRE(threw);
}

}  // namespace

int main() {
  // ---- CREATE TABLE: AST fields ----
  {
    Statement s = parse_one(
        "CREATE TABLE users (id INT64 NOT NULL, name TEXT, active BOOL)");
    REQUIRE(s.kind == Statement::Kind::kCreate);
    REQUIRE(s.create.table == "users");
    REQUIRE(s.create.schema.columns.size() == 3);
    REQUIRE(s.create.schema.columns[0].name == "id");
    REQUIRE(s.create.schema.columns[0].type == ColType::kInt64);
    REQUIRE(s.create.schema.columns[0].nullable == false);
    REQUIRE(s.create.schema.columns[1].type == ColType::kText);
    REQUIRE(s.create.schema.columns[1].nullable == true);
  }

  // ---- INSERT: literals, including the '' escape and NULL/TRUE ----
  {
    Statement s = parse_one("INSERT INTO t VALUES (100, 'O''Brien', TRUE, NULL)");
    REQUIRE(s.kind == Statement::Kind::kInsert);
    REQUIRE(s.insert.values.size() == 4);
    REQUIRE(s.insert.values[0].i64 == 100);
    REQUIRE(s.insert.values[1].type == ColType::kText);
    REQUIRE(s.insert.values[1].text == "O'Brien");  // unescaped
    REQUIRE(s.insert.values[2].type == ColType::kBool && s.insert.values[2].i64 == 1);
    REQUIRE(s.insert.values[3].is_null);
  }

  // ---- SELECT: star, column list, and WHERE ----
  {
    Statement a = parse_one("SELECT * FROM logs");
    REQUIRE(a.select.star && a.select.table == "logs" && !a.select.where);

    Statement b = parse_one("SELECT id, name FROM users WHERE id >= 42");
    REQUIRE(!b.select.star);
    REQUIRE(b.select.columns.size() == 2 && b.select.columns[1] == "name");
    REQUIRE(b.select.where.has_value());
    REQUIRE(b.select.where->column == "id");
    REQUIRE(b.select.where->op == TokenType::kGe);
    REQUIRE(b.select.where->literal.i64 == 42);
  }

  // ---- case-insensitive keywords, case-sensitive identifiers ----
  {
    Statement s = parse_one("select Name from Users where Name <> 'x'");
    REQUIRE(s.select.table == "Users");         // identifier case preserved
    REQUIRE(s.select.columns[0] == "Name");
  }

  // ---- round-trip stability over a batch ----
  const std::vector<std::string> batch = {
      "CREATE TABLE t (a INT64, b INT32 NOT NULL, c TEXT, d BOOL)",
      "INSERT INTO t VALUES (1, 2, 'hi', FALSE)",
      "INSERT INTO t VALUES (7, 8, 'a''b''c', NULL)",
      "SELECT * FROM t",
      "SELECT a, c FROM t WHERE a < 100",
      "SELECT b FROM t WHERE c = 'needle'",
  };
  for (const auto& sql : batch) check_roundtrip(sql);

  // ---- errors land at the right caret position ----
  check_error_at("SELECT id name FROM users", 1, 11);   // missing comma: at 'name'
  check_error_at("INSERT INTO t VALUES (1, 2", 1, 27);   // missing ')': at end
  check_error_at("CREATE TABLE t (id WOBBLE)", 1, 20);   // bad type: at 'WOBBLE'
  check_error_at("SELECT", 1, 7);                        // truncated: at end
  check_error_at("UPDATE t SET x = 1", 1, 1);            // unsupported verb: at 'UPDATE'

  std::printf(
      "u07_parser_test: PASS (CREATE/INSERT/SELECT ASTs, %zu round-trips "
      "stable, case rules, '' escape, and 5 errors caret-located exactly)\n",
      batch.size());
  return 0;
}
