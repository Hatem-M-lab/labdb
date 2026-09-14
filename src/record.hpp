#pragma once
// Record -- typed, variable-length rows encoded into bytes. Unit 5.
//
// After four units a stored value is still an opaque 8-byte integer. A real
// database stores rows: an id, a name, an age, each with a type. This file is
// the codec that turns a typed tuple into bytes and back.
//
// Encoding (fixed-slot, null-aware):
//
//     [ null bitmap : ceil(ncols/8) bytes ]
//     [ FIXED PART : one slot per column, in order ]
//     [ VARIABLE PART : concatenated Text payloads ]
//
// Every column occupies a slot in the fixed part whether or not it is null:
// a fixed-width column's slot holds its bytes; a Text column's slot holds a
// 4-byte pointer (u16 offset, u16 length) into the variable part. Because
// every column has a slot, the byte offset of column i is fixed -- it does
// NOT depend on which earlier columns are null -- so any column can be read
// in one step without decoding the others. A null column's slot is present
// but ignored; the bitmap is the source of truth for null.
//
// That the offset of column i counts *every* prior slot, null or not, is the
// property Challenge 5.4's forensic trap violates: a decoder that skips the
// slots of null columns reads later columns from the wrong place.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common.hpp"  // load/store_u16/u32/u64, check_that, fail

namespace labdb {

enum class ColType : std::uint8_t {
  kInt64 = 1,
  kInt32 = 2,
  kBool = 3,
  kText = 4,
};

inline bool is_fixed(ColType t) { return t != ColType::kText; }
inline std::size_t fixed_width(ColType t) {
  switch (t) {
    case ColType::kInt64: return 8;
    case ColType::kInt32: return 4;
    case ColType::kBool: return 1;
    case ColType::kText: return 0;
  }
  return 0;
}
// Bytes column i occupies in the FIXED part. Text stores a 4-byte pointer.
inline std::size_t slot_width(ColType t) {
  return t == ColType::kText ? 4 : fixed_width(t);
}

struct Column {
  std::string name;
  ColType type;
  bool nullable = true;
};

struct Schema {
  std::vector<Column> columns;
  std::size_t size() const { return columns.size(); }
  std::size_t null_bitmap_bytes() const { return (columns.size() + 7) / 8; }
  std::size_t fixed_part_bytes() const {
    std::size_t w = 0;
    for (const auto& c : columns) w += slot_width(c.type);
    return w;
  }
  // Byte offset of column i's slot: the bitmap, then every prior slot --
  // null or not. Null-independent, and that is the whole point.
  std::size_t slot_offset(std::size_t i) const {
    std::size_t off = null_bitmap_bytes();
    for (std::size_t j = 0; j < i; ++j) off += slot_width(columns[j].type);
    return off;
  }
};

// A single column value, for building or reading a row.
struct Field {
  ColType type = ColType::kInt64;
  bool is_null = false;
  std::int64_t i64 = 0;  // Int64 / Int32 / Bool (0 or 1)
  std::string text;      // Text

  static Field Int64(std::int64_t v) { Field x; x.type=ColType::kInt64; x.i64=v; return x; }
  static Field Int32(std::int32_t v) { Field x; x.type=ColType::kInt32; x.i64=v; return x; }
  static Field Bool(bool v)          { Field x; x.type=ColType::kBool; x.i64=v?1:0; return x; }
  static Field Text(std::string v)   { Field x; x.type=ColType::kText; x.text=std::move(v); return x; }
  static Field Null(ColType t)       { Field x; x.type=t; x.is_null=true; return x; }

  bool operator==(const Field& o) const {
    if (is_null != o.is_null) return false;
    if (is_null) return true;
    if (type == ColType::kText) return text == o.text;
    return i64 == o.i64;
  }
};

using Row = std::vector<Field>;

// ---- encode: a row becomes bytes ----
inline std::vector<std::uint8_t> encode(const Schema& s, const Row& row) {
  check_that(row.size() == s.size(), "row has the wrong number of columns");
  const std::size_t nb = s.null_bitmap_bytes();
  const std::size_t fp = s.fixed_part_bytes();
  std::vector<std::uint8_t> out(nb + fp, 0);  // bitmap + fixed part, zeroed
  std::vector<std::uint8_t> var;              // variable part accumulates
  for (std::size_t i = 0; i < row.size(); ++i) {
    const Field& v = row[i];
    const ColType t = s.columns[i].type;
    const std::size_t off = s.slot_offset(i);
    if (v.is_null) {
      out[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
      continue;  // slot stays zero; bitmap records the null
    }
    switch (t) {
      case ColType::kInt64:
        store_u64(out.data() + off, static_cast<std::uint64_t>(v.i64));
        break;
      case ColType::kInt32:
        store_u32(out.data() + off,
                  static_cast<std::uint32_t>(static_cast<std::int32_t>(v.i64)));
        break;
      case ColType::kBool:
        out[off] = v.i64 ? 1 : 0;
        break;
      case ColType::kText: {
        check_that(v.text.size() <= 0xFFFF, "text too long for a 16-bit length");
        store_u16(out.data() + off, static_cast<std::uint16_t>(var.size()));
        store_u16(out.data() + off + 2, static_cast<std::uint16_t>(v.text.size()));
        var.insert(var.end(), v.text.begin(), v.text.end());
        break;
      }
    }
  }
  out.insert(out.end(), var.begin(), var.end());
  return out;
}

namespace detail {
inline bool null_at(std::span<const std::uint8_t> rec, std::size_t i) {
  return (rec[i / 8] >> (i % 8)) & 1u;
}
// Read the (non-null) value of column i from its fixed slot. `slot_off` is the
// byte offset of that slot; taking it as a parameter is what lets the trap
// demo substitute a wrong offset without duplicating the read logic.
inline Field read_at(const Schema& s, std::span<const std::uint8_t> rec,
                     std::size_t i, std::size_t slot_off) {
  const ColType t = s.columns[i].type;
  Field v;
  v.type = t;
  if (t == ColType::kText) {
    check_that(slot_off + 4 <= rec.size(), "record truncated at a text pointer");
    const std::size_t voff = load_u16(rec.data() + slot_off);
    const std::size_t vlen = load_u16(rec.data() + slot_off + 2);
    const std::size_t vstart = s.null_bitmap_bytes() + s.fixed_part_bytes() + voff;
    check_that(vstart + vlen <= rec.size(), "text pointer runs off the record");
    v.text.assign(reinterpret_cast<const char*>(rec.data()) + vstart, vlen);
  } else {
    const std::size_t w = fixed_width(t);
    check_that(slot_off + w <= rec.size(), "record truncated mid-column");
    if (t == ColType::kInt64) v.i64 = static_cast<std::int64_t>(load_u64(rec.data() + slot_off));
    else if (t == ColType::kInt32) v.i64 = static_cast<std::int32_t>(load_u32(rec.data() + slot_off));
    else v.i64 = rec[slot_off] ? 1 : 0;  // Bool
  }
  return v;
}
}  // namespace detail

// ---- project: decode ONE column, in one step, without touching the rest ----
inline Field project(const Schema& s, std::span<const std::uint8_t> rec,
                     std::size_t col) {
  check_that(col < s.size(), "column index out of range");
  check_that(rec.size() >= s.null_bitmap_bytes(), "record shorter than its bitmap");
  if (detail::null_at(rec, col)) return Field::Null(s.columns[col].type);
  return detail::read_at(s, rec, col, s.slot_offset(col));
}

// ---- decode: bytes become a whole row ----
inline Row decode(const Schema& s, std::span<const std::uint8_t> rec) {
  check_that(rec.size() >= s.null_bitmap_bytes(), "record shorter than its bitmap");
  Row row;
  row.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (detail::null_at(rec, i)) {
      row.push_back(Field::Null(s.columns[i].type));
      continue;
    }
    row.push_back(detail::read_at(s, rec, i, s.slot_offset(i)));
  }
  return row;
}

// ---- RID: a record's address, (page, slot), packed into the B+Tree's u64 ----
struct RID {
  PageId page = kNullPage;
  std::uint16_t slot = 0;
  bool operator==(const RID& o) const { return page == o.page && slot == o.slot; }
};
inline std::uint64_t rid_encode(RID r) {
  return (static_cast<std::uint64_t>(r.page) << 16) | r.slot;
}
inline RID rid_decode(std::uint64_t v) {
  return RID{static_cast<PageId>(v >> 16), static_cast<std::uint16_t>(v & 0xFFFF)};
}

}  // namespace labdb
