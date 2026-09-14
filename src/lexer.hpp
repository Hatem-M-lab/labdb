#pragma once
// Lexer -- SQL text into a stream of tokens. Unit 7.
//
// A single left-to-right pass over the source. Whitespace separates tokens
// and is otherwise ignored; keywords are recognized case-insensitively but
// identifiers keep their case (a table is named exactly what the catalog
// stored). Every token carries the line and column of its first character so
// the parser can point a caret at it.
//
// String literals use SQL's doubled-quote escape: inside '...', two single
// quotes '' mean one literal quote, so 'O''Brien' is the five-plus-value
// O'Brien. Getting that escape right is Challenge 7.4's forensic trap.

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "token.hpp"

namespace labdb {

// Thrown on a character the lexer cannot make into a token (a stray '@', an
// unterminated string). Carries a position so the caller can draw a caret.
struct LexError {
  std::string message;
  std::size_t line, col;
};

class Lexer {
 public:
  explicit Lexer(std::string src) : src_(std::move(src)) {}

  std::vector<Token> tokenize() {
    std::vector<Token> out;
    for (;;) {
      Token t = next();
      out.push_back(t);
      if (t.type == TokenType::kEnd) break;
    }
    return out;
  }

 private:
  const std::string src_;
  std::size_t pos_ = 0, line_ = 1, col_ = 1;

  char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
  char peek2() const { return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0'; }
  bool done() const { return pos_ >= src_.size(); }

  char advance() {
    const char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
  }

  void skip_space() {
    while (!done()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') advance();
      else break;
    }
  }

  static bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
  static bool ident_cont(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

  [[noreturn]] void error(const std::string& msg, std::size_t l, std::size_t c) {
    throw LexError{msg, l, c};
  }

  Token next() {
    skip_space();
    Token t;
    t.line = line_;
    t.col = col_;
    if (done()) { t.type = TokenType::kEnd; return t; }

    const char c = peek();
    if (ident_start(c)) return lex_word(t);
    if (std::isdigit((unsigned char)c)) return lex_number(t);
    if (c == '\'') return lex_string(t);
    return lex_operator(t);
  }

  Token lex_word(Token& t) {
    const std::size_t start = pos_;
    while (!done() && ident_cont(peek())) advance();
    t.text = src_.substr(start, pos_ - start);
    t.type = keyword_or_identifier(t.text);
    return t;
  }

  Token lex_number(Token& t) {
    const std::size_t start = pos_;
    while (!done() && std::isdigit((unsigned char)peek())) advance();
    t.text = src_.substr(start, pos_ - start);
    t.type = TokenType::kIntLiteral;
    t.int_val = std::stoll(t.text);
    return t;
  }

  // A single-quoted string with the doubled-quote escape. The opening quote
  // is consumed; then every character is copied into the value until a quote
  // that is NOT immediately followed by another quote -- that one closes the
  // string. A quote followed by a quote is an escaped literal quote: emit one
  // and continue. (Challenge 7.4 shows what goes wrong when the "followed by
  // another quote" case is missed.)
  Token lex_string(Token& t) {
    advance();  // opening '
    std::string value;
    for (;;) {
      if (done()) error("unterminated string literal", t.line, t.col);
      const char c = advance();
      if (c == '\'') {
        if (peek() == '\'') {   // '' -> one literal quote, string continues
          advance();
          value.push_back('\'');
        } else {                // a lone quote closes the string
          break;
        }
      } else {
        value.push_back(c);
      }
    }
    t.type = TokenType::kStrLiteral;
    t.text = value;
    return t;
  }

  Token lex_operator(Token& t) {
    const char c = advance();
    switch (c) {
      case '(': t.type = TokenType::kLParen; t.text = "("; return t;
      case ')': t.type = TokenType::kRParen; t.text = ")"; return t;
      case ',': t.type = TokenType::kComma; t.text = ","; return t;
      case '*': t.type = TokenType::kStar; t.text = "*"; return t;
      case ';': t.type = TokenType::kSemicolon; t.text = ";"; return t;
      case '=': t.type = TokenType::kEq; t.text = "="; return t;
      case '<':
        if (peek() == '=') { advance(); t.type = TokenType::kLe; t.text = "<="; }
        else if (peek() == '>') { advance(); t.type = TokenType::kNe; t.text = "<>"; }
        else { t.type = TokenType::kLt; t.text = "<"; }
        return t;
      case '>':
        if (peek() == '=') { advance(); t.type = TokenType::kGe; t.text = ">="; }
        else { t.type = TokenType::kGt; t.text = ">"; }
        return t;
      default:
        error(std::string("unexpected character '") + c + "'", t.line, t.col);
    }
  }

  static std::string upper(const std::string& s) {
    std::string u = s;
    for (char& ch : u) ch = (char)std::toupper((unsigned char)ch);
    return u;
  }

  static TokenType keyword_or_identifier(const std::string& word) {
    const std::string u = upper(word);
    if (u == "CREATE") return TokenType::kCreate;
    if (u == "TABLE") return TokenType::kTable;
    if (u == "INSERT") return TokenType::kInsert;
    if (u == "INTO") return TokenType::kInto;
    if (u == "VALUES") return TokenType::kValues;
    if (u == "SELECT") return TokenType::kSelect;
    if (u == "FROM") return TokenType::kFrom;
    if (u == "WHERE") return TokenType::kWhere;
    if (u == "INT64") return TokenType::kInt64;
    if (u == "INT32") return TokenType::kInt32;
    if (u == "BOOL") return TokenType::kBool;
    if (u == "TEXT") return TokenType::kText;
    if (u == "NULL") return TokenType::kNull;
    if (u == "NOT") return TokenType::kNot;
    if (u == "TRUE") return TokenType::kTrue;
    if (u == "FALSE") return TokenType::kFalse;
    return TokenType::kIdentifier;
  }
};

}  // namespace labdb
