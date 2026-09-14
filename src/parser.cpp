#include "parser.hpp"

#include <sstream>

namespace labdb {

std::string caret_at(const std::string& src, std::size_t line, std::size_t col,
                     const std::string& message) {
  // pull out the requested 1-based line
  std::size_t cur = 1, start = 0;
  for (std::size_t i = 0; i < src.size() && cur < line; ++i)
    if (src[i] == '\n') { start = i + 1; ++cur; }
  std::size_t end = start;
  while (end < src.size() && src[end] != '\n') ++end;
  const std::string text = src.substr(start, end - start);

  std::ostringstream os;
  os << "parse error at line " << line << ", column " << col << ": " << message
     << "\n"
     << "    " << text << "\n"
     << "    ";
  for (std::size_t i = 1; i < col; ++i) os << ' ';
  os << '^';
  return os.str();
}

Parser::Parser(std::string src) : src_(std::move(src)) {
  Lexer lx(src_);
  toks_ = lx.tokenize();  // may throw LexError; the caller renders it
}

const Token& Parser::expect(TokenType t, const char* what) {
  if (!check(t)) {
    fail(std::string("expected ") + what + ", found " +
         (peek().type == TokenType::kEnd
              ? "end of input"
              : std::string("'") + peek().text + "'"));
  }
  return advance();
}

std::vector<Statement> Parser::parse_script() {
  std::vector<Statement> stmts;
  while (!at_end()) {
    stmts.push_back(parse_statement());
    if (check(TokenType::kSemicolon)) advance();
    else if (!at_end()) fail("expected ';' between statements");
  }
  return stmts;
}

Statement Parser::parse_statement() {
  switch (peek().type) {
    case TokenType::kCreate: return parse_create();
    case TokenType::kInsert: return parse_insert();
    case TokenType::kSelect: return parse_select();
    default:
      fail("expected a statement (CREATE, INSERT, or SELECT)");
  }
}

ColType Parser::parse_type() {
  switch (peek().type) {
    case TokenType::kInt64: advance(); return ColType::kInt64;
    case TokenType::kInt32: advance(); return ColType::kInt32;
    case TokenType::kBool: advance(); return ColType::kBool;
    case TokenType::kText: advance(); return ColType::kText;
    default: fail("expected a column type (INT64, INT32, BOOL, or TEXT)");
  }
}

Column Parser::parse_column_def() {
  Column c;
  c.name = expect(TokenType::kIdentifier, "a column name").text;
  c.type = parse_type();
  c.nullable = true;  // nullable unless NOT NULL is stated
  if (check(TokenType::kNot)) {
    advance();
    expect(TokenType::kNull, "NULL after NOT");
    c.nullable = false;
  } else if (check(TokenType::kNull)) {
    advance();  // explicit NULL: the default, accepted for symmetry
  }
  return c;
}

Statement Parser::parse_create() {
  expect(TokenType::kCreate, "CREATE");
  expect(TokenType::kTable, "TABLE");
  Statement s;
  s.kind = Statement::Kind::kCreate;
  s.create.table = expect(TokenType::kIdentifier, "a table name").text;
  expect(TokenType::kLParen, "'(' before the column list");
  for (;;) {
    s.create.schema.columns.push_back(parse_column_def());
    if (check(TokenType::kComma)) { advance(); continue; }
    break;
  }
  expect(TokenType::kRParen, "')' after the column list");
  return s;
}

Field Parser::parse_literal() {
  const Token& t = peek();
  switch (t.type) {
    case TokenType::kIntLiteral:
      advance();
      return Field::Int64(t.int_val);  // width is reconciled to the schema later
    case TokenType::kStrLiteral:
      advance();
      return Field::Text(t.text);
    case TokenType::kTrue: advance(); return Field::Bool(true);
    case TokenType::kFalse: advance(); return Field::Bool(false);
    case TokenType::kNull:
      advance();
      return Field::Null(ColType::kInt64);  // typed against the schema later
    default:
      fail("expected a literal (a number, a 'string', TRUE, FALSE, or NULL)");
  }
}

Statement Parser::parse_insert() {
  expect(TokenType::kInsert, "INSERT");
  expect(TokenType::kInto, "INTO");
  Statement s;
  s.kind = Statement::Kind::kInsert;
  s.insert.table = expect(TokenType::kIdentifier, "a table name").text;
  expect(TokenType::kValues, "VALUES");
  expect(TokenType::kLParen, "'(' before the value list");
  for (;;) {
    s.insert.values.push_back(parse_literal());
    if (check(TokenType::kComma)) { advance(); continue; }
    break;
  }
  expect(TokenType::kRParen, "')' after the value list");
  return s;
}

Comparison Parser::parse_where() {
  Comparison c;
  c.column = expect(TokenType::kIdentifier, "a column name in WHERE").text;
  switch (peek().type) {
    case TokenType::kEq: case TokenType::kNe: case TokenType::kLt:
    case TokenType::kLe: case TokenType::kGt: case TokenType::kGe:
      c.op = advance().type;
      break;
    default:
      fail("expected a comparison operator (=, <>, <, <=, >, >=)");
  }
  c.literal = parse_literal();
  return c;
}

Statement Parser::parse_select() {
  expect(TokenType::kSelect, "SELECT");
  Statement s;
  s.kind = Statement::Kind::kSelect;
  if (check(TokenType::kStar)) {
    advance();
    s.select.star = true;
  } else {
    for (;;) {
      s.select.columns.push_back(
          expect(TokenType::kIdentifier, "a column name").text);
      if (check(TokenType::kComma)) { advance(); continue; }
      break;
    }
  }
  expect(TokenType::kFrom, "FROM");
  s.select.table = expect(TokenType::kIdentifier, "a table name").text;
  if (check(TokenType::kWhere)) {
    advance();
    s.select.where = parse_where();
  }
  return s;
}

}  // namespace labdb
