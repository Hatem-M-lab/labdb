#pragma once
// Timing several things against each other on a shared machine (Unit 12's
// benchmarks). Each round runs every function once, after one round that only
// warms; a slow moment of the machine then falls on all of them alike,
// instead of on whichever happened to be running. Within a round the order is
// shuffled afresh, so that no function always runs right after the same other
// one: a run can leave the machine in a state -- its caches, its allocator --
// that the next run pays for, and a fixed order would charge that to one
// function every time. Not part of the engine.

#include <algorithm>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

#include "harness.hpp"

namespace labdb::test {

struct Timing {
  double median = 0, lo = 0, hi = 0;  // seconds: the median run, the fastest, the slowest
};

inline std::vector<Timing> interleaved(const std::vector<std::function<void()>>& fns, int rounds) {
  for (const auto& f : fns) f();  // warm
  std::vector<std::vector<double>> t(fns.size());
  std::vector<std::size_t> order(fns.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::mt19937 rng(12);  // the same orders on every run of a program
  for (int r = 0; r < rounds; ++r) {
    std::shuffle(order.begin(), order.end(), rng);
    for (std::size_t i : order) {
      const double t0 = now_s();
      fns[i]();
      t[i].push_back(now_s() - t0);
    }
  }
  std::vector<Timing> out;
  for (auto& v : t) {
    std::sort(v.begin(), v.end());
    out.push_back({v[v.size() / 2], v.front(), v.back()});
  }
  return out;
}

}  // namespace labdb::test
