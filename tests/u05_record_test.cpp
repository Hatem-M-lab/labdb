// Unit 5 record codec correctness. Generates random typed rows -- every
// column type, NULLs in every position -- then checks two things against the
// original values: decode() reproduces the whole row, and project(col)
// reproduces each column on its own (walking past any NULLs before it). The
// projection checks are also the regression guard for Challenge 5.4's trap.
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include "harness.hpp"
#include "record.hpp"

using namespace labdb;

int main() {
  Schema s{{
      {"id", ColType::kInt64, false},
      {"flag", ColType::kBool, true},
      {"name", ColType::kText, true},
      {"score", ColType::kInt32, true},
      {"note", ColType::kText, true},
  }};

  std::mt19937_64 rng(20260705);
  const int kRows = 200000;
  std::size_t null_hits = 0, text_bytes = 0;

  for (int r = 0; r < kRows; ++r) {
    Row row(s.size());
    // id: never null
    row[0] = Field::Int64(static_cast<std::int64_t>(rng()));
    // flag
    row[1] = (rng() % 4 == 0) ? Field::Null(ColType::kBool)
                              : Field::Bool(rng() & 1);
    // name: variable-length, sometimes empty, sometimes null
    if (rng() % 5 == 0) {
      row[2] = Field::Null(ColType::kText);
    } else {
      const std::size_t len = rng() % 24;  // 0..23 chars, incl. empty
      std::string t;
      for (std::size_t i = 0; i < len; ++i) t.push_back('a' + (rng() % 26));
      row[2] = Field::Text(t);
    }
    // score
    row[3] = (rng() % 3 == 0)
                 ? Field::Null(ColType::kInt32)
                 : Field::Int32(static_cast<std::int32_t>(rng()));
    // note: occasionally long
    if (rng() % 6 == 0) {
      row[4] = Field::Null(ColType::kText);
    } else {
      const std::size_t len = (rng() % 10 == 0) ? 300 : (rng() % 12);
      row[4] = Field::Text(std::string(len, 'x'));
    }

    for (const auto& v : row)
      if (v.is_null) ++null_hits;
    text_bytes += row[2].text.size() + row[4].text.size();

    const auto bytes = encode(s, row);

    // 1) whole-row decode matches
    const Row back = decode(s, bytes);
    REQUIRE(back.size() == row.size());
    for (std::size_t c = 0; c < row.size(); ++c) REQUIRE(back[c] == row[c]);

    // 2) each column projected on its own matches -- including columns that
    //    sit AFTER a NULL, which is exactly the trap's failure mode.
    for (std::size_t c = 0; c < row.size(); ++c)
      REQUIRE(project(s, bytes, c) == row[c]);
  }

  std::printf(
      "u05_record_test: PASS (%d rows, all types, %zu NULLs across every "
      "position; decode and per-column projection both exact; %zu text bytes "
      "round-tripped)\n",
      kRows, null_hits, text_bytes);
  return 0;
}
