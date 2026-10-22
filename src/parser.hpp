#pragma once
// Parser -- tokens into an abstract syntax tree. Unit 7; joins in Unit 9;
// aggregates, GROUP BY, HAVING, ORDER BY and LIMIT in Unit 10.
//
// A hand-written recursive-descent parser for the engine's SQL subset:
// CREATE TABLE, INSERT, and SELECT -- since Unit 9 over one or more tables
// joined with JOIN ... ON, with qualified columns and AND-ed conditions. The
// SELECT grammar, in EBNF:
//
//   select     = "SELECT" ( "*" | item { "," item } )
//                "FROM" table_ref { join } [ "WHERE" condition ]
//                [ "GROUP" "BY" column_ref { "," column_ref } ]
//                [ "HAVING" condition ]
//                [ "ORDER" "BY" order_item { "," order_item } ]
//                [ "LIMIT" integer ] ;
//   item       = value [ "AS" identifier ] ;
//   value      = column_ref | aggregate ;
//   aggregate  = ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" )
//                "(" ( "*" | column_ref ) ")" ;
//   order_item = value [ "ASC" | "DESC" ] ;
//   table_ref  = identifier [ [ "AS" ] identifier ] ;
//   join       = [ "INNER" ] "JOIN" table_ref "ON" condition ;
//   condition  = comparison { "AND" comparison } ;
//   comparison = value op ( literal | column_ref ) ;
//   column_ref = identifier [ "." identifier ] ;
//   op         = "=" | "<>" | "<" | "<=" | ">" | ">=" ;
//
// The aggregate names are not reserved words: an identifier followed by '('
// is a function call, so a column may still be called `count`.
//
// Each grammar rule
// is one small method that consumes the tokens it expects and calls the
// methods for its sub-parts. When a token is not what the grammar expects,
// the parser throws a ParseError carrying the token's position, so the caller
// can print the offending line with a caret under it.

#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"
#include "token.hpp"

namespace labdb {

struct ParseError {
  std::string message;
  std::size_t line, col;
};

// Render one source line with a caret under (line, col) -- the shape every
// good compiler error takes. 1-based positions.
std::string caret_at(const std::string& src, std::size_t line, std::size_t col,
                     const std::string& message);

class Parser {
 public:
  explicit Parser(std::string src);

  // Parse one statement and stop after it. Tokens that follow are left for
  // the caller -- parse_script uses this to read statement after statement.
  Statement parse_statement();

  // Parse exactly one statement: an optional ';' may follow it, and then the
  // input must END. Unit 9: with AND in the grammar, `WHERE a = 1 b = 2` must
  // be an error, not a query that silently ignores `b = 2`.
  Statement parse_single();

  // Parse a whole script: statements separated/terminated by ';'.
  std::vector<Statement> parse_script();

  const std::string& source() const { return src_; }

 private:
  std::string src_;
  std::vector<Token> toks_;
  std::size_t i_ = 0;

  const Token& peek() const { return toks_[i_]; }
  // The token after the next one (the end token repeats at the end).
  const Token& peek_next() const { return toks_[i_ + 1 < toks_.size() ? i_ + 1 : i_]; }
  const Token& advance() { return toks_[i_++]; }
  bool check(TokenType t) const { return peek().type == t; }
  bool at_end() const { return peek().type == TokenType::kEnd; }

  [[noreturn]] void fail(const std::string& msg) const {
    throw ParseError{msg, peek().line, peek().col};
  }
  // Consume a token of exactly this type or throw a positioned error.
  const Token& expect(TokenType t, const char* what);

  // grammar rules
  Statement parse_create();
  Statement parse_insert();
  Statement parse_select();
  Column parse_column_def();
  Field parse_literal();
  ColType parse_type();
  // Unit 9
  ColumnRef parse_column_ref(const char* what);
  Comparison parse_comparison();
  std::vector<Comparison> parse_condition();
  TableRef parse_table_ref();
  // Unit 10
  SelectItem parse_value(const char* what);
};

}  // namespace labdb
