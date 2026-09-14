// Challenge 1.3 measurement.
// Part 1: sustained erase+insert churn on one page -- throughput, and how
//         often compaction actually fires.
// Part 2: the cost of one compact() as a function of live bytes.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>
#include <vector>

#include "harness.hpp"
#include "page.hpp"
#include "slotted_page.hpp"

using namespace labdb;
using labdb::test::do_not_optimize;
using labdb::test::now_s;

int main() {
  // ---- Part 1: churn ----
  {
    Page page;
    SlottedPage view(page);
    view.init(1);
    std::mt19937 rng(1234);
    std::uniform_int_distribution<std::size_t> len_dist(16, 192);
    std::vector<std::uint8_t> buf(256, 0xAB);

    std::vector<std::uint16_t> live;
    while (true) {  // fill to capacity with random-size records
      auto slot =
          view.insert(std::span<const std::uint8_t>(buf.data(), len_dist(rng)));
      if (!slot) break;
      live.push_back(*slot);
    }

    constexpr int kOps = 2000000;  // one op = one erase + one insert
    long compactions = 0;
    const double t0 = now_s();
    for (int i = 0; i < kOps; ++i) {
      const std::size_t victim = rng() % live.size();
      view.erase(live[victim]);
      live[victim] = live.back();
      live.pop_back();

      std::size_t len = len_dist(rng);
      for (;;) {
        const std::uint16_t frag_before = page.frag_bytes();
        auto slot =
            view.insert(std::span<const std::uint8_t>(buf.data(), len));
        if (slot) {
          // frag > 0 collapsing to 0 across an insert means compact() ran.
          if (frag_before > 0 && page.frag_bytes() == 0) ++compactions;
          live.push_back(*slot);
          break;
        }
        len /= 2;            // page too full for this size: try smaller
        if (len < 8) break;  // give up on this insert
      }
    }
    const double dt = now_s() - t0;
    do_not_optimize(live);

    std::printf(
        "churn on one page: %d erase+insert pairs in %.3f s -> %.0f ns per pair\n",
        kOps, dt, dt / kOps * 1e9);
    std::printf("compactions triggered: %ld (one every %.1f pairs)\n\n",
                compactions,
                double(kOps) / double(compactions ? compactions : 1));
  }

  // ---- Part 2: compact() cost vs live bytes ----
  std::printf("| target fill | live bytes | live records | ns per compact() |\n");
  std::printf("|---:|---:|---:|---:|\n");
  for (int fill_pct : {25, 50, 75, 95}) {
    Page page;
    SlottedPage view(page);
    view.init(1);
    std::vector<std::uint8_t> rec(32, 0xCD);
    const std::size_t target = kPageSize * std::size_t(fill_pct) / 100;
    std::vector<std::uint16_t> slots;
    std::size_t live_bytes = 0;
    while (live_bytes + rec.size() <= target) {
      auto s = view.insert(rec);
      if (!s) break;
      slots.push_back(*s);
      live_bytes += rec.size();
    }
    // Fragment it: erase every other record. What is left is what
    // compact() must move.
    std::size_t erased = 0;
    for (std::size_t i = 0; i < slots.size(); i += 2) {
      view.erase(slots[i]);
      ++erased;
    }
    live_bytes -= erased * rec.size();
    const std::size_t live_records = slots.size() - erased;

    Page snapshot;
    std::memcpy(snapshot.data(), page.data(), kPageSize);

    constexpr int kIters = 50000;
    double t0 = now_s();
    for (int i = 0; i < kIters; ++i) {
      std::memcpy(page.data(), snapshot.data(), kPageSize);  // restore frag
      view.compact();
      do_not_optimize(page.data()[kPageSize - 1]);
    }
    const double with_compact = now_s() - t0;

    t0 = now_s();
    for (int i = 0; i < kIters; ++i) {
      std::memcpy(page.data(), snapshot.data(), kPageSize);
      do_not_optimize(page.data()[kPageSize - 1]);
    }
    const double restore_only = now_s() - t0;

    const double ns = (with_compact - restore_only) / kIters * 1e9;
    std::printf("| %d%% | %zu | %zu | %.0f |\n", fill_pct, live_bytes,
                live_records, ns);
  }
  return 0;
}
