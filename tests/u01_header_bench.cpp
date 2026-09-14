// Challenge 1.1 measurement: what does memcpy-based field access cost
// compared to a raw pointer cast? Sum all 2048 u16 values in a page,
// 200,000 times, both ways.
#include <cstdint>
#include <cstdio>

#include "common.hpp"
#include "harness.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::now_s;

namespace {

alignas(64) std::uint8_t g_buf[kPageSize];

// The engine's way: load_u16 (memcpy inside).
std::uint64_t sum_via_memcpy() {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < kPageSize; i += 2) sum += load_u16(g_buf + i);
  return sum;
}

// The "fast, unsafe" way. This violates strict aliasing and exists in this
// benchmark only so we can measure what the shortcut buys. It buys nothing.
std::uint64_t sum_via_cast() {
  std::uint64_t sum = 0;
  const auto* w = reinterpret_cast<const std::uint16_t*>(g_buf);
  for (std::size_t i = 0; i < kPageSize / 2; ++i) sum += w[i];
  return sum;
}

}  // namespace

int main() {
  for (std::size_t i = 0; i < kPageSize; ++i)
    g_buf[i] = static_cast<std::uint8_t>(i * 37 + 11);

  // Correctness first: both must agree.
  if (sum_via_memcpy() != sum_via_cast()) {
    std::fprintf(stderr, "FAIL: sums disagree\n");
    return 1;
  }

  constexpr int kRounds = 200000;
  const double total = double(kRounds) * double(kPageSize / 2);

  std::uint64_t acc = 0;
  double t0 = now_s();
  for (int r = 0; r < kRounds; ++r) {
    acc += sum_via_memcpy();
    do_not_optimize(acc);
  }
  const double memcpy_s = now_s() - t0;

  t0 = now_s();
  for (int r = 0; r < kRounds; ++r) {
    acc += sum_via_cast();
    do_not_optimize(acc);
  }
  const double cast_s = now_s() - t0;
  do_not_optimize(acc);

  std::printf("| method | u16 loads | seconds | ns per load |\n");
  std::printf("|---|---:|---:|---:|\n");
  std::printf("| `load_u16` (memcpy) | %.0f | %.3f | %.4f |\n", total, memcpy_s,
              memcpy_s / total * 1e9);
  std::printf("| pointer cast (UB) | %.0f | %.3f | %.4f |\n", total, cast_s,
              cast_s / total * 1e9);
  return 0;
}
