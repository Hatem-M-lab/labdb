#include "io.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>

#include "common.hpp"

namespace labdb {

int open_or_create(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  check_sys(fd >= 0, "open");
  return fd;
}

void pread_exact(int fd, void* buf, std::size_t n, off_t off) {
  auto* p = static_cast<std::uint8_t*>(buf);
  while (n > 0) {
    const ssize_t r = ::pread(fd, p, n, off);
    if (r < 0) {
      if (errno == EINTR) continue;  // interrupted before any byte moved
      check_sys(false, "pread");
    }
    check_that(r > 0, "pread hit end of file (truncated database?)");
    p += r;
    off += r;
    n -= static_cast<std::size_t>(r);
  }
}

void pwrite_exact(int fd, const void* buf, std::size_t n, off_t off) {
  const auto* p = static_cast<const std::uint8_t*>(buf);
  while (n > 0) {
    const ssize_t w = ::pwrite(fd, p, n, off);
    if (w < 0) {
      if (errno == EINTR) continue;
      check_sys(false, "pwrite");
    }
    check_that(w > 0, "pwrite wrote zero bytes and cannot make progress");
    p += w;
    off += w;
    n -= static_cast<std::size_t>(w);
  }
}

}  // namespace labdb
