#pragma once
// SlottedPage -- interprets a Page's payload as a set of variable-length
// records addressed by stable slot ids. Challenge 1.2: insert and read.
//
// Layout inside the page:
//
//   [ header 20B ][ slot directory --> ]  free  [ <-- cell area ]
//   ^0            ^kPageHeaderSize   ^free_start ^free_end       ^4096
//
// The directory grows forward, cells grow backward; the page is full when
// they would meet. A slot is 4 bytes: u16 cell offset, u16 cell length.

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "page.hpp"

namespace labdb {

inline constexpr std::size_t kSlotSize = 4;
inline constexpr std::size_t kMaxRecordSize =
    kPageSize - kPageHeaderSize - kSlotSize;  // 4072: one record, one slot

class SlottedPage {
 public:
  explicit SlottedPage(Page& page) : p_(&page) {}

  // Format the underlying page as a fresh, empty slotted page.
  void init(PageId id) {
    p_->set_id(id);
    p_->set_type(PageType::kSlotted);
    p_->set_slot_count(0);
    p_->set_free_start(static_cast<std::uint16_t>(kPageHeaderSize));
    p_->set_free_end(static_cast<std::uint16_t>(kPageSize));
    p_->set_frag_bytes(0);
    p_->set_next_page(kNullPage);
  }

  std::uint16_t slot_count() const { return p_->slot_count(); }

  // Bytes between the end of the directory and the start of the cell area.
  std::size_t contiguous_free() const {
    return static_cast<std::size_t>(p_->free_end()) - p_->free_start();
  }

  // Insert a record. Returns its slot id, or nullopt if it does not fit.
  std::optional<std::uint16_t> insert(std::span<const std::uint8_t> rec) {
    if (rec.empty() || rec.size() > kMaxRecordSize) return std::nullopt;
    const auto len = static_cast<std::uint16_t>(rec.size());
    if (contiguous_free() < len + kSlotSize) return std::nullopt;  // full

    const auto off = static_cast<std::uint16_t>(p_->free_end() - len);
    std::memcpy(p_->data() + off, rec.data(), len);
    p_->set_free_end(off);

    const std::uint16_t slot = slot_count();
    p_->set_slot_count(static_cast<std::uint16_t>(slot + 1));
    p_->set_free_start(
        static_cast<std::uint16_t>(p_->free_start() + kSlotSize));
    set_slot(slot, off, len);
    return slot;
  }

  // Read a record by slot id. The span points into the page: it is valid
  // only until the next mutation of this page.
  std::optional<std::span<const std::uint8_t>> read(std::uint16_t slot) const {
    if (slot >= slot_count()) return std::nullopt;
    return std::span<const std::uint8_t>(p_->data() + slot_off(slot),
                                         slot_len(slot));
  }

 private:
  static constexpr std::size_t slot_pos(std::uint16_t slot) {
    return kPageHeaderSize + kSlotSize * static_cast<std::size_t>(slot);
  }
  std::uint16_t slot_off(std::uint16_t s) const {
    return load_u16(p_->data() + slot_pos(s));
  }
  std::uint16_t slot_len(std::uint16_t s) const {
    return load_u16(p_->data() + slot_pos(s) + 2);
  }
  void set_slot(std::uint16_t s, std::uint16_t off, std::uint16_t len) {
    store_u16(p_->data() + slot_pos(s), off);
    store_u16(p_->data() + slot_pos(s) + 2, len);
  }

  Page* p_;
};

}  // namespace labdb
