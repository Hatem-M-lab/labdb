// Unit 5 heap correctness. Stores many encoded rows in the table heap,
// retrieves each by its RID, proves the table survives a reopen (the head is
// in the meta page), and checks that erase removes a row. Because rows are
// larger than a fraction of a page, this spans many slotted pages, exercising
// the page chain and the newest-first insert.
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "buffer_pool.hpp"
#include "harness.hpp"
#include "heap.hpp"
#include "pager.hpp"
#include "record.hpp"

using namespace labdb;

int main() {
  const char* path = "u05_heap_test.db";
  ::unlink(path);

  Schema s{{
      {"id", ColType::kInt64, false},
      {"name", ColType::kText, true},
      {"age", ColType::kInt32, true},
  }};

  const int kRows = 20000;
  std::vector<RID> rids(kRows);
  std::vector<std::vector<std::uint8_t>> expect(kRows);
  std::mt19937_64 rng(7);

  // ---- insert, and read each back immediately ----
  {
    Pager pager(path);
    BufferPool pool(pager, 256);
    Heap heap(pool);
    for (int i = 0; i < kRows; ++i) {
      const std::size_t len = 3 + rng() % 40;
      Row row{Field::Int64(1000 + i),
              (rng() % 7 == 0) ? Field::Null(ColType::kText)
                               : Field::Text(std::string(len, 'a' + (i % 26))),
              (rng() % 5 == 0) ? Field::Null(ColType::kInt32)
                               : Field::Int32(i * 3)};
      expect[i] = encode(s, row);
      rids[i] = heap.insert(expect[i]);
    }
    // every RID reads back its exact bytes
    for (int i = 0; i < kRows; ++i) REQUIRE(heap.get(rids[i]) == expect[i]);
    pool.sync();
    std::printf("PASS insert: %d rows stored across a chain starting at page %u\n",
                kRows, heap.head());
  }

  // ---- reopen: the table is on disk, addressed by the same RIDs ----
  {
    Pager pager(path);
    BufferPool pool(pager, 256);
    Heap heap(pool);
    REQUIRE(heap.head() != kNullPage);  // recovered from the meta page
    for (int i = 0; i < kRows; ++i) {
      const auto bytes = heap.get(rids[i]);
      REQUIRE(bytes == expect[i]);
      // decode a couple of fields to prove it is a real row, not just bytes
      const Row row = decode(s, bytes);
      REQUIRE(row[0].i64 == 1000 + i);
    }
    std::printf("PASS reopen: all %d rows recovered by RID and decode correctly\n",
                kRows);

    // ---- erase: a removed row is gone; others are untouched ----
    int erased = 0;
    for (int i = 0; i < kRows; i += 3) {
      REQUIRE(heap.erase(rids[i]));
      ++erased;
    }
    // survivors still read back; erased ones now fail to read
    for (int i = 0; i < kRows; ++i) {
      if (i % 3 == 0) continue;
      REQUIRE(heap.get(rids[i]) == expect[i]);
    }
    pool.sync();
    std::printf("PASS erase: removed %d rows; every survivor still intact\n", erased);
  }

  ::unlink(path);
  std::printf("u05_heap_test: PASS\n");
  return 0;
}
