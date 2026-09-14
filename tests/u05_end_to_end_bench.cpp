// The record layer, measured -- and the whole stack working together. We
// build a table of typed rows in the heap, index the id column with the
// B+Tree (whose 8-byte value now holds a RID), then look rows up BY KEY:
// search the tree for an id, get a RID, fetch the row from the heap, decode
// it. This is the first time the engine does what a database is for: a key
// finds a real, typed row. Seeds fixed.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "harness.hpp"
#include "heap.hpp"
#include "pager.hpp"
#include "record.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {
Schema make_schema() {
  return Schema{{
      {"id", ColType::kInt64, false},
      {"name", ColType::kText, true},
      {"age", ColType::kInt32, true},
  }};
}
// Deterministic row for id i, so lookups can be verified without storing them.
Row row_for(int i) {
  const std::size_t len = 4 + (i % 20);
  return Row{Field::Int64(i),
             (i % 9 == 0) ? Field::Null(ColType::kText)
                          : Field::Text(std::string(len, 'a' + (i % 26))),
             (i % 7 == 0) ? Field::Null(ColType::kInt32)
                          : Field::Int32(i * 2)};
}
}  // namespace

int main() {
  const char* path = "u05_e2e_bench.db";
  ::unlink(path);
  const Schema s = make_schema();
  const int kN = 500000;

  Pager pager(path);
  BufferPool pool(pager, 8192);

  // ---- build: rows into the heap, (id -> RID) into the tree ----
  double build_s;
  std::uint32_t total_pages;
  {
    Heap heap(pool);
    BTree tree(pool);
    const double t0 = now_s();
    for (int i = 0; i < kN; ++i) {
      const auto bytes = encode(s, row_for(i));
      const RID rid = heap.insert(bytes);
      tree.insert(static_cast<Key>(i), rid_encode(rid));
    }
    pool.sync();
    build_s = now_s() - t0;
    total_pages = pool.page_count();
  }

  // ---- lookup by key: tree -> RID -> heap -> decode, and verify ----
  std::vector<int> probes(200000);
  std::mt19937_64 pr(2024);
  for (auto& p : probes) p = pr() % kN;

  Heap heap(pool);
  BTree tree(pool);
  std::uint64_t checked = 0, mismatches = 0;
  const double t1 = now_s();
  for (int id : probes) {
    const auto v = tree.search(static_cast<Key>(id));  // key -> RID
    if (!v) { ++mismatches; continue; }
    const RID rid = rid_decode(*v);
    const auto bytes = heap.get(rid);                  // RID -> row bytes
    const Row got = decode(s, bytes);                  // bytes -> typed row
    const Row want = row_for(id);
    if (!(got[0] == want[0] && got[1] == want[1] && got[2] == want[2]))
      ++mismatches;
    ++checked;
  }
  const double look_s = now_s() - t1;
  REQUIRE(mismatches == 0);

  // ---- projection: read just the age column vs decoding the whole row ----
  const double t2 = now_s();
  std::int64_t sink1 = 0;
  for (int id : probes) {
    const auto v = tree.search(static_cast<Key>(id));
    const auto bytes = heap.get(rid_decode(*v));
    const Field age = project(s, bytes, 2);            // one slot, no strings
    if (!age.is_null) sink1 += age.i64;
  }
  const double proj_s = now_s() - t2;
  const double t3 = now_s();
  std::int64_t sink2 = 0;
  for (int id : probes) {
    const auto v = tree.search(static_cast<Key>(id));
    const auto bytes = heap.get(rid_decode(*v));
    const Row full = decode(s, bytes);                 // allocates name string
    if (!full[2].is_null) sink2 += full[2].i64;
  }
  const double dec_s = now_s() - t3;
  REQUIRE(sink1 == sink2);

  std::printf(
      "built %d typed rows: heap + index in %u pages (%.2f s, %.0f rows/s).\n",
      kN, total_pages, build_s, kN / build_s);
  std::printf("bytes per row on disk (whole file / rows): %.1f\n",
              double(total_pages) * 4096.0 / kN);
  std::printf(
      "\nlooked up %llu rows BY KEY (tree -> RID -> heap -> decode), 0 "
      "mismatches, at %.0f lookups/s (%.0f ns each).\n",
      (unsigned long long)checked, checked / look_s, look_s / checked * 1e9);
  std::printf(
      "\nprojecting one column vs decoding the whole row (same %llu lookups):\n"
      "  project(age): %.0f ns/row     decode(full row): %.0f ns/row     "
      "%.2fx\n",
      (unsigned long long)probes.size(), proj_s / probes.size() * 1e9,
      dec_s / probes.size() * 1e9, dec_s / proj_s);

  ::unlink(path);
  return 0;
}
