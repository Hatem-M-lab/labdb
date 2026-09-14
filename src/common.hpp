#pragma once
// labdb -- common definitions shared by every layer of the engine.
// Introduced in Challenge 1.1.

#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace labdb {

// The engine's one fixed size. Everything on disk is a multiple of this.
inline constexpr std::size_t kPageSize = 4096;

// Pages are named by 32-bit ids. Page 0 is the pager's meta page and is
// never handed out, so 0 doubles as "no page" in on-disk links.
using PageId = std::uint32_t;
inline constexpr PageId kNullPage = 0;

// The on-disk format is little-endian, byte for byte. We refuse to build on
// a big-endian host rather than pretend to a portability we have not tested.
static_assert(std::endian::native == std::endian::little,
              "labdb's on-disk format assumes a little-endian host");

// ---------------------------------------------------------------------------
// Error policy (fixed here, used for the whole book):
//   * Expected outcomes -- page full, slot empty, key absent -- travel
//     through return values (std::optional, bool). Never exceptions.
//   * Broken invariants and failed syscalls are unrecoverable: we throw,
//     the process reports and dies. A storage engine that limps past a
//     failed write is worse than one that stops.
// ---------------------------------------------------------------------------

[[noreturn]] inline void fail(const std::string& msg) {
  throw std::runtime_error(msg);
}

// For syscall results: attaches strerror(errno) to the message.
inline void check_sys(bool ok, const char* what) {
  if (!ok) fail(std::string(what) + ": " + std::strerror(errno));
}

// For internal invariants.
inline void check_that(bool ok, const char* what) {
  if (!ok) fail(std::string("invariant violated: ") + what);
}

// ---------------------------------------------------------------------------
// Byte-exact field access. All on-disk integers are read and written with
// memcpy: no struct overlays, no alignment assumptions, no strict-aliasing
// violations. Challenge 1.1 measures what this safety costs (nothing).
// ---------------------------------------------------------------------------

inline std::uint16_t load_u16(const std::uint8_t* p) {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

inline std::uint32_t load_u32(const std::uint8_t* p) {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

inline std::uint64_t load_u64(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

inline void store_u16(std::uint8_t* p, std::uint16_t v) {
  std::memcpy(p, &v, sizeof v);
}

inline void store_u32(std::uint8_t* p, std::uint32_t v) {
  std::memcpy(p, &v, sizeof v);
}

inline void store_u64(std::uint8_t* p, std::uint64_t v) {
  std::memcpy(p, &v, sizeof v);
}

}  // namespace labdb
