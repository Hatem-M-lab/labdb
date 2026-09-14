#pragma once
// Token -- the atoms of SQL text. Unit 7.
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
  kStrLiteral,  // 'hello'  (value already unescaped)
  // keywords (case-insensitive in the source)
  kCreate, kTable, kInsert, kInto, kValues, kSelect, kFrom, kWhere,
  kInt64, kInt32, kBool, kText,   // column types
  kNull, kNot, kTrue, kFalse,
  // operators
  kEq, kNe, kLt, kLe, kGt, kGe,   // =  <>  <  <=  >  >=
  // punctuation
  kLParen, kRParen, kComma, kStar, kSemicolon,
  kEnd,     // end of input
};

struct Token {
  TokenType type = TokenType::kEnd;
  std::string text;         // the exact source slice (or unescaped string value)
  std::int64_t int_val = 0; // valid when type == kIntLiteral
  std::size_t line = 1;     // 1-based line, for error carets
  std::size_t col = 1;      // 1-based column of the token's first character
};

inline const char* token_type_name(TokenType t) {
  switch (t) {
    case TokenType::kIdentifier: return "identifier";
    case TokenType::kIntLiteral: return "integer";
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
    case TokenType::kEnd: return "end of input";
  }
  return "?";
}

}  // namespace labdb
