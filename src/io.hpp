#pragma once
// Exact-or-die file I/O. POSIX read/write/pread/pwrite are allowed to
// transfer fewer bytes than asked -- a fact most programs ignore and most
// programs get away with, until they don't (see the Unit 1 forensic trap).
// The engine only ever does I/O through these functions.
// Introduced in Challenge 1.4; sync_exact, truncate_exact and file_size in
// Unit 13, with the hook below.

#include <sys/types.h>

#include <cstddef>
#include <string>

namespace labdb {

// Open (creating if needed) a file for read/write. Throws on failure.
int open_or_create(const std::string& path);

// Read exactly n bytes at offset off, retrying on partial reads and EINTR.
// Throws if the file ends early or the read fails.
void pread_exact(int fd, void* buf, std::size_t n, off_t off);

// Write exactly n bytes at offset off, retrying on partial writes and
// EINTR. Throws if the write fails. A short write that cannot make
// progress becomes a loud error, never silent corruption.
void pwrite_exact(int fd, const void* buf, std::size_t n, off_t off);

// Unit 13: fsync, or die. Until it returns, nothing written to `fd` is
// promised to survive a power cut -- only a crash of the process.
void sync_exact(int fd);

// Unit 13: set the file's length (growing it with zeroes, or cutting it).
void truncate_exact(int fd, off_t size);

// Unit 13: the file's length in bytes.
off_t file_size(int fd);

// Unit 13: an observer of every write and every sync, called just before
// each one happens -- the seam through which the crash tests of Challenge
// 13.4 stop the engine at any write it makes, or cut its power there. The
// engine itself installs none; with no hook, the cost is one test of a
// pointer per call.
class IoHook {
 public:
  virtual ~IoHook() = default;
  virtual void before_write(int fd, const void* buf, std::size_t n, off_t off) = 0;
  virtual void before_sync(int fd) = 0;
};
void set_io_hook(IoHook* hook);  // nullptr removes it

}  // namespace labdb
