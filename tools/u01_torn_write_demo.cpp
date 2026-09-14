// THE BUG, preserved for study. This program writes a page the way naive
// storage code does: one pwrite, return value ignored. To make the
// kernel's "write() may transfer fewer bytes" clause fire on demand, it
// caps its own file size with RLIMIT_FSIZE first -- the short write below
// is performed by the kernel, not simulated.
//
// usage: u01_torn_write_demo <path>   (then hexdump the file)
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <path>\n", argv[0]);
    return 2;
  }

  const int fd = ::open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    std::perror("open");
    return 1;
  }

  unsigned char old_page[4096], new_page[4096];
  std::memset(old_page, 0x11, sizeof old_page);  // version 1 of the page
  std::memset(new_page, 0x22, sizeof new_page);  // version 2

  // Version 1 lands on disk, fully and durably.
  if (::pwrite(fd, old_page, sizeof old_page, 0) != 4096 || ::fsync(fd) != 0) {
    std::perror("setup write");
    return 1;
  }
  std::printf("setup: page written with 0x11 and fsynced\n");

  // The trap: from here on, the kernel will not let this file grow past
  // byte 2048. A write crossing that line is cut short -- the same
  // behavior POSIX permits for signals, disk-full, and quota.
  ::signal(SIGXFSZ, SIG_IGN);
  struct rlimit rl = {2048, 2048};
  ::setrlimit(RLIMIT_FSIZE, &rl);

  // ---- the buggy write: return value dropped on the floor ----
  ssize_t n = ::pwrite(fd, new_page, sizeof new_page, 0);
  std::printf("pwrite asked to write 4096 bytes... returned %zd\n", n);
  std::printf("buggy pager: ignores that number, reports the page as written\n");

  ::close(fd);
  return 0;
}
