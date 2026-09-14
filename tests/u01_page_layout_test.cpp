// Challenge 1.1 correctness: every header field lands at its documented
// byte offset, little-endian, and round-trips exactly.
#include <cstdint>
#include <cstdio>

#include "common.hpp"
#include "harness.hpp"
#include "page.hpp"

using namespace labdb;

int main() {
  static_assert(sizeof(Page) == kPageSize);
  Page p;

  // A fresh page is all zeroes: unformatted, PageType::kInvalid.
  for (std::size_t i = 0; i < kPageSize; ++i) REQUIRE(p.data()[i] == 0);
  REQUIRE(p.type() == PageType::kInvalid);

  // Fields land at documented offsets, least significant byte first.
  p.set_id(0xAABBCCDDu);
  REQUIRE(p.data()[0] == 0xDD && p.data()[1] == 0xCC &&
          p.data()[2] == 0xBB && p.data()[3] == 0xAA);

  p.set_type(PageType::kSlotted);
  REQUIRE(p.data()[4] == 2 && p.data()[5] == 0);

  p.set_slot_count(0x0102);
  REQUIRE(p.data()[6] == 0x02 && p.data()[7] == 0x01);

  p.set_free_start(20);
  REQUIRE(p.data()[8] == 20 && p.data()[9] == 0);

  p.set_free_end(4096);  // 0x1000: largest legal value, fits in a u16
  REQUIRE(p.data()[10] == 0x00 && p.data()[11] == 0x10);

  p.set_frag_bytes(0x0304);
  REQUIRE(p.data()[12] == 0x04 && p.data()[13] == 0x03);

  p.set_next_page(0x11223344u);
  REQUIRE(p.data()[16] == 0x44 && p.data()[17] == 0x33 &&
          p.data()[18] == 0x22 && p.data()[19] == 0x11);

  // Round trips.
  REQUIRE(p.id() == 0xAABBCCDDu);
  REQUIRE(p.type() == PageType::kSlotted);
  REQUIRE(p.slot_count() == 0x0102);
  REQUIRE(p.free_start() == 20);
  REQUIRE(p.free_end() == 4096);
  REQUIRE(p.frag_bytes() == 0x0304);
  REQUIRE(p.next_page() == 0x11223344u);

  // The reserved bytes were never touched.
  REQUIRE(p.data()[14] == 0 && p.data()[15] == 0);

  std::printf(
      "u01_page_layout_test: PASS (all header fields at documented offsets)\n");
  return 0;
}
