// Challenge 1.4 measurement: what a page write costs without and with a
// durability barrier. This one table foreshadows the design of Unit 13.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "common.hpp"
#include "harness.hpp"
#include "io.hpp"
#include "page.hpp"

using namespace labdb;
using labdb::test::now_s;

int main() {
  const std::string path = "u01_fsync_bench.db";
  ::unlink(path.c_str());
  const int fd = open_or_create(path);

  Page page;
  page.set_id(1);
  page.set_type(PageType::kSlotted);
  // Land the page once so the loop below overwrites in place.
  pwrite_exact(fd, page.data(), kPageSize, 0);
  check_sys(::fsync(fd) == 0, "fsync");

  constexpr int kPlain = 20000, kSync = 500;

  double t0 = now_s();
  for (int i = 0; i < kPlain; ++i) pwrite_exact(fd, page.data(), kPageSize, 0);
  const double plain = (now_s() - t0) / kPlain;

  t0 = now_s();
  for (int i = 0; i < kSync; ++i) {
    pwrite_exact(fd, page.data(), kPageSize, 0);
    check_sys(::fdatasync(fd) == 0, "fdatasync");
  }
  const double fdatasync_each = (now_s() - t0) / kSync;

  t0 = now_s();
  for (int i = 0; i < kSync; ++i) {
    pwrite_exact(fd, page.data(), kPageSize, 0);
    check_sys(::fsync(fd) == 0, "fsync");
  }
  const double fsync_each = (now_s() - t0) / kSync;

  std::printf("| one 4 KiB page write | ops | us per op | vs plain |\n");
  std::printf("|---|---:|---:|---:|\n");
  std::printf("| pwrite only | %d | %.1f | 1.0x |\n", kPlain, plain * 1e6);
  std::printf("| pwrite + fdatasync | %d | %.1f | %.0fx |\n", kSync,
              fdatasync_each * 1e6, fdatasync_each / plain);
  std::printf("| pwrite + fsync | %d | %.1f | %.0fx |\n", kSync,
              fsync_each * 1e6, fsync_each / plain);

  ::close(fd);
  ::unlink(path.c_str());
  return 0;
}
