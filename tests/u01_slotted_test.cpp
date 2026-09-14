// Challenges 1.2 + 1.3 correctness.
// Phase 1: deterministic fill/read for four record sizes (also produces
//          the records-per-page table).
// Phase 2: 300,000 randomized insert/erase operations checked against an
//          in-memory reference model, with page invariants verified
//          throughout. Seed is fixed: every run is the same run.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <span>
#include <vector>

#include "harness.hpp"
#include "page.hpp"
#include "slotted_page.hpp"

using namespace labdb;

namespace {

using Model = std::map<std::uint16_t, std::vector<std::uint8_t>>;

std::vector<std::uint8_t> make_record(std::uint32_t seed, std::size_t len) {
  std::vector<std::uint8_t> r(len);
  for (std::size_t i = 0; i < len; ++i)
    r[i] = static_cast<std::uint8_t>(seed * 2654435761u + i * 97);
  return r;
}

// The page invariants that must hold after every operation.
void check_invariants(Page& page, const Model& model) {
  SlottedPage view(page);
  REQUIRE(page.free_start() ==
          kPageHeaderSize + kSlotSize * page.slot_count());
  REQUIRE(page.free_start() <= page.free_end());
  REQUIRE(page.free_end() <= kPageSize);

  std::size_t live_bytes = 0;
  for (const auto& [slot, rec] : model) live_bytes += rec.size();
  // The cell area is exactly live bytes plus dead (fragmented) bytes.
  REQUIRE(kPageSize - page.free_end() == live_bytes + page.frag_bytes());

  // Every record the model knows about reads back byte-identical.
  for (const auto& [slot, rec] : model) {
    auto got = view.read(slot);
    REQUIRE(got.has_value());
    REQUIRE(got->size() == rec.size());
    REQUIRE(std::memcmp(got->data(), rec.data(), rec.size()) == 0);
  }
}

}  // namespace

int main() {
  // ---- Phase 1: fill with fixed-size records, read everything back ----
  std::printf(
      "records per page by record size (page %zu B, header %zu B, slot %zu B):\n\n",
      kPageSize, kPageHeaderSize, kSlotSize);
  std::printf("| record size | records/page | payload bytes | payload utilization |\n");
  std::printf("|---:|---:|---:|---:|\n");
  for (std::size_t size : {std::size_t{16}, std::size_t{64}, std::size_t{256},
                           std::size_t{1024}}) {
    Page page;
    SlottedPage view(page);
    view.init(1);
    std::uint32_t count = 0;
    while (true) {
      auto rec = make_record(count, size);
      if (!view.insert(rec).has_value()) break;
      ++count;
    }
    for (std::uint16_t s = 0; s < count; ++s) {
      auto got = view.read(s);
      auto expect = make_record(s, size);
      REQUIRE(got && got->size() == size);
      REQUIRE(std::memcmp(got->data(), expect.data(), size) == 0);
    }
    std::printf("| %zu B | %u | %zu | %.1f%% |\n", size, count,
                std::size_t{count} * size,
                100.0 * double(count) * double(size) / double(kPageSize));
  }

  // ---- Phase 2: randomized churn against a reference model ----
  Page page;
  SlottedPage view(page);
  view.init(7);
  Model model;
  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> len_dist(8, 256);

  constexpr int kOps = 300000;
  int inserts = 0, erases = 0, full_rejections = 0, compactions = 0;
  std::uint32_t next_seed = 1000;

  for (int op = 0; op < kOps; ++op) {
    const bool do_insert = model.empty() || (rng() % 100) < 55;
    if (do_insert) {
      auto rec = make_record(next_seed++, len_dist(rng));
      const std::uint16_t frag_before = page.frag_bytes();
      auto slot = view.insert(rec);
      if (slot) {
        // frag > 0 collapsing to 0 across an insert means compact() ran.
        if (frag_before > 0 && page.frag_bytes() == 0) ++compactions;
        REQUIRE(model.count(*slot) == 0);  // never hands out a live slot
        model[*slot] = std::move(rec);
        ++inserts;
      } else {
        // "Full" must be honest: even compaction could not have made room.
        const bool has_dead_slot = view.slot_count() > model.size();
        const std::size_t need = rec.size() + (has_dead_slot ? 0 : kSlotSize);
        REQUIRE(view.total_free() < need);
        ++full_rejections;
      }
    } else {
      auto it = model.begin();
      std::advance(it, rng() % model.size());
      REQUIRE(view.erase(it->first));
      REQUIRE(!view.read(it->first).has_value());  // really gone
      model.erase(it);
      ++erases;
    }
    if (op % 5000 == 0) check_invariants(page, model);
  }
  check_invariants(page, model);

  std::printf("\nrandomized model test: PASS\n");
  std::printf(
      "  ops=%d inserts=%d erases=%d page-full rejections=%d compactions=%d\n",
      kOps, inserts, erases, full_rejections, compactions);
  std::printf("  live records at end=%zu, frag_bytes=%u, contiguous_free=%zu\n",
              model.size(), page.frag_bytes(), view.contiguous_free());
  return 0;
}
