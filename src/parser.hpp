#pragma once
// Parser -- tokens into an abstract syntax tree. Unit 7.
//
// A hand-written recursive-descent parser for the engine's SQL subset:
// CREATE TABLE, INSERT, and SELECT with an optional WHERE. Each grammar rule
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

  // Parse a single statement (an optional trailing ';' is accepted).
  Statement parse_statement();

  // Parse a whole script: statements separated/terminated by ';'.
  std::vector<Statement> parse_script();

  const std::string& source() const { return src_; }

 private:
  std::string src_;
  std::vector<Token> toks_;
  std::size_t i_ = 0;

  const Token& peek() const { return toks_[i_]; }
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
  Comparison parse_where();
  ColType parse_type();
};

}  // namespace labdb
