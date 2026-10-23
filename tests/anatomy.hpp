#pragma once
// The anatomy of a scanned row (Unit 12, Challenges 12.1 and 12.2): one table,
// and the steps of reading every row of it, each a loop of its own, so that
// what each step adds can be measured -- timed by u12_anatomy_bench, counted
// in instructions by u12_row_probe under valgrind. Not part of the engine.
//
// The table is Unit 11's calibration table: events (id, v, w), 200,000 rows,
// the same seed and the same rows as tests/u11_cost_bench.cpp.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "batch.hpp"
#include "btree.hpp"
#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "heap.hpp"
#include "operator.hpp"
#include "harness.hpp"
#include "slotted_page.hpp"

namespace labdb::test {

inline constexpr int kAnatomyRows = 200000;

inline void build_events(Executor& e) {
  std::mt19937_64 rng(1103);
  e.run_sql("CREATE TABLE events (id INT64 NOT NULL, v INT64, w INT64)");
  for (int i = 0; i < kAnatomyRows; ++i)
    e.run_sql("INSERT INTO events VALUES (" + std::to_string(i) + ", " + std::to_string(rng() % 1000000) +
              ", " + std::to_string(rng() % 1000) + ")");
}

// The steps, in the order the chapter builds them up. Each reads the first
// `n` rows of events in key order and returns a checksum of what it read.
enum class Step {
  kWalk,         // the table's own B+Tree, leaf by leaf: each entry's key and RID
  kZeroPage,     // + a fresh Page for the row, which zero-fills its 4 KiB (Heap::get's first line)
  kCopyPage,     // + the row's heap page copied into it out of the pool (read_page)
  kCopyRecord,   // + the record copied out of that copy into a new vector: all of Heap::get
  kDecode,       // + a new Row decoded from the record: Unit 11's TableCursor::next
  kScanCopy,     // SeqScan, with Unit 11's reads (Catalog::set_copy_pages)
  kPin,          // the walk + the row's page pinned (once per page), the record found in the frame
  kDecodeInto,   // + decoded in place into the same Row: Unit 12's TableCursor::next
  kScanPin,      // SeqScan, with Unit 12's reads
  kBatches,      // SeqScan, drained in batches of 1,024 rows
};

inline const char* step_name(Step s) {
  switch (s) {
    case Step::kWalk: return "walk";
    case Step::kZeroPage: return "zero_page";
    case Step::kCopyPage: return "copy_page";
    case Step::kCopyRecord: return "copy_record";
    case Step::kDecode: return "decode";
    case Step::kScanCopy: return "scan_copy";
    case Step::kPin: return "pin";
    case Step::kDecodeInto: return "decode_into";
    case Step::kScanPin: return "scan_pin";
    case Step::kBatches: return "batches";
  }
  return "?";
}

inline std::uint64_t run_step(Step step, BufferPool& pool, Catalog& cat, std::size_t n) {
  const TableInfo info = *cat.find_table("events");
  const BTree tree(pool, info.btree_root, nullptr);
  const Heap heap(pool, info.heap_head, nullptr);
  std::uint64_t sum = 0;
  std::size_t k = 0;
  switch (step) {
    case Step::kWalk:
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) sum += c.key() ^ c.value();
      break;
    case Step::kZeroPage:
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) {
        const RID rid = rid_decode(c.value());
        Page pg;
        asm volatile("" : : "r"(pg.data()) : "memory");  // the zeroes must be written
        sum += pg.data()[rid.slot] + rid.slot;
      }
      break;
    case Step::kCopyPage:
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) {
        const RID rid = rid_decode(c.value());
        Page pg;
        pool.read_page(rid.page, pg);
        sum += pg.data()[rid.slot];
      }
      break;
    case Step::kCopyRecord:
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) {
        const std::vector<std::uint8_t> rec = heap.get(rid_decode(c.value()));
        sum += rec.size() + rec[1];
      }
      break;
    case Step::kDecode:
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) {
        const Row r = decode(info.schema, heap.get(rid_decode(c.value())));
        sum += static_cast<std::uint64_t>(r[2].i64);
      }
      break;
    case Step::kScanCopy:
    case Step::kScanPin: {
      cat.set_copy_pages(step == Step::kScanCopy);
      SeqScan scan(cat, TableRef{"events", ""});
      scan.open();
      Row r;
      while (k < n && scan.next(r)) {
        sum += static_cast<std::uint64_t>(r[2].i64);
        ++k;
      }
      scan.close();
      cat.set_copy_pages(false);
      break;
    }
    case Step::kPin:
    case Step::kDecodeInto: {
      PagePin pin;
      Row r;
      const RecordLayout layout(info.schema);
      for (Cursor c = tree.seek(0); c.valid() && k < n; c.next(), ++k) {
        const RID rid = rid_decode(c.value());
        if (!pin.holds(rid.page)) pin = PagePin(pool, rid.page);
        const auto rec = read_record(pin.page(), rid.slot);
        if (step == Step::kPin) {
          sum += rec->size() + (*rec)[1];
        } else {
          decode_into(layout, *rec, r);
          sum += static_cast<std::uint64_t>(r[2].i64);
        }
      }
      break;
    }
    case Step::kBatches: {
      SeqScan scan(cat, TableRef{"events", ""});
      scan.set_batch_rows(kDefaultBatchRows);
      scan.open();
      Batch b;
      while (k < n && scan.next_batch(b, n - k)) {
        const std::int64_t* w = b.cols[2].ints.data();
        for (std::size_t j = 0; j < b.count(); ++j) sum += static_cast<std::uint64_t>(w[j]);
        k += b.count();
      }
      scan.close();
      break;
    }
  }
  return sum;
}

}  // namespace labdb::test
