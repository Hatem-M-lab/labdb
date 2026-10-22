#pragma once
// Printing a result as a table, for the Unit 10 programs whose output the
// book shows. Not part of the engine: nothing in src/ may include this.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "executor.hpp"

namespace labdb::test {

// One value as a person reads it: text unquoted, a DOUBLE to `places` decimals.
inline std::string cell_text(const Field& f, int places = 2) {
  if (f.is_null) return "NULL";
  switch (f.type) {
    case ColType::kText: return f.text;
    case ColType::kBool: return f.i64 ? "TRUE" : "FALSE";
    case ColType::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.*f", places, f.f64);
      return buf;
    }
    default: return std::to_string(f.i64);
  }
}

// The columns and rows, aligned: text to the left, numbers to the right.
inline void print_table(const ResultSet& r, int places = 2, std::FILE* out = stdout) {
  const std::size_t n = r.columns.size();
  std::vector<std::size_t> width(n);
  std::vector<bool> left(n, true);
  for (std::size_t c = 0; c < n; ++c) width[c] = r.columns[c].size();
  for (const auto& row : r.rows)
    for (std::size_t c = 0; c < n; ++c) {
      width[c] = std::max(width[c], cell_text(row[c], places).size());
      if (!row[c].is_null) left[c] = row[c].type == ColType::kText;
    }
  auto line = [&](auto cell) {
    std::string s;
    for (std::size_t c = 0; c < n; ++c) {
      const std::string v = cell(c);
      const std::string pad(width[c] - v.size(), ' ');
      s += (c ? "  " : "") + (left[c] ? v + pad : pad + v);
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    std::fprintf(out, "%s\n", s.c_str());
  };
  line([&](std::size_t c) { return r.columns[c]; });
  line([&](std::size_t c) { return std::string(width[c], '-'); });
  for (const auto& row : r.rows) line([&](std::size_t c) { return cell_text(row[c], places); });
}

}  // namespace labdb::test
