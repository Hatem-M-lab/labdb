// Challenge 13.4: crash the engine on purpose, at every write it makes.
//
// The workload of tests/u13_workload.hpp runs in a child process, which
// reports through a pipe each transaction it has finished: an
// acknowledgement, what a client would have been told. The child is stopped
// by tests/crashsim.hpp at its k-th write or sync -- for every k, once as a
// process that dies there and once as a power cut there, which keeps,
// sector by sector, any version of what was written since the last fsync --
// and also by kill -9 at random moments. Then the parent opens the database,
// which recovers it, and asks which prefix of the workload it holds: what each
// prefix must hold comes from a model of the rows (tests/u13_workload.hpp),
// not from the engine -- and the engine, run without a crash, is first held
// to the model after every transaction.
//
// The rule, under Durability::kFull: the database holds exactly the
// transactions acknowledged, or those and the one in flight -- never less,
// never a part of one, never anything else; and every index agrees with its
// table. Under kNormal and kOff a power cut may lose acknowledged
// transactions (that is what they trade for speed), but what remains must
// still be some prefix, whole. Under kOff, which never syncs, only a process
// crash is survivable: a power cut may leave the database damaged, and the
// test counts how often it does. A process crash loses nothing in any mode.
//
//   u13_crash_test [STRIDE]   crash at every STRIDE-th write (default 1: all)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "crashsim.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "pager.hpp"
#include "u13_workload.hpp"

using namespace labdb;
using namespace labdb::test;

namespace {

const char* kPath = "u13_crash_test.db";
constexpr std::size_t kPoolFrames = 24;       // small: pages spill into the log before they commit
constexpr std::uint64_t kCheckpointAt = 64;   // small: a checkpoint every few transactions

const Workload kWorkload = crash_workload();
const std::vector<Txn>& kWork = kWorkload.txns;

void fresh_database() {
  ::unlink(kPath);
  ::unlink((std::string(kPath) + "-wal").c_str());
  Pager pager(kPath);
  BufferPool pool(pager, kPoolFrames);
  Catalog cat(pool);  // the empty catalog, committed: the database exists
}

// The workload, in this process, through `hook`; acks into `ack`.
void run(Durability d, IoHook* hook, const std::function<void(int)>& ack) {
  set_io_hook(hook);
  Pager pager(kPath);
  pager.set_durability(d);
  pager.set_checkpoint_frames(kCheckpointAt);
  BufferPool pool(pager, kPoolFrames);
  Catalog cat(pool);
  Executor e(cat);
  run_workload(e, kWork, ack);
  _exit(0);  // done: and no destructor runs, as if the power failed just after
}

struct Outcome {
  int acked = 0;      // the last transaction the child acknowledged
  int holds = -1;     // the prefix the recovered database holds (-1: none)
  RecoveryReport rec;
  std::string digest;
  std::string error;  // recovery or reading the database threw this
};

const std::vector<std::string>& g_expected = kWorkload.expected;  // [i]: after the first i

Outcome crash_once(Durability d, CrashSim::Mode mode, std::uint64_t at, std::uint64_t kill_after_us) {
  fresh_database();
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(fds[0]);
    CrashSim sim(mode, at, at * 7919 + 13);
    run(d, mode == CrashSim::Mode::kCount ? nullptr : &sim, [&](int i) {
      if (::write(fds[1], &i, sizeof i) != sizeof i) _exit(3);
    });
  }
  ::close(fds[1]);
  if (kill_after_us) {
    ::usleep(static_cast<useconds_t>(kill_after_us));
    ::kill(pid, SIGKILL);
  }
  Outcome o;
  int i;
  while (::read(fds[0], &i, sizeof i) == sizeof i) o.acked = i;
  ::close(fds[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  const bool ok = (WIFEXITED(status) && (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == kCrashExit)) ||
                  (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  REQUIRE(ok);
  // recover, and see what is there
  try {
    Pager pager(kPath);
    o.rec = pager.recovery();
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    o.digest = digest(e, cat);
    pager.freelist_length();  // walks the free list: a cycle or a stray page throws
  } catch (const std::exception& x) {
    o.error = x.what();
    return o;
  }
  for (int j = 0; j < static_cast<int>(g_expected.size()); ++j)
    if (g_expected[static_cast<std::size_t>(j)] == o.digest &&
        (o.holds < 0 || std::abs(j - o.acked) < std::abs(o.holds - o.acked)))
      o.holds = j;
  return o;
}

struct Tally {
  int runs = 0, at_ack = 0, in_flight = 0, lost_runs = 0, max_lost = 0, wrong = 0, damaged = 0;
  int torn = 0, other_gen = 0, cut_short = 0, tails = 0;
  // may_lose: acknowledged transactions may be missing; may_damage: so may
  // everything (kOff under a power cut), and the run is only counted
  void add(const Outcome& o, bool may_lose, const char* what, std::uint64_t k, bool may_damage = false) {
    ++runs;
    if (may_damage && (!o.error.empty() || o.holds < 0)) {
      ++damaged;
      return;
    }
    if (!o.error.empty()) {
      if (wrong++ == 0)
        std::printf("  WRONG (%s, at %llu): acknowledged %d, and the database cannot be read: %s\n", what,
                    static_cast<unsigned long long>(k), o.acked, o.error.c_str());
      return;
    }
    if (o.rec.stopped.rfind("a bad checksum", 0) == 0) ++torn;
    if (o.rec.stopped.rfind("a frame of another generation", 0) == 0) ++other_gen;
    if (o.rec.stopped.rfind("a frame cut short", 0) == 0) ++cut_short;
    if (o.rec.discarded > 0) ++tails;
    // a transaction that rolled back leaves the same contents as the one
    // before it, so a prefix is "in flight" only if the acked one is not it
    const bool exact = o.holds == o.acked ||
                       (o.holds >= 0 && g_expected[static_cast<std::size_t>(o.holds)] ==
                                            g_expected[static_cast<std::size_t>(o.acked)]);
    if (exact) {
      ++at_ack;
    } else if (o.holds == o.acked + 1) {
      ++in_flight;
    } else if (may_lose && o.holds >= 0 && o.holds < o.acked) {
      ++lost_runs;
      max_lost = std::max(max_lost, o.acked - o.holds);
    } else {
      if (wrong++ == 0)
        std::printf("  WRONG (%s, at %llu): acknowledged %d, the database holds %s\n    %s\n    recovery: %s\n", what,
                    static_cast<unsigned long long>(k), o.acked,
                    o.holds < 0 ? "no prefix of the workload" : std::to_string(o.holds).c_str(),
                    o.digest.substr(0, 300).c_str(), o.rec.stopped.c_str());
    }
  }
  void print(const char* what) const {
    std::printf("  %-44s %5d runs: the acknowledged %5d, and the one in flight %4d", what, runs, at_ack, in_flight);
    if (lost_runs) std::printf(", fewer %4d (by up to %d)", lost_runs, max_lost);
    if (damaged) std::printf(", damaged %d", damaged);
    std::printf(", wrong %d\n", wrong);
    std::fflush(stdout);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t stride = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;

  // ---- without a crash, the engine holds the model's rows after every transaction ----
  fresh_database();
  {
    const pid_t pid = ::fork();
    if (pid == 0) {
      Pager pager(kPath);
      pager.set_checkpoint_frames(kCheckpointAt);
      BufferPool pool(pager, kPoolFrames);
      Catalog cat(pool);
      Executor e(cat);
      std::FILE* f = std::fopen("u13_crash_test.expected", "w");
      run_workload(e, kWork, [&](int) { std::fprintf(f, "%s\n", digest(e, cat).c_str()); });
      std::fclose(f);
      _exit(0);
    }
    int st;
    ::waitpid(pid, &st, 0);
    std::FILE* f = std::fopen("u13_crash_test.expected", "r");
    REQUIRE(f);
    std::string line;
    std::size_t i = 1, same = 0;
    for (int c; (c = std::fgetc(f)) != EOF;) {
      if (c != '\n') {
        line += static_cast<char>(c);
        continue;
      }
      if (line == g_expected[i]) ++same;
      else if (same + 1 == i) std::printf("  the engine, after transaction %zu, holds:\n    %s\n  the model:\n    %s\n", i,
                                          line.substr(0, 300).c_str(), g_expected[i].substr(0, 300).c_str());
      ++i;
      line.clear();
    }
    std::fclose(f);
    ::unlink("u13_crash_test.expected");
    std::printf("without a crash: the model's contents after %zu of %zu transactions\n", same, kWork.size());
    REQUIRE(same == kWork.size());
  }
  std::uint64_t events = 0;
  double run_us = 0;
  for (Durability d : {Durability::kFull, Durability::kNormal, Durability::kOff}) {
    fresh_database();
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid == 0) {
      ::close(fds[0]);
      CrashSim count(CrashSim::Mode::kCount, 0, 0);
      run(d, &count, [&](int i) {
        if (i == static_cast<int>(kWork.size())) {
          const std::uint64_t n = count.events();
          if (::write(fds[1], &n, sizeof n) != sizeof n) _exit(3);
        }
      });
    }
    ::close(fds[1]);
    std::uint64_t n = 0;
    REQUIRE(::read(fds[0], &n, sizeof n) == sizeof n);
    ::close(fds[0]);
    int st;
    ::waitpid(pid, &st, 0);
    if (d == Durability::kFull) {
      events = n;
      run_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    }
    std::printf("%s", d == Durability::kFull ? "" : "");
    if (d == Durability::kFull)
      std::printf("the workload: %zu transactions, %llu writes and syncs under Durability::kFull (%.0f ms)\n",
                  kWork.size(), static_cast<unsigned long long>(n), run_us / 1000);
    else
      std::printf("              ... %llu writes and syncs under Durability::%s\n",
                  static_cast<unsigned long long>(n), d == Durability::kNormal ? "kNormal" : "kOff");
  }
  std::printf("(a pool of %zu frames, a checkpoint whenever the log reaches %llu frames)\n\n", kPoolFrames,
              static_cast<unsigned long long>(kCheckpointAt));

  // ---- crash at every write and sync, both ways, under kFull ----
  Tally process, power, killed;
  for (std::uint64_t k = 1; k <= events; k += stride)
    process.add(crash_once(Durability::kFull, CrashSim::Mode::kProcess, k, 0), false, "process", k);
  process.print("kFull, the process dies at each write/sync:");
  for (std::uint64_t k = 1; k <= events; k += stride)
    power.add(crash_once(Durability::kFull, CrashSim::Mode::kPower, k, 0), false, "power", k);
  power.print("kFull, the power fails at each write/sync:");
  std::mt19937_64 rng(13);
  const int kills = static_cast<int>(std::max<std::uint64_t>(20, 300 / stride));
  for (int r = 0; r < kills; ++r)
    killed.add(crash_once(Durability::kFull, CrashSim::Mode::kCount, 0,
                          1 + rng() % static_cast<std::uint64_t>(run_us * 1.1)),
               false, "kill -9", static_cast<std::uint64_t>(r));
  killed.print("kFull, kill -9 at a random moment:");

  // ---- the weaker modes: a power cut may lose what was acknowledged ----
  const std::uint64_t step = std::max<std::uint64_t>(1, stride) * 3;
  Tally normal, off_process, off_power;
  for (std::uint64_t k = 1; k <= events; k += step)
    normal.add(crash_once(Durability::kNormal, CrashSim::Mode::kPower, k, 0), true, "kNormal power", k);
  normal.print("kNormal, the power fails at every 3rd:");
  for (std::uint64_t k = 1; k <= events; k += step)
    off_process.add(crash_once(Durability::kOff, CrashSim::Mode::kProcess, k, 0), false, "kOff process", k);
  off_process.print("kOff, the process dies at every 3rd:");
  for (std::uint64_t k = 1; k <= events; k += step)
    off_power.add(crash_once(Durability::kOff, CrashSim::Mode::kPower, k, 0), true, "kOff power", k, true);
  off_power.print("kOff, the power fails at every 3rd:");
  REQUIRE(normal.wrong == 0 && off_process.wrong == 0 && off_power.wrong == 0);

  std::printf("\nwhere recovery stopped reading the log: a torn frame %d times, a frame of an older\n"
              "generation %d times, a frame cut short %d times; a transaction cut off and discarded %d times\n",
              process.torn + power.torn + killed.torn, process.other_gen + power.other_gen + killed.other_gen,
              process.cut_short + power.cut_short + killed.cut_short, process.tails + power.tails + killed.tails);
  REQUIRE(process.wrong == 0 && power.wrong == 0 && killed.wrong == 0);
  ::unlink(kPath);
  ::unlink((std::string(kPath) + "-wal").c_str());
  std::printf("u13_crash_test: PASS\n");
  return 0;
}
