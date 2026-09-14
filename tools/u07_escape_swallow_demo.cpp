// THE BUG, preserved for study: ending a string literal at the first closing
// quote without checking whether it is really an escaped '' quote. In SQL,
// two single quotes inside a string mean one literal quote -- 'it''s' is the
// four-character value it's. A lexer that stops at the first inner quote reads
// 'it''s' as the string "it", then tries to lex the leftover 's'' as fresh
// tokens: an identifier `s`, then an unterminated string. The value is
// silently wrong, and the error -- if one surfaces at all -- lands far from
// the real mistake, blaming code that was never wrong.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc tools/u07_escape_swallow_demo.cpp
//        -o bin/u07_escape_swallow_demo
#include <cstdio>
#include <string>
#include <vector>

#include "lexer.hpp"

using namespace labdb;

namespace {

// A buggy string lexer: it copies characters until the first quote and stops
// there, never asking whether that quote is doubled (an escape). Returns the
// value it thinks the string holds and the source offset it stopped at.
std::pair<std::string, std::size_t> buggy_scan_string(const std::string& src,
                                                      std::size_t open) {
  std::string value;
  std::size_t i = open + 1;  // past the opening quote
  for (; i < src.size(); ++i) {
    if (src[i] == '\'') break;  // BUG: first quote wins, escape unchecked
    value.push_back(src[i]);
  }
  return {value, i};
}

// The correct scan, for contrast: '' is an escaped quote and the string
// continues past it.
std::string correct_scan_string(const std::string& src, std::size_t open) {
  std::string value;
  std::size_t i = open + 1;
  for (; i < src.size(); ++i) {
    if (src[i] == '\'') {
      if (i + 1 < src.size() && src[i + 1] == '\'') { value.push_back('\''); ++i; }
      else break;
    } else {
      value.push_back(src[i]);
    }
  }
  return value;
}

}  // namespace

int main() {
  const std::string sql = "INSERT INTO t VALUES ('it''s here')";
  const std::size_t open = sql.find('\'');

  const std::string correct = correct_scan_string(sql, open);
  const auto [buggy, stop] = buggy_scan_string(sql, open);

  std::printf("source: %s\n\n", sql.c_str());
  std::printf("  correct string value: \"%s\"\n", correct.c_str());
  std::printf("  buggy   string value: \"%s\"%s\n", buggy.c_str(),
              correct == buggy ? "" : "   <-- WRONG: truncated at the escaped quote");

  // What the buggy lexer does with the leftover the truncation created. The
  // buggy scan stopped AT the first inner quote, so the leftover the lexer
  // would resume on begins there.
  const std::string leftover = sql.substr(stop);
  std::printf(
      "\nHaving stopped early, the buggy lexer resumes on the leftover "
      "`%s`. It lexes the doubled quote as an empty string, then `s here` as "
      "identifiers, then hits a lone quote it cannot close -- an error that "
      "points into the middle of what was one perfectly good string literal:\n",
      leftover.c_str());
  try {
    Lexer lx(leftover);
    for (const auto& t : lx.tokenize())
      if (t.type == TokenType::kEnd) break;
    std::printf("  (no error surfaced -- the truncation stays silent)\n");
  } catch (const LexError& e) {
    std::printf("  downstream lex error at column %zu of the leftover: %s\n",
                e.col, e.message.c_str());
  }
  std::printf(
      "\nEither way the blame lands far from the real mistake: the string was "
      "never wrong, the scanner's escape handling was.\n");
  return 0;
}
