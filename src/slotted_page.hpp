#pragma once
// SlottedPage -- interprets a Page's payload as a set of variable-length
// records addressed by stable slot ids. Challenges 1.2 (insert/read) and
// 1.3 (erase/compaction).
//
// Layout inside the page:
//
//   [ header 20B ][ slot directory --> ]  free  [ <-- cell area ]
//   ^0            ^kPageHeaderSize   ^free_start ^free_end       ^4096
//
// The directory grows forward, cells grow backward; the page is full when
// they would meet. A slot is 4 bytes: u16 cell offset, u16 cell length.
// offset == 0 marks a dead slot -- offset 0 lands inside the header, so no
// live cell can ever be there.

#include <array>
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

  // Contiguous space plus dead bytes that compaction can reclaim.
  std::size_t total_free() const {
    return contiguous_free() + p_->frag_bytes();
  }

  bool live(std::uint16_t slot) const {
    return slot < slot_count() && slot_off(slot) != 0;
  }

  // Insert a record. Returns its slot id, or nullopt if the record cannot
  // fit even after compaction. Compacts automatically when the space
  // exists but is fragmented (Challenge 1.3).
  std::optional<std::uint16_t> insert(std::span<const std::uint8_t> rec) {
    if (rec.empty() || rec.size() > kMaxRecordSize) return std::nullopt;
    const auto len = static_cast<std::uint16_t>(rec.size());

    // Reuse a dead slot if one exists: it costs no directory bytes and
    // keeps the directory from growing without bound under churn.
    std::optional<std::uint16_t> reuse;
    const std::uint16_t n = slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
      if (slot_off(i) == 0) {
        reuse = i;
        break;
      }
    }

    const std::size_t need = len + (reuse ? 0 : kSlotSize);
    if (contiguous_free() < need) {
      if (total_free() < need) return std::nullopt;  // genuinely full
      compact();  // space exists, just fragmented: rebuild the cell area
    }

    const auto off = static_cast<std::uint16_t>(p_->free_end() - len);
    std::memcpy(p_->data() + off, rec.data(), len);
    p_->set_free_end(off);

    std::uint16_t slot;
    if (reuse) {
      slot = *reuse;
    } else {
      slot = n;
      p_->set_slot_count(static_cast<std::uint16_t>(n + 1));
      p_->set_free_start(
          static_cast<std::uint16_t>(p_->free_start() + kSlotSize));
    }
    set_slot(slot, off, len);
    return slot;
  }

  // Read a record by slot id. The span points into the page: it is valid
  // only until the next mutation of this page. (Unit 4's forensic trap is
  // what happens to people who forget this.)
  std::optional<std::span<const std::uint8_t>> read(std::uint16_t slot) const {
    if (!live(slot)) return std::nullopt;
    return std::span<const std::uint8_t>(p_->data() + slot_off(slot),
                                         slot_len(slot));
  }

  // Erase a record. Its bytes become fragmentation; its slot becomes dead
  // and reusable. Live slot ids are never invalidated by an erase.
  bool erase(std::uint16_t slot) {
    if (!live(slot)) return false;
    p_->set_frag_bytes(
        static_cast<std::uint16_t>(p_->frag_bytes() + slot_len(slot)));
    set_slot(slot, 0, 0);
    // Dead slots at the tail of the directory can give their bytes back.
    std::uint16_t n = slot_count();
    while (n > 0 && slot_off(static_cast<std::uint16_t>(n - 1)) == 0) {
      --n;
      p_->set_free_start(
          static_cast<std::uint16_t>(p_->free_start() - kSlotSize));
    }
    p_->set_slot_count(n);
    return true;
  }

  // Rebuild the cell area so it contains live cells only, packed against
  // the end of the page. O(kPageSize) time, one page of scratch space.
  void compact() {
    std::array<std::uint8_t, kPageSize> scratch;
    auto write_pos = static_cast<std::uint16_t>(kPageSize);
    const std::uint16_t n = slot_count();
    for (std::uint16_t i = 0; i < n; ++i) {
      const std::uint16_t off = slot_off(i);
      if (off == 0) continue;  // dead slot: its cell is not copied
      const std::uint16_t len = slot_len(i);
      write_pos = static_cast<std::uint16_t>(write_pos - len);
      std::memcpy(scratch.data() + write_pos, p_->data() + off, len);
      set_slot(i, write_pos, len);
    }
    std::memcpy(p_->data() + write_pos, scratch.data() + write_pos,
                kPageSize - write_pos);
    p_->set_free_end(write_pos);
    p_->set_frag_bytes(0);
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
