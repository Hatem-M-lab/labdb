// THE BUG, preserved for study: computing a column's offset by skipping the
// slots of null columns. In the fixed-slot record layout every column owns a
// slot whether or not it is null, so the offset of column i is fixed. A
// decoder that instead advances only past NON-null columns will, for any row
// with an earlier null, land one slot too early and read a different column's
// bytes -- returning a plausible wrong value, with no error at all. Rows
// without an early null decode fine, which is what makes the bug survive
// casual testing.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc tools/u05_null_shift_demo.cpp
//        -o bin/u05_null_shift_demo
#include <cstdint>
#include <cstdio>

#include "record.hpp"

using namespace labdb;

namespace {

// The buggy offset: the bitmap, then only the slots of columns that are NOT
// null. (The correct offset -- Schema::slot_offset -- counts every prior slot.)
std::size_t buggy_offset(const Schema& s, std::span<const std::uint8_t> rec,
                         std::size_t col) {
  std::size_t off = s.null_bitmap_bytes();
  for (std::size_t j = 0; j < col; ++j)
    if (!detail::null_at(rec, j)) off += slot_width(s.columns[j].type);
  return off;
}

Field buggy_project(const Schema& s, std::span<const std::uint8_t> rec,
                    std::size_t col) {
  if (detail::null_at(rec, col)) return Field::Null(s.columns[col].type);
  return detail::read_at(s, rec, col, buggy_offset(s, rec, col));  // wrong off
}

void show(const Schema& s, const Row& row) {
  const auto bytes = encode(s, row);
  const Field ok = project(s, bytes, 2);      // correct
  const Field bug = buggy_project(s, bytes, 2);  // buggy
  std::printf(
      "  row (id=%lld, mid=%s, tail=%lld): correct project(tail)=%lld, "
      "buggy=%lld%s\n",
      (long long)row[0].i64, row[1].is_null ? "NULL" : "present",
      (long long)row[2].i64, (long long)ok.i64, (long long)bug.i64,
      ok.i64 == bug.i64 ? "" : "   <-- WRONG: read the NULL slot, not tail");
}

}  // namespace

int main() {
  // Three fixed-width columns; the middle one is nullable.
  Schema s{{
      {"id", ColType::kInt64, false},
      {"mid", ColType::kInt32, true},
      {"tail", ColType::kInt32, false},
  }};
  std::printf(
      "Every column owns a fixed slot, so tail always sits at the same offset. "
      "The buggy decoder skips null slots.\n\n");
  show(s, Row{Field::Int64(7), Field::Int32(55), Field::Int32(99)});  // no null
  show(s, Row{Field::Int64(7), Field::Null(ColType::kInt32), Field::Int32(99)});  // null mid
  std::printf(
      "\nWith mid present, both agree. With mid NULL, the buggy offset drops "
      "mid's 4-byte slot and reads tail from where mid was -- returning 0, the "
      "zeroed null slot, instead of 99. No error fired.\n");
  return 0;
}
