#pragma once
// Token -- the atoms of SQL text. Unit 7; JOIN, ON, AND, AS and '.' added in
// Unit 9; GROUP BY, HAVING, ORDER BY, LIMIT and decimal literals in Unit 10;
// INDEX in Unit 11.
//
// The front end turns a string of SQL into an abstract syntax tree in two
// stages: a lexer that splits the text into tokens (here), and a parser that
// assembles tokens into statements. A token is the smallest meaningful piece
// -- a keyword, a name, a number, a string, an operator -- tagged with what
// it is and where in the source it came from, so an error can point at it.

#include <cstdint>
#include <string>

namespace labdb {

enum class TokenType : std::uint8_t {
  // literals and names
  kIdentifier,  // a table or column name (case-sensitive)
  kIntLiteral,  // 42
  kFloatLiteral,  // 3.5  (Unit 10: compared against AVG's results)
  kStrLiteral,  // 'hello'  (value already unescaped)
  // keywords (case-insensitive in the source)
  kCreate, kTable, kInsert, kInto, kValues, kSelect, kFrom, kWhere,
  kInt64, kInt32, kBool, kText,   // column types
  kNull, kNot, kTrue, kFalse,
  kJoin, kInner, kOn, kAnd, kAs,  // Unit 9: joins and conjunctions
  kGroup, kBy, kHaving, kOrder, kAsc, kDesc, kLimit,  // Unit 10
  kIndex,                         // Unit 11: CREATE INDEX
  // operators
  kEq, kNe, kLt, kLe, kGt, kGe,   // =  <>  <  <=  >  >=
  // punctuation
  kLParen, kRParen, kComma, kStar, kSemicolon,
  kDot,     // Unit 9: the '.' in a qualified column, o.customer
  kEnd,     // end of input
};

struct Token {
  TokenType type = TokenType::kEnd;
  std::string text;         // the exact source slice (or unescaped string value)
  std::int64_t int_val = 0; // valid when type == kIntLiteral
  double float_val = 0;     // valid when type == kFloatLiteral
  std::size_t line = 1;     // 1-based line, for error carets
  std::size_t col = 1;      // 1-based column of the token's first character
};

inline const char* token_type_name(TokenType t) {
  switch (t) {
    case TokenType::kIdentifier: return "identifier";
    case TokenType::kIntLiteral: return "integer";
    case TokenType::kFloatLiteral: return "decimal";
    case TokenType::kStrLiteral: return "string";
    case TokenType::kCreate: return "CREATE";
    case TokenType::kTable: return "TABLE";
    case TokenType::kInsert: return "INSERT";
    case TokenType::kInto: return "INTO";
    case TokenType::kValues: return "VALUES";
    case TokenType::kSelect: return "SELECT";
    case TokenType::kFrom: return "FROM";
    case TokenType::kWhere: return "WHERE";
    case TokenType::kInt64: return "INT64";
    case TokenType::kInt32: return "INT32";
    case TokenType::kBool: return "BOOL";
    case TokenType::kText: return "TEXT";
    case TokenType::kNull: return "NULL";
    case TokenType::kNot: return "NOT";
    case TokenType::kTrue: return "TRUE";
    case TokenType::kFalse: return "FALSE";
    case TokenType::kJoin: return "JOIN";
    case TokenType::kInner: return "INNER";
    case TokenType::kOn: return "ON";
    case TokenType::kAnd: return "AND";
    case TokenType::kAs: return "AS";
    case TokenType::kGroup: return "GROUP";
    case TokenType::kBy: return "BY";
    case TokenType::kHaving: return "HAVING";
    case TokenType::kOrder: return "ORDER";
    case TokenType::kAsc: return "ASC";
    case TokenType::kDesc: return "DESC";
    case TokenType::kLimit: return "LIMIT";
    case TokenType::kIndex: return "INDEX";
    case TokenType::kEq: return "=";
    case TokenType::kNe: return "<>";
    case TokenType::kLt: return "<";
    case TokenType::kLe: return "<=";
    case TokenType::kGt: return ">";
    case TokenType::kGe: return ">=";
    case TokenType::kLParen: return "(";
    case TokenType::kRParen: return ")";
    case TokenType::kComma: return ",";
    case TokenType::kStar: return "*";
    case TokenType::kSemicolon: return ";";
    case TokenType::kDot: return ".";
    case TokenType::kEnd: return "end of input";
  }
  return "?";
}

}  // namespace labdb
