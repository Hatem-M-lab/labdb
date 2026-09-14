#pragma once
// Exact-or-die file I/O. POSIX read/write/pread/pwrite are allowed to
// transfer fewer bytes than asked -- a fact most programs ignore and most
// programs get away with, until they don't (see the Unit 1 forensic trap).
// The engine only ever does I/O through these two functions.
// Introduced in Challenge 1.4.

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

}  // namespace labdb
