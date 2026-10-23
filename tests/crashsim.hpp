#pragma once
// Crashes, simulated at a chosen write (Unit 13's tests). Not part of the
// engine: an IoHook (src/io.hpp) that counts every write and every fsync the
// engine makes, and at the k-th one either
//
//   * kills the process (kProcess) -- _exit before the k-th happens, so every
//     earlier write is in the file, as the kernel would keep it if the process
//     died; or
//   * cuts the power (kPower) -- lets the k-th write happen, then decides, for
//     every 512-byte sector written since its file was last fsynced, which of
//     its versions the disk had got to: the one at the last fsync, or the one
//     after any of the writes to it since, independently of every other
//     sector. That is what a disk may leave behind, a sector at a time: a
//     write lost whole, a page torn between its old and new halves, a later
//     write kept and an earlier one lost. Then _exit.
//
// Or, with kCount, only counts: how many writes and syncs a run makes.

#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include "io.hpp"

namespace labdb::test {

inline constexpr int kCrashExit = 77;  // the exit status of a simulated crash
inline constexpr std::size_t kSector = 512;

class CrashSim : public IoHook {
 public:
  enum class Mode { kCount, kProcess, kPower };
  CrashSim(Mode mode, std::uint64_t at, std::uint64_t seed) : mode_(mode), at_(at), rng_(seed) {}

  void before_write(int fd, const void* buf, std::size_t n, off_t off) override {
    const std::uint64_t k = ++events_;
    if (mode_ == Mode::kProcess && k == at_) _exit(kCrashExit);
    if (mode_ == Mode::kPower) {
      remember(fd, off, n);
      writes_[fd].push_back({off, std::vector<std::uint8_t>(static_cast<const std::uint8_t*>(buf),
                                                           static_cast<const std::uint8_t*>(buf) + n)});
      if (k == at_) {
        // the k-th write is issued, and the power fails some time after
        const auto* p = static_cast<const std::uint8_t*>(buf);
        for (std::size_t done = 0; done < n;) {
          const ssize_t w = ::pwrite(fd, p + done, n - done, off + static_cast<off_t>(done));
          if (w <= 0) break;
          done += static_cast<std::size_t>(w);
        }
        power_cut();
      }
    }
  }
  void before_sync(int fd) override {
    const std::uint64_t k = ++events_;
    if (k == at_) {
      if (mode_ == Mode::kProcess) _exit(kCrashExit);
      if (mode_ == Mode::kPower) power_cut();  // before the sync is done: nothing more is safe
    }
    pre_.erase(fd);  // past this sync, what is in the file stays
    writes_.erase(fd);
  }
  std::uint64_t events() const { return events_; }

 private:
  struct Write {
    off_t off;
    std::vector<std::uint8_t> data;
  };
  // The content of every sector this write touches, as of the last sync --
  // read before the first write to it since then.
  void remember(int fd, off_t off, std::size_t n) {
    auto& pre = pre_[fd];
    const std::uint64_t first = static_cast<std::uint64_t>(off) / kSector;
    const std::uint64_t last = (static_cast<std::uint64_t>(off) + n - 1) / kSector;
    for (std::uint64_t s = first; s <= last; ++s) {
      if (pre.count(s)) continue;
      std::vector<std::uint8_t> old(kSector, 0);  // past the end of the file: zeroes
      ssize_t r = ::pread(fd, old.data(), kSector, static_cast<off_t>(s * kSector));
      (void)r;
      pre[s] = std::move(old);
    }
  }
  [[noreturn]] void power_cut() {
    for (auto& [fd, pre] : pre_) {
      const auto& ws = writes_[fd];
      for (auto& [s, image] : pre) {
        // the versions of this sector: before any write, then after each one
        std::vector<std::vector<std::uint8_t>> versions{image};
        std::vector<std::uint8_t> cur = image;
        const off_t lo = static_cast<off_t>(s * kSector), hi = lo + static_cast<off_t>(kSector);
        for (const Write& w : ws) {
          const off_t a = std::max(lo, w.off), b = std::min(hi, w.off + static_cast<off_t>(w.data.size()));
          if (a >= b) continue;
          std::memcpy(cur.data() + (a - lo), w.data.data() + (a - w.off), static_cast<std::size_t>(b - a));
          versions.push_back(cur);
        }
        const auto& keep = versions[rng_() % versions.size()];
        ssize_t r = ::pwrite(fd, keep.data(), kSector, lo);
        (void)r;
      }
    }
    _exit(kCrashExit);
  }

  Mode mode_;
  std::uint64_t at_;
  std::mt19937_64 rng_;
  std::uint64_t events_ = 0;
  std::map<int, std::map<std::uint64_t, std::vector<std::uint8_t>>> pre_;  // fd -> sector -> as of the last sync
  std::map<int, std::vector<Write>> writes_;                               // fd -> writes since the last sync
};

}  // namespace labdb::test
