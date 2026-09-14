// Challenge 1.6: the measurement pages exist for.
// One million 64-byte records, stored two ways:
//   u01_flat.bin  : record i at byte offset i*64 -- "the obvious design"
//   u01_paged.db  : the same records packed into labdb slotted pages
// Read them back sequentially and randomly, cold cache and warm, and see
// what a syscall per record actually costs.
//
// Cold cache = posix_fadvise(POSIX_FADV_DONTNEED) on the file right before
// the run: the kernel evicts the file's pages, so reads go to the device.
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "common.hpp"
#include "harness.hpp"
#include "io.hpp"
#include "page.hpp"
#include "pager.hpp"
#include "slotted_page.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

constexpr std::uint32_t kRecords = 1000000;
constexpr std::size_t kRecSize = 64;
constexpr std::uint64_t kExpectedSum = 499999500000ULL;  // sum of 0..999999

void fill_record(std::uint8_t* out, std::uint32_t id) {
  store_u32(out, id);
  for (std::size_t i = 4; i < kRecSize; ++i)
    out[i] = static_cast<std::uint8_t>(id * 2654435761u + i);
}

void drop_cache(int fd) {
  check_sys(::fsync(fd) == 0, "fsync");
  const int rc = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
  check_that(rc == 0, "posix_fadvise(DONTNEED) failed");
}

// ---- builders --------------------------------------------------------

int build_flat(const std::string& path) {
  ::unlink(path.c_str());
  const int fd = open_or_create(path);
  std::vector<std::uint8_t> chunk(kPageSize);
  constexpr std::uint32_t per_chunk = kPageSize / kRecSize;  // 64
  off_t off = 0;
  for (std::uint32_t id = 0; id < kRecords;) {
    std::size_t used = 0;
    for (std::uint32_t j = 0; j < per_chunk && id < kRecords; ++j, ++id) {
      fill_record(chunk.data() + used, id);
      used += kRecSize;
    }
    pwrite_exact(fd, chunk.data(), used, off);
    off += static_cast<off_t>(used);
  }
  check_sys(::fsync(fd) == 0, "fsync");
  return fd;
}

// Returns records per page (needed to locate record i later).
std::uint32_t build_paged(Pager& pager) {
  Page page;
  SlottedPage view(page);
  PageId pid = pager.allocate_page();
  REQUIRE(pid == 1);
  view.init(pid);
  std::uint8_t rec[kRecSize];
  std::uint32_t per_page = 0;
  for (std::uint32_t id = 0; id < kRecords; ++id) {
    fill_record(rec, id);
    auto slot = view.insert(std::span<const std::uint8_t>(rec, kRecSize));
    if (!slot) {
      pager.write_page(pid, page);
      if (per_page == 0) per_page = view.slot_count();
      pid = pager.allocate_page();
      view.init(pid);
      slot = view.insert(std::span<const std::uint8_t>(rec, kRecSize));
      REQUIRE(slot.has_value());
    }
    // Packing is deterministic: record id lives at (1 + id/per_page,
    // id % per_page). The random reader below re-verifies this per read.
    if (per_page != 0) REQUIRE(*slot == id % per_page);
  }
  pager.write_page(pid, page);
  pager.sync();
  return per_page;
}

// ---- readers ---------------------------------------------------------

std::uint64_t scan_flat_per_record(int fd) {
  std::uint8_t buf[kRecSize];
  std::uint64_t sum = 0;
  for (std::uint32_t id = 0; id < kRecords; ++id) {
    pread_exact(fd, buf, kRecSize, static_cast<off_t>(id) * kRecSize);
    sum += load_u32(buf);
  }
  return sum;
}

std::uint64_t scan_flat_chunked(int fd) {
  std::uint8_t buf[kPageSize];
  std::uint64_t sum = 0;
  constexpr std::uint32_t per_chunk = kPageSize / kRecSize;
  constexpr std::uint32_t chunks = kRecords / per_chunk;
  for (std::uint32_t c = 0; c < chunks; ++c) {
    pread_exact(fd, buf, kPageSize, static_cast<off_t>(c) * kPageSize);
    for (std::uint32_t j = 0; j < per_chunk; ++j)
      sum += load_u32(buf + j * kRecSize);
  }
  return sum;
}

std::uint64_t scan_paged(Pager& pager) {
  Page page;
  std::uint64_t sum = 0;
  const std::uint32_t pages = pager.page_count();
  for (PageId id = 1; id < pages; ++id) {
    pager.read_page(id, page);
    SlottedPage view(page);
    const std::uint16_t n = view.slot_count();
    for (std::uint16_t s = 0; s < n; ++s) {
      auto rec = view.read(s);
      sum += load_u32(rec->data());
    }
  }
  return sum;
}

std::uint64_t random_flat(int fd, const std::vector<std::uint32_t>& ids) {
  std::uint8_t buf[kRecSize];
  std::uint64_t sum = 0;
  for (std::uint32_t id : ids) {
    pread_exact(fd, buf, kRecSize, static_cast<off_t>(id) * kRecSize);
    REQUIRE(load_u32(buf) == id);  // right record, verified
    sum += load_u32(buf);
  }
  return sum;
}

std::uint64_t random_paged(Pager& pager, std::uint32_t per_page,
                           const std::vector<std::uint32_t>& ids) {
  Page page;
  std::uint64_t sum = 0;
  for (std::uint32_t id : ids) {
    const PageId pid = 1 + id / per_page;
    const auto slot = static_cast<std::uint16_t>(id % per_page);
    pager.read_page(pid, page);
    SlottedPage view(page);
    auto rec = view.read(slot);
    REQUIRE(rec.has_value());
    REQUIRE(load_u32(rec->data()) == id);  // locator verified per read
    sum += load_u32(rec->data());
  }
  return sum;
}

}  // namespace

int main() {
  const std::string flat_path = "u01_flat.bin";
  const std::string paged_path = "u01_paged.db";

  std::printf("building %u records of %zu bytes each...\n\n", kRecords,
              kRecSize);
  double t0 = now_s();
  const int flat_fd = build_flat(flat_path);
  const double t_flat = now_s() - t0;

  ::unlink(paged_path.c_str());
  t0 = now_s();
  Pager pager(paged_path);
  const std::uint32_t per_page = build_paged(pager);
  const double t_paged = now_s() - t0;
  // Second fd on the same file, used only to evict its cache pages.
  const int paged_fd = open_or_create(paged_path);

  const std::uint32_t data_pages = pager.page_count() - 1;
  std::printf("u01_flat.bin : records at fixed 64 B offsets, %.1f MiB, built in %.2f s\n",
              double(kRecords) * kRecSize / (1024.0 * 1024.0), t_flat);
  std::printf("u01_paged.db : %u records/page, %u data pages, %.1f MiB, built in %.2f s\n\n",
              per_page, data_pages,
              double(pager.page_count()) * kPageSize / (1024.0 * 1024.0),
              t_paged);

  // ---- sequential scan of all records --------------------------------
  std::printf("sequential scan of all %u records:\n\n", kRecords);
  std::printf("| layout | read granularity | syscalls | cold (s) | cold ns/rec | warm (s) | warm ns/rec |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|\n");

  double a_warm, b_warm, c_warm;
  {
    drop_cache(flat_fd);
    t0 = now_s();
    std::uint64_t s = scan_flat_per_record(flat_fd);
    const double cold = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    t0 = now_s();
    s = scan_flat_per_record(flat_fd);
    a_warm = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    std::printf("| flat | 64 B, one pread per record | %u | %.3f | %.0f | %.3f | %.0f |\n",
                kRecords, cold, cold / kRecords * 1e9, a_warm,
                a_warm / kRecords * 1e9);
  }
  {
    drop_cache(paged_fd);
    t0 = now_s();
    std::uint64_t s = scan_paged(pager);
    const double cold = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    t0 = now_s();
    s = scan_paged(pager);
    b_warm = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    std::printf("| paged (labdb) | 4 KiB, one pread per page | %u | %.3f | %.0f | %.3f | %.0f |\n",
                data_pages, cold, cold / kRecords * 1e9, b_warm,
                b_warm / kRecords * 1e9);
  }
  {
    drop_cache(flat_fd);
    t0 = now_s();
    std::uint64_t s = scan_flat_chunked(flat_fd);
    const double cold = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    t0 = now_s();
    s = scan_flat_chunked(flat_fd);
    c_warm = now_s() - t0;
    REQUIRE(s == kExpectedSum);
    std::printf("| flat | 4 KiB, one pread per chunk | %u | %.3f | %.0f | %.3f | %.0f |\n",
                kRecords / (unsigned)(kPageSize / kRecSize), cold,
                cold / kRecords * 1e9, c_warm, c_warm / kRecords * 1e9);
  }
  std::printf("\nwarm sequential, per-record syscalls vs paged: %.1fx slower\n",
              a_warm / b_warm);
  std::printf("warm sequential, paged vs raw 4 KiB chunks: %.2fx (the price of structure)\n\n",
              b_warm / c_warm);

  // ---- random point reads --------------------------------------------
  std::mt19937 rng(7);
  std::uniform_int_distribution<std::uint32_t> id_dist(0, kRecords - 1);
  std::vector<std::uint32_t> warm_ids(300000), cold_ids(30000);
  for (auto& id : warm_ids) id = id_dist(rng);
  for (auto& id : cold_ids) id = id_dist(rng);

  std::printf("random point reads (record ids drawn uniformly, fixed seed):\n\n");
  std::printf("| layout | bytes moved per read | state | reads | us per read |\n");
  std::printf("|---|---|---|---:|---:|\n");

  // Warm first: both files are fully cached from the sequential scans.
  t0 = now_s();
  std::uint64_t s = random_flat(flat_fd, warm_ids);
  const double a_rw = now_s() - t0;
  labdb::test::do_not_optimize(s);
  t0 = now_s();
  s = random_paged(pager, per_page, warm_ids);
  const double b_rw = now_s() - t0;
  labdb::test::do_not_optimize(s);

  // Cold: evict both files, then read a smaller batch.
  drop_cache(flat_fd);
  t0 = now_s();
  s = random_flat(flat_fd, cold_ids);
  const double a_rc = now_s() - t0;
  labdb::test::do_not_optimize(s);
  drop_cache(paged_fd);
  t0 = now_s();
  s = random_paged(pager, per_page, cold_ids);
  const double b_rc = now_s() - t0;
  labdb::test::do_not_optimize(s);

  std::printf("| flat | 64 B | cold | %zu | %.2f |\n", cold_ids.size(),
              a_rc / double(cold_ids.size()) * 1e6);
  std::printf("| paged (labdb) | 4096 B | cold | %zu | %.2f |\n",
              cold_ids.size(), b_rc / double(cold_ids.size()) * 1e6);
  std::printf("| flat | 64 B | warm | %zu | %.2f |\n", warm_ids.size(),
              a_rw / double(warm_ids.size()) * 1e6);
  std::printf("| paged (labdb) | 4096 B | warm | %zu | %.2f |\n",
              warm_ids.size(), b_rw / double(warm_ids.size()) * 1e6);
  std::printf("\nrandom point reads, paged vs flat, warm: %.2fx\n", b_rw / a_rw);

  // The two files are left on disk deliberately: inspect them with od,
  // filefrag, or ls -ls. `make clean` removes them.
  ::close(flat_fd);
  ::close(paged_fd);
  return 0;
}
