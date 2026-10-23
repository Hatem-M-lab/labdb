#include "wal.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>

#include "common.hpp"
#include "io.hpp"

namespace labdb {

namespace {

constexpr char kWalMagic[8] = {'l', 'a', 'b', 'd', 'b', 'w', 'a', 'l'};
constexpr std::uint32_t kWalVersion = 1;
constexpr std::uint64_t kP1 = 0x9E3779B185EBCA87ULL, kP2 = 0xC2B2AE3D27D4EB4FULL,
                        kP3 = 0x165667B19E3779F9ULL, kP4 = 0x85EBCA77C2B2AE63ULL;

inline std::uint64_t rotl64(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
inline std::uint64_t round64(std::uint64_t acc, std::uint64_t w) {
  acc += w * kP2;
  acc = rotl64(acc, 31);
  return acc * kP1;
}

std::uint64_t fresh_generation() {
  std::random_device rd;
  return (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ static_cast<std::uint64_t>(::getpid());
}

}  // namespace

std::uint64_t checksum64(const std::uint8_t* p, std::size_t n, std::uint64_t seed) {
  check_that(n % 8 == 0, "checksum64: length must be a multiple of 8");
  std::uint64_t v[4] = {seed + kP1 + kP2, seed + kP2, seed, seed - kP1};
  std::size_t i = 0;
  for (; i + 32 <= n; i += 32)
    for (int l = 0; l < 4; ++l) v[l] = round64(v[l], load_u64(p + i + 8 * l));
  std::uint64_t h = rotl64(v[0], 1) + rotl64(v[1], 7) + rotl64(v[2], 12) + rotl64(v[3], 18);
  for (; i < n; i += 8) h = rotl64(h ^ round64(0, load_u64(p + i)), 27) * kP1 + kP4;
  h += n;
  h ^= h >> 33;
  h *= kP2;
  h ^= h >> 29;
  h *= kP3;
  h ^= h >> 32;
  return h;
}

Wal::Wal(const std::string& path, std::uint64_t db_id) : db_id_(db_id) {
  fd_ = open_or_create(path);
  if (file_size(fd_) >= static_cast<off_t>(kWalHeaderSize)) {
    std::uint8_t h[kWalHeaderSize];
    pread_exact(fd_, h, sizeof h, 0);
    header_ok_ = std::memcmp(h, kWalMagic, 8) == 0 && load_u32(h + 8) == kWalVersion &&
                 load_u32(h + 12) == kPageSize && load_u64(h + 24) == db_id_ &&
                 load_u64(h + 32) == checksum64(h, 32, 0);
    if (header_ok_) generation_ = load_u64(h + 16);
  }
  // No usable header: whatever frames follow belong to no log of this
  // database. A fresh generation makes sure none of them is ever read.
  if (!header_ok_) generation_ = fresh_generation();
}

Wal::~Wal() {
  if (fd_ >= 0) ::close(fd_);
}

void Wal::write_header() {
  std::uint8_t h[kWalHeaderSize] = {};
  std::memcpy(h, kWalMagic, 8);
  store_u32(h + 8, kWalVersion);
  store_u32(h + 12, kPageSize);
  store_u64(h + 16, generation_);
  store_u64(h + 24, db_id_);
  store_u64(h + 32, checksum64(h, 32, 0));
  pwrite_exact(fd_, h, sizeof h, 0);
  header_ok_ = true;
}

void Wal::write_frame(std::uint64_t slot, PageId id, std::uint32_t flags, const Page& page) {
  if (!header_ok_) write_header();
  std::uint8_t buf[kFrameSize];
  store_u32(buf + 0, id);
  store_u32(buf + 4, flags);
  store_u64(buf + 8, generation_);
  std::memcpy(buf + kFrameHeaderSize, page.data(), kPageSize);
  store_u64(buf + 16, checksum64(buf + kFrameHeaderSize, kPageSize, checksum64(buf, 16, db_id_)));
  pwrite_exact(fd_, buf, sizeof buf, slot_offset(slot));
  ++frames_written_;
}

RecoveryReport Wal::recover() {
  RecoveryReport r;
  committed_.clear();
  pending_.clear();
  end_ = commit_end_ = 0;
  if (!header_ok_) return r;  // nothing of this database's to recover
  r.found = true;
  const off_t size = file_size(fd_);
  std::uint8_t buf[kFrameSize];
  r.stopped = "end of log";
  for (std::uint64_t slot = 0;; ++slot) {
    const off_t at = slot_offset(slot);
    if (at + static_cast<off_t>(kFrameSize) > size) {
      if (at < size) r.stopped = "a frame cut short at slot " + std::to_string(slot);
      break;
    }
    pread_exact(fd_, buf, sizeof buf, at);
    if (load_u64(buf + 8) != generation_) {
      r.stopped = "a frame of another generation at slot " + std::to_string(slot);
      break;
    }
    if (load_u64(buf + 16) !=
        checksum64(buf + kFrameHeaderSize, kPageSize, checksum64(buf, 16, db_id_))) {
      r.stopped = "a bad checksum at slot " + std::to_string(slot);
      break;
    }
    ++r.frames;
    const PageId id = load_u32(buf + 0);
    pending_[id] = slot;
    if (load_u32(buf + 4) & kFrameCommit) {
      for (const auto& [page, s] : pending_) committed_[page] = s;
      pending_.clear();
      commit_end_ = slot + 1;
      ++r.transactions;
    }
  }
  r.discarded = r.frames - commit_end_;
  r.pages = committed_.size();
  pending_.clear();
  end_ = commit_end_;  // a cut-off transaction's frames are overwritten by the next one
  return r;
}

bool Wal::read(PageId id, Page& out) {
  auto it = pending_.find(id);
  if (it == pending_.end()) {
    it = committed_.find(id);
    if (it == committed_.end()) return false;
  }
  read_frame(it->second, out);
  return true;
}

void Wal::read_frame(std::uint64_t slot, Page& out) {
  pread_exact(fd_, out.data(), kPageSize, slot_offset(slot) + static_cast<off_t>(kFrameHeaderSize));
}

void Wal::write(PageId id, const Page& page) {
  auto it = pending_.find(id);
  const std::uint64_t slot = it != pending_.end() ? it->second : end_++;
  write_frame(slot, id, 0, page);
  pending_[id] = slot;
}

void Wal::commit(const Page& meta, bool sync) {
  const std::uint64_t slot = end_++;
  write_frame(slot, 0, kFrameCommit, meta);
  for (const auto& [page, s] : pending_) committed_[page] = s;
  committed_[0] = slot;
  pending_.clear();
  commit_end_ = end_;
  if (sync) Wal::sync();
}

void Wal::rollback() {
  pending_.clear();
  end_ = commit_end_;
}

std::vector<std::pair<PageId, std::uint64_t>> Wal::committed_frames() const {
  std::vector<std::pair<PageId, std::uint64_t>> v(committed_.begin(), committed_.end());
  std::sort(v.begin(), v.end());
  return v;
}

void Wal::reset() {
  check_that(pending_.empty(), "log reset inside a transaction");
  committed_.clear();
  end_ = commit_end_ = 0;
  if (!keep_generation_) ++generation_;
  write_header();
}

void Wal::truncate() {
  check_that(pending_.empty(), "log truncated inside a transaction");
  committed_.clear();
  end_ = commit_end_ = 0;
  truncate_exact(fd_, 0);
  header_ok_ = false;
  if (!keep_generation_) ++generation_;
}

void Wal::sync() {
  sync_exact(fd_);
  ++syncs_;
}

}  // namespace labdb
