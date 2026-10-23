// Challenge 13.5, the forensic trap: the log that remembered too much.
//
// A checkpoint copies the log's pages into the database file and starts the
// log again from its first frame. The first version of this unit's log did
// that by moving its write position back to the start and nothing more:
// the file was not cut -- cutting and re-growing it costs a sync of its own
// every time -- and the old frames past the new end were left to be
// overwritten as the new ones arrived. Every frame has a checksum, and
// recovery stops at the first frame that fails it, so the old frames looked
// harmless. They were not: each of them is a perfectly valid frame. After a
// crash, recovery read the new frames, then went straight on into the old
// ones, and replayed them as committed transactions on top of the new.
//
// This program runs the same history twice -- with that first version
// (Pager::demo_keep_log_generation) and with the engine as it is -- in a
// child process that dies without closing anything, and reopens each.
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "io.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

constexpr int kBefore = 400, kAfter = 10;

struct Seen {
  RecoveryReport rec;
  std::int64_t payments = 0, cents = 0, after_found = 0;
  long long log_bytes = 0;
};

Seen history(bool first_version) {
  const std::string path = first_version ? "u13_stale_log_demo_a.db" : "u13_stale_log_demo_b.db";
  ::unlink(path.c_str());
  ::unlink((path + "-wal").c_str());
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    Pager pager(path);
    pager.set_checkpoint_frames(1'000'000);  // only the checkpoint asked for below
    if (first_version) pager.demo_keep_log_generation();
    BufferPool pool(pager, 256);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE payments (id INT64 NOT NULL, cents INT64, memo TEXT)");
    for (int i = 0; i < kBefore + kAfter; ++i) {
      if (i == kBefore) pool.sync();  // the checkpoint: the database file now holds 400 payments
      e.run_sql("INSERT INTO payments VALUES (" + std::to_string(i) + ", " + std::to_string(100 + i) +
                ", 'invoice " + std::to_string(i) + "')");
    }
    _exit(0);  // 410 payments committed -- and the process dies, closing nothing
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  Seen s;
  {
    const int fd = open_or_create(path + "-wal");
    s.log_bytes = static_cast<long long>(file_size(fd));
    ::close(fd);
  }
  Pager pager(path);
  s.rec = pager.recovery();
  BufferPool pool(pager, 256);
  Catalog cat(pool);
  Executor e(cat);
  const ResultSet r = e.run_sql("SELECT COUNT(*), SUM(cents) FROM payments");
  s.payments = r.rows[0][0].i64;
  s.cents = r.rows[0][1].i64;
  s.after_found = static_cast<std::int64_t>(
      e.run_sql("SELECT id FROM payments WHERE id >= " + std::to_string(kBefore)).row_count());
  return s;
}

}  // namespace

int main() {
  std::printf("a ledger: CREATE TABLE payments, %d INSERTs, a checkpoint, %d more INSERTs --\n"
              "every one committed under Durability::kFull -- and then the process dies\n\n",
              kBefore, kAfter);
  const Seen a = history(true), b = history(false);
  const long long want_cents = 100LL * (kBefore + kAfter) + (kBefore + kAfter - 1LL) * (kBefore + kAfter) / 2;
  std::printf("                                  the log reset as first written     the engine's log\n");
  std::printf("  the log file                    %10lld bytes                  %10lld bytes\n", a.log_bytes, b.log_bytes);
  std::printf("  recovery read                   %6llu frames, %4llu commits       %6llu frames, %4llu commits\n",
              static_cast<unsigned long long>(a.rec.frames), static_cast<unsigned long long>(a.rec.transactions),
              static_cast<unsigned long long>(b.rec.frames), static_cast<unsigned long long>(b.rec.transactions));
  std::printf("  and stopped at                  %-34s %s\n", a.rec.stopped.c_str(), b.rec.stopped.c_str());
  std::printf("  payments in the database        %10lld                         %10lld\n",
              static_cast<long long>(a.payments), static_cast<long long>(b.payments));
  std::printf("  their cents                     %10lld                         %10lld   (committed: %lld)\n",
              static_cast<long long>(a.cents), static_cast<long long>(b.cents), want_cents);
  std::printf("  the %d after the checkpoint      %10lld found                   %10lld found\n", kAfter,
              static_cast<long long>(a.after_found), static_cast<long long>(b.after_found));
  const bool lost = a.payments != kBefore + kAfter;
  const bool kept = b.payments == kBefore + kAfter && b.cents == want_cents && b.after_found == kAfter;
  std::printf("\n%s\n", lost && kept ? "u13_stale_log_demo: the first version loses committed payments; the engine keeps them"
                                     : "u13_stale_log_demo: UNEXPECTED");
  ::unlink("u13_stale_log_demo_a.db");
  ::unlink("u13_stale_log_demo_a.db-wal");
  ::unlink("u13_stale_log_demo_b.db");
  ::unlink("u13_stale_log_demo_b.db-wal");
  return lost && kept ? 0 : 1;
}
