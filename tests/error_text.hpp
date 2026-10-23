// What a statement's error says, whatever its type (Unit 14). The engine's
// errors are plain structs -- LexError, ParseError, ExecError -- and its I/O
// errors and broken invariants are std::exceptions; this is the one place that
// knows all of them. Given the statement, a lexing or parsing error is drawn
// with a caret under the place it was found.
#pragma once

#include <exception>
#include <string>

#include "lexer.hpp"
#include "operator.hpp"
#include "parser.hpp"

namespace labdb::test {

inline std::string error_text(std::exception_ptr p, const std::string& sql = "") {
  try {
    std::rethrow_exception(p);
  } catch (const LexError& x) {
    return sql.empty() ? x.message : caret_at(sql, x.line, x.col, x.message);
  } catch (const ParseError& x) {
    return sql.empty() ? x.message : caret_at(sql, x.line, x.col, x.message);
  } catch (const ExecError& x) {
    return x.message;
  } catch (const std::exception& x) {
    return x.what();
  } catch (...) {
    return "an error of unknown type";
  }
}

}  // namespace labdb::test
