#pragma once
// Minimal test/bench harness shared by all unit tests. Not part of the
// engine: nothing in src/ may include this.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#define REQUIRE(cond)                                                      \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: REQUIRE(%s)\n", __FILE__,          \
                   __LINE__, #cond);                                       \
      std::abort();                                                        \
    }                                                                      \
  } while (0)

namespace labdb::test {

inline double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Keep a value alive in the optimizer's eyes without paying for a store.
template <class T>
inline void do_not_optimize(const T& v) {
  __asm__ __volatile__("" : : "g"(v) : "memory");
}

}  // namespace labdb::test
