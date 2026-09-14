#pragma once
// A Page is kPageSize raw bytes plus typed access to the 20-byte header.
// It knows nothing about records. Interpretations of the payload -- slotted
// records in this unit, B+Tree nodes in Unit 2 -- are separate view classes.
// Introduced in Challenge 1.1.

#include <array>
#include <cstdint>

#include "common.hpp"

namespace labdb {

enum class PageType : std::uint16_t {
  kInvalid      = 0,  // all-zero page: allocated but never formatted
  kMeta         = 1,  // page 0 only: pager bookkeeping (Challenge 1.4)
  kSlotted      = 2,  // variable-length records (Challenge 1.2)
  kFree         = 3,  // on the free list (Challenge 1.5)
  kBTreeLeaf    = 4,  // B+Tree leaf: sorted (key,value) pairs (Unit 2)
  kBTreeInternal = 5, // B+Tree internal: keys + child pointers (Unit 2)
};

// Header layout, byte-exact:
//
//   offset  size  field       meaning
//   ------  ----  ----------  ------------------------------------------
//        0     4  page_id     who am I (self-check against file offset)
//        4     2  page_type   PageType
//        6     2  slot_count  slots in the directory, live or dead
//        8     2  free_start  first byte past the slot directory
//       10     2  free_end    first byte of the cell area
//       12     2  frag_bytes  dead bytes inside the cell area
//       14     2  reserved    zero for now; a later unit will claim it
//       16     4  next_page   generic link: free list now, leaf chains later
//
inline constexpr std::size_t kPageHeaderSize = 20;

class Page {
 public:
  Page() : bytes_{} {}  // all zeroes: PageType::kInvalid

  std::uint8_t*       data()       { return bytes_.data(); }
  const std::uint8_t* data() const { return bytes_.data(); }

  PageId id() const       { return load_u32(data() + 0); }
  void set_id(PageId v)   { store_u32(data() + 0, v); }

  PageType type() const   { return static_cast<PageType>(load_u16(data() + 4)); }
  void set_type(PageType v) { store_u16(data() + 4, static_cast<std::uint16_t>(v)); }

  std::uint16_t slot_count() const     { return load_u16(data() + 6); }
  void set_slot_count(std::uint16_t v) { store_u16(data() + 6, v); }

  std::uint16_t free_start() const     { return load_u16(data() + 8); }
  void set_free_start(std::uint16_t v) { store_u16(data() + 8, v); }

  std::uint16_t free_end() const       { return load_u16(data() + 10); }
  void set_free_end(std::uint16_t v)   { store_u16(data() + 10, v); }

  std::uint16_t frag_bytes() const     { return load_u16(data() + 12); }
  void set_frag_bytes(std::uint16_t v) { store_u16(data() + 12, v); }

  PageId next_page() const     { return load_u32(data() + 16); }
  void set_next_page(PageId v) { store_u32(data() + 16, v); }

 private:
  alignas(64) std::array<std::uint8_t, kPageSize> bytes_;
};

static_assert(sizeof(Page) == kPageSize,
              "a Page must be exactly one page of bytes -- nothing more");

}  // namespace labdb
