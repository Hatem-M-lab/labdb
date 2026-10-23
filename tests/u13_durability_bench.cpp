// Challenges 13.1 and 13.6: what durability costs.
//
// First the price of the one thing a durable commit must wait for: a write
// of 4 KiB and an fsync, on this machine's disk. Then INSERTs into an empty
// table through SQL, 2,000 each time into a fresh database: one transaction
// per INSERT under each Durability, and then 10, 100, 1,000 and all 2,000 to
// a transaction under kFull. Each configuration is run in turn, round after
// round (as tests/interleave.hpp does): medians of 5 rounds. Beside the
// times, what the log did per INSERT -- frames written, bytes into the log
// and into the database file, syncs -- which is the same on every run.
//
// Then recovery: a process that dies with a log of committed frames not yet
// checkpointed, and how long the next open takes to put them in the file.
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "io.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

const char* kPath = "u13_durability_bench.db";
constexpr int kInserts = 2000;
constexpr int kRounds = 5;

void wipe() {
  ::unlink(kPath);
  ::unlink((std::string(kPath) + "-wal").c_str());
}

std::string row(int i) {
  return "INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string((i * 7919) % 1000) +
         ", 'order " + std::to_string(i) + " for customer " + std::to_string(i % 97) + "')";
}

struct Config {
  const char* name;
  Durability d;
  int per_txn;
};

struct Cost {
  double seconds;
  std::uint64_t frames, pages, syncs;
};

Cost insert_run(const Config& c) {
  wipe();
  Pager pager(kPath);
  pager.set_durability(c.d);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  Executor e(cat);
  e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64, s TEXT)");
  const std::uint64_t f0 = pager.log_frames_written(), p0 = pager.pages_checkpointed(), s0 = pager.log_syncs();
  const double t0 = now_s();
  for (int i = 0; i < kInserts; ++i) {
    if (c.per_txn > 1 && i % c.per_txn == 0) e.run_sql("BEGIN");
    e.run_sql(row(i));
    if (c.per_txn > 1 && (i % c.per_txn == c.per_txn - 1 || i == kInserts - 1)) e.run_sql("COMMIT");
  }
  const double t = now_s() - t0;
  return {t, pager.log_frames_written() - f0, pager.pages_checkpointed() - p0, pager.log_syncs() - s0};
}

}  // namespace

int main() {
  // ---- a sync ----
  {
    const char* p = "u13_durability_bench.sync";
    ::unlink(p);
    const int fd = open_or_create(p);
    std::uint8_t page[kPageSize] = {};
    std::vector<double> t;
    for (int i = 0; i <= 200; ++i) {
      page[0] = static_cast<std::uint8_t>(i);
      const double t0 = now_s();
      pwrite_exact(fd, page, kPageSize, 0);
      sync_exact(fd);
      if (i) t.push_back(now_s() - t0);  // the first only warms
    }
    ::close(fd);
    ::unlink(p);
    std::sort(t.begin(), t.end());
    std::printf("\na 4 KiB write and an fsync, 200 times: median %.0f us (fastest %.0f, slowest %.0f)\n",
                t[t.size() / 2] * 1e6, t.front() * 1e6, t.back() * 1e6);
  }

  // ---- INSERTs, by durability and by transaction size ----
  const Config configs[] = {
      {"1 per transaction, kFull", Durability::kFull, 1},
      {"1 per transaction, kNormal", Durability::kNormal, 1},
      {"1 per transaction, kOff", Durability::kOff, 1},
      {"10 per transaction, kFull", Durability::kFull, 10},
      {"100 per transaction, kFull", Durability::kFull, 100},
      {"1,000 per transaction, kFull", Durability::kFull, 1000},
      {"all 2,000 in one, kFull", Durability::kFull, kInserts},
  };
  const std::size_t n = std::size(configs);
  std::vector<std::vector<double>> times(n);
  std::vector<Cost> last(n);
  for (std::size_t i = 0; i < n; ++i) insert_run(configs[i]);  // warm
  for (int r = 0; r < kRounds; ++r)
    for (std::size_t i = 0; i < n; ++i) {
      last[i] = insert_run(configs[i]);
      times[i].push_back(last[i].seconds);
    }
  std::printf("\n%d INSERTs into an empty table (id, v, s) through SQL, a fresh database each time;\n"
              "median of %d rounds (fastest-slowest), and what the log did per INSERT\n\n", kInserts, kRounds);
  std::printf("                                   us/INSERT        (range)   INSERTs/s   frames   into the log   into the file   syncs\n");
  for (std::size_t i = 0; i < n; ++i) {
    auto& v = times[i];
    std::sort(v.begin(), v.end());
    const double us = v[v.size() / 2] * 1e6 / kInserts;
    const Cost& c = last[i];
    std::printf("  %-30s %9.1f  (%6.1f-%6.1f)  %10.0f   %6.2f   %8.0f B     %8.0f B    %5.3f\n", configs[i].name, us,
                v.front() * 1e6 / kInserts, v.back() * 1e6 / kInserts, kInserts / v[v.size() / 2],
                static_cast<double>(c.frames) / kInserts, static_cast<double>(c.frames) * kFrameSize / kInserts,
                static_cast<double>(c.pages) * kPageSize / kInserts, static_cast<double>(c.syncs) / kInserts);
  }

  // ---- recovery: a log left behind by a process that died ----
  std::printf("\nrecovery: a process commits INSERTs (one transaction each, kOff, no checkpoint) until its log\n"
              "holds N frames, and dies; the next open reads the log and copies it into the file\n\n");
  std::printf("     frames in the log   commits   pages copied   open (ms)   per frame (us)\n");
  for (std::uint64_t want : {1000, 4000, 16000}) {
    wipe();
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
      Pager pager(kPath);
      pager.set_durability(Durability::kOff);
      pager.set_checkpoint_frames(~std::uint64_t{0});
      BufferPool pool(pager, 8192);
      Catalog cat(pool);
      Executor e(cat);
      e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64, s TEXT)");
      for (int i = 0; pager.log_frames() < want; ++i) e.run_sql(row(i));
      _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::vector<double> t;  // reopening is timed once: a second open finds nothing to do
    const double t0 = now_s();
    Pager pager(kPath);
    const double open_s = now_s() - t0;
    const RecoveryReport& r = pager.recovery();
    std::printf("  %20llu  %8llu  %13llu  %10.1f  %15.2f\n", static_cast<unsigned long long>(r.frames),
                static_cast<unsigned long long>(r.transactions), static_cast<unsigned long long>(r.pages),
                open_s * 1e3, open_s * 1e6 / static_cast<double>(r.frames));
  }
  wipe();
  return 0;
}
