// Compile + smoke-test the Challenge 1.2 snapshot of slotted_page.hpp.
// Built with -Isnapshots/c12 so "slotted_page.hpp" resolves to the
// snapshot while page.hpp/common.hpp come from src/.
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "harness.hpp"
#include "slotted_page.hpp"

using namespace labdb;

int main() {
  Page page;
  SlottedPage view(page);
  view.init(1);

  std::vector<std::uint16_t> slots;
  const char* words[] = {"alpha", "beta", "gamma", "delta"};
  for (const char* w : words) {
    auto s = view.insert(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(w), std::strlen(w)));
    REQUIRE(s.has_value());
    slots.push_back(*s);
  }
  for (std::size_t i = 0; i < 4; ++i) {
    auto r = view.read(slots[i]);
    REQUIRE(r && r->size() == std::strlen(words[i]));
    REQUIRE(std::memcmp(r->data(), words[i], r->size()) == 0);
  }

  // Fill a fresh page to capacity with 64-byte records: must be 59.
  Page p2;
  SlottedPage v2(p2);
  v2.init(2);
  std::uint8_t rec[64] = {0};
  int count = 0;
  while (v2.insert(std::span<const std::uint8_t>(rec, sizeof rec))) ++count;
  REQUIRE(count == 59);

  std::printf("c12 snapshot smoke test: PASS (4 words round-trip, 59 x 64 B fill)\n");
  return 0;
}
