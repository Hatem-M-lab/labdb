#pragma once
// Batch -- rows moved many at a time, held column by column. Unit 12.
//
// Units 9 to 11 moved one row per call: next(Row&) fills one Row, a vector of
// 48-byte Fields, and every operator above it pays a call, a few branches and
// a copy for that one row. A Batch carries up to a thousand rows per call, and
// holds them the other way around: one array per column. The values a
// comparison or an aggregate reads are then contiguous and of one type, and an
// operator reads them in a loop with nothing else in it. This is the layout of
// MonetDB/X100's vectors (Boncz, Zukowski and Nes, CIDR 2005).
//
// A Filter does not move the rows that pass; it lists them. When `selected`
// is set, `sel[0 .. live)` holds the positions of the rows still live, and
// every operator reads a batch through count() and row(k) -- the k-th live
// row's position -- never through `size` alone.
//
// A batch's values belong to whoever filled it, and only until the next call
// that refills it. An operator that keeps a value past that (a hash table, a
// sort) copies it into storage of its own.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "record.hpp"

namespace labdb {

// The batch size the executor runs with unless told otherwise: 1,024 rows, the
// size X100 found best (Challenge 12.3 measures this machine's curve).
inline constexpr std::size_t kDefaultBatchRows = 1024;

// One column: a value per row in the array its type uses, and a NULL flag
// per row. Only one of the three value arrays is in use, the one for `type`.
struct ColumnVector {
  ColType type = ColType::kInt64;
  std::vector<std::int64_t> ints;   // INT64, INT32, BOOL: the value as Field::i64 holds it
  std::vector<double> doubles;      // DOUBLE
  std::vector<std::string> texts;   // TEXT
  std::vector<std::uint8_t> nulls;  // 1 where the row's value is NULL

  // Room for `n` rows of type `t`, addressed by position. What is already
  // allocated is kept, so a column formatted once is never allocated again.
  void format(ColType t, std::size_t n) {
    type = t;
    if (nulls.size() < n) nulls.resize(n);
    switch (t) {
      case ColType::kText:
        if (texts.size() < n) texts.resize(n);
        break;
      case ColType::kDouble:
        if (doubles.size() < n) doubles.resize(n);
        break;
      default:
        if (ints.size() < n) ints.resize(n);
        break;
    }
  }

  // Row i := f, a value of this column's type (or NULL).
  void set(std::size_t i, const Field& f) {
    nulls[i] = f.is_null ? 1 : 0;
    if (type == ColType::kText) texts[i].assign(f.is_null ? std::string() : f.text);
    else if (type == ColType::kDouble) doubles[i] = f.is_null ? 0.0 : f.f64;
    else ints[i] = f.is_null ? 0 : f.i64;
  }

  // f := row i, exactly the Field the row engine would hold for it.
  void get(std::size_t i, Field& f) const {
    f.type = type;
    f.is_null = nulls[i] != 0;
    if (type == ColType::kText) {
      if (f.is_null) f.text.clear();
      else f.text.assign(texts[i]);
      f.i64 = 0;
      return;
    }
    if (!f.text.empty()) f.text.clear();
    if (f.is_null) f.i64 = 0;
    else if (type == ColType::kDouble) f.f64 = doubles[i];
    else f.i64 = ints[i];
  }

  // Row i := row j of `src`, a column of the same type.
  void copy_from(std::size_t i, const ColumnVector& src, std::size_t j) {
    nulls[i] = src.nulls[j];
    if (type == ColType::kText) texts[i].assign(src.texts[j]);
    else if (type == ColType::kDouble) doubles[i] = src.doubles[j];
    else ints[i] = src.ints[j];
  }

  // Growing use, for storage that outlives a batch (a hash table's rows, a
  // sort's input, the keys of an aggregate's groups): append row j of `src`.
  void push_from(const ColumnVector& src, std::size_t j) {
    nulls.push_back(src.nulls[j]);
    if (type == ColType::kText) texts.push_back(src.texts[j]);
    else if (type == ColType::kDouble) doubles.push_back(src.doubles[j]);
    else ints.push_back(src.ints[j]);
  }
  void push(const Field& f) {
    nulls.push_back(f.is_null ? 1 : 0);
    if (type == ColType::kText) texts.push_back(f.is_null ? std::string() : f.text);
    else if (type == ColType::kDouble) doubles.push_back(f.is_null ? 0.0 : f.f64);
    else ints.push_back(f.is_null ? 0 : f.i64);
  }
  void clear_rows() {
    nulls.clear();
    ints.clear();
    doubles.clear();
    texts.clear();
  }
  // Bytes this column holds, by sizeof (a string's own buffer, past its
  // inline 15 bytes, aside).
  std::size_t bytes() const {
    return nulls.capacity() + ints.capacity() * sizeof(std::int64_t) +
           doubles.capacity() * sizeof(double) + texts.capacity() * sizeof(std::string);
  }
};

struct Batch {
  std::vector<ColumnVector> cols;
  std::size_t size = 0;            // rows filled, at positions 0 .. size-1
  bool selected = false;           // if set, only the rows listed in sel are live
  std::vector<std::uint32_t> sel;  // their positions, in order: sel[0 .. live)
  std::size_t live = 0;

  std::size_t capacity() const { return sel.size(); }
  std::size_t count() const { return selected ? live : size; }  // the live rows
  std::size_t row(std::size_t k) const { return selected ? sel[k] : k; }  // the k-th one's position

  // One column per type in `types`, room for `n` rows, and empty. Cheap when
  // the batch already is all of that: nothing is allocated twice.
  void format(const std::vector<ColType>& types, std::size_t n) {
    if (cols.size() != types.size()) cols.resize(types.size());
    for (std::size_t c = 0; c < types.size(); ++c) cols[c].format(types[c], n);
    if (sel.size() < n) sel.resize(n);
    size = 0;
    selected = false;
    live = 0;
  }

  // The rows there is room for now, in every column and in sel.
  std::size_t room() const {
    std::size_t r = sel.size();
    for (const ColumnVector& c : cols) r = std::min(r, c.nulls.size());
    return r;
  }

  // Room for at least n rows, the rows already there kept: what a producer
  // that cannot know in advance how many rows it will write calls as it goes
  // -- each time it reaches room(), not for every row. Capacity doubles, so a
  // batch filled a row at a time is regrown only a few times -- and a plan
  // that produces a handful of rows never allocates for a thousand.
  void ensure(std::size_t n) {
    if (room() >= n) return;
    std::size_t room = sel.size() < 16 ? 16 : sel.size();
    while (room < n) room *= 2;
    for (ColumnVector& c : cols) c.format(c.type, room);
    if (sel.size() < room) sel.resize(room);
  }

  void set_row(std::size_t i, const Row& r) {
    for (std::size_t c = 0; c < cols.size(); ++c) cols[c].set(i, r[c]);
  }
  void get_row(std::size_t i, Row& out) const {
    out.resize(cols.size());
    for (std::size_t c = 0; c < cols.size(); ++c) cols[c].get(i, out[c]);
  }

};

// order_key (record.hpp) of row i of a column that holds a value there.
inline std::uint64_t order_key_at(const ColumnVector& c, std::size_t i) {
  if (c.type == ColType::kText) return order_key_text(c.texts[i]);
  if (c.type == ColType::kDouble) return order_key_double(c.doubles[i]);
  return order_key_int(c.ints[i]);
}

// compare_values (operator.hpp) of row i of `a` and row j of `b`, two columns
// of one type, neither NULL there: -1, 0 or 1.
inline int compare_at(const ColumnVector& a, std::size_t i, const ColumnVector& b, std::size_t j) {
  if (a.type == ColType::kText) {
    const int c = a.texts[i].compare(b.texts[j]);
    return (c > 0) - (c < 0);
  }
  if (a.type == ColType::kDouble) return (a.doubles[i] > b.doubles[j]) - (a.doubles[i] < b.doubles[j]);
  return (a.ints[i] > b.ints[j]) - (a.ints[i] < b.ints[j]);
}

// ---- records into batches: the codec's third form (Unit 12) ----

// Decode `n` records into rows at, at+1, ... of a batch formatted with the
// layout's types -- column by column: each column's type is looked at once for
// all n records, and the loop that decodes it does nothing else. The records
// are those of one page, read where they lie; decode_to below is the same for
// one record. The values are exactly decode()'s.
inline void decode_run(const RecordLayout& l, const std::uint8_t* const* recs, const std::size_t* sizes,
                       std::size_t n, Batch& b, std::size_t at) {
  for (std::size_t r = 0; r < n; ++r) check_that(sizes[r] >= l.var_start, "record shorter than its fixed part");
  for (std::size_t c = 0; c < l.types.size(); ++c) {
    ColumnVector& col = b.cols[c];
    std::uint8_t* null = col.nulls.data() + at;
    const std::size_t byte = c / 8, bit = c % 8, slot = l.slot[c];
    switch (l.types[c]) {
      case ColType::kInt64: {
        std::int64_t* v = col.ints.data() + at;
        for (std::size_t r = 0; r < n; ++r) {
          const std::uint8_t nl = (recs[r][byte] >> bit) & 1u;
          null[r] = nl;
          v[r] = nl ? 0 : static_cast<std::int64_t>(load_u64(recs[r] + slot));
        }
        break;
      }
      case ColType::kInt32: {
        std::int64_t* v = col.ints.data() + at;
        for (std::size_t r = 0; r < n; ++r) {
          const std::uint8_t nl = (recs[r][byte] >> bit) & 1u;
          null[r] = nl;
          v[r] = nl ? 0 : static_cast<std::int32_t>(load_u32(recs[r] + slot));
        }
        break;
      }
      case ColType::kBool: {
        std::int64_t* v = col.ints.data() + at;
        for (std::size_t r = 0; r < n; ++r) {
          const std::uint8_t nl = (recs[r][byte] >> bit) & 1u;
          null[r] = nl;
          v[r] = nl ? 0 : (recs[r][slot] ? 1 : 0);
        }
        break;
      }
      case ColType::kDouble: {
        double* v = col.doubles.data() + at;
        for (std::size_t r = 0; r < n; ++r) {
          const std::uint8_t nl = (recs[r][byte] >> bit) & 1u;
          null[r] = nl;
          v[r] = nl ? 0.0 : std::bit_cast<double>(load_u64(recs[r] + slot));
        }
        break;
      }
      case ColType::kText:
        for (std::size_t r = 0; r < n; ++r) {
          const std::uint8_t nl = (recs[r][byte] >> bit) & 1u;
          null[r] = nl;
          std::string& t = col.texts[at + r];
          if (nl) {
            t.clear();
            continue;
          }
          const std::size_t vstart = l.var_start + load_u16(recs[r] + slot);
          const std::size_t vlen = load_u16(recs[r] + slot + 2);
          check_that(vstart + vlen <= sizes[r], "text pointer runs off the record");
          t.assign(reinterpret_cast<const char*>(recs[r]) + vstart, vlen);
        }
        break;
    }
  }
}

// Decode one record into row i of a batch formatted with the layout's types:
// the same values decode() produces, written straight into the columns.
inline void decode_to(const RecordLayout& l, std::span<const std::uint8_t> rec, Batch& b,
                      std::size_t i) {
  check_that(rec.size() >= l.var_start, "record shorter than its fixed part");
  const std::uint8_t* p = rec.data();
  for (std::size_t c = 0; c < l.types.size(); ++c) {
    ColumnVector& col = b.cols[c];
    const bool null = (p[c / 8] >> (c % 8)) & 1u;
    col.nulls[i] = null ? 1 : 0;
    const std::uint8_t* at = p + l.slot[c];
    switch (l.types[c]) {
      case ColType::kInt64: col.ints[i] = null ? 0 : static_cast<std::int64_t>(load_u64(at)); break;
      case ColType::kInt32: col.ints[i] = null ? 0 : static_cast<std::int32_t>(load_u32(at)); break;
      case ColType::kBool: col.ints[i] = null ? 0 : (*at ? 1 : 0); break;
      case ColType::kDouble: col.doubles[i] = null ? 0.0 : std::bit_cast<double>(load_u64(at)); break;
      case ColType::kText: {
        if (null) {
          col.texts[i].clear();
          break;
        }
        const std::size_t vstart = l.var_start + load_u16(at);
        const std::size_t vlen = load_u16(at + 2);
        check_that(vstart + vlen <= rec.size(), "text pointer runs off the record");
        col.texts[i].assign(reinterpret_cast<const char*>(p) + vstart, vlen);
        break;
      }
    }
  }
}

}  // namespace labdb
