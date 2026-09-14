// Forensic-trap regression test: under an induced short-write condition
// (RLIMIT_FSIZE), pwrite_exact must raise a loud error -- never report
// success for a page it could not fully write.
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "common.hpp"
#include "harness.hpp"
#include "io.hpp"

using namespace labdb;

int main() {
  const std::string path = "u01_torn_write_test.img";
  ::unlink(path.c_str());
  const int fd = open_or_create(path);

  std::uint8_t old_page[kPageSize], new_page[kPageSize];
  std::memset(old_page, 0x11, sizeof old_page);
  std::memset(new_page, 0x22, sizeof new_page);
  pwrite_exact(fd, old_page, kPageSize, 0);
  check_sys(::fsync(fd) == 0, "fsync");

  // Rig the trap: cap the file size mid-page. The kernel itself will now
  // perform a genuine short write. Ignore SIGXFSZ so we observe the return
  // value instead of dying.
  ::signal(SIGXFSZ, SIG_IGN);
  struct rlimit rl{2048, 2048};
  check_sys(::setrlimit(RLIMIT_FSIZE, &rl) == 0, "setrlimit");

  bool caught = false;
  try {
    pwrite_exact(fd, new_page, kPageSize, 0);
  } catch (const std::exception& e) {
    caught = true;
    std::printf("pwrite_exact raised as required: %s\n", e.what());
  }
  REQUIRE(caught);

  std::printf(
      "u01_torn_write_test: PASS (short write became a loud error, not "
      "silent corruption)\n");
  ::close(fd);
  ::unlink(path.c_str());
  return 0;
}
