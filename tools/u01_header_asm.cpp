// Two ways to load a u16 from a byte buffer, isolated so the generated
// code can be inspected:
//   g++ -std=c++20 -O2 -S -o - tools/u01_header_asm.cpp
// (The cast version violates strict aliasing; it exists for comparison.)
#include <cstdint>
#include <cstring>

std::uint16_t load_via_memcpy(const std::uint8_t* p) {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

std::uint16_t load_via_cast(const std::uint8_t* p) {
  return *reinterpret_cast<const std::uint16_t*>(p);
}
