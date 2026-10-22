// THE BUG, preserved for study: a hash join whose bucket index is the raw key
// masked to its low bits -- no mixing. "An integer key is already a fine
// hash", the reasoning goes, and for keys like 0, 1, 2, 3 it is: consecutive
// integers land in consecutive buckets, perfectly spread. Every test passes.
//
// Then the join meets keys whose low bits never change. Snowflake-style ids
// -- (milliseconds << 22) | (worker << 12) | sequence -- from one worker at
// low traffic have the same low 22 bits on every row, so `key & (buckets-1)`
// is the same number for every key, and every build row lands in one bucket.
// The answer is still exactly right. Only the cost changes: each probe walks
// the whole build side, and the hash join has quietly become a nested loop.
//
// build: see the Makefile (links the engine).
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "operator.hpp"
#include "pager.hpp"

using namespace labdb;
using labdb::test::now_s;

namespace {

// BUGGY: "the key is already a good hash" -- so the bucket is its low bits.
std::uint64_t no_mix(std::uint64_t h) { return h; }

struct Outcome {
  std::size_t rows;
  double seconds;
  JoinHashTable::Occupancy occ;
};

// Join events to sessions with a hash join that uses `mix` -- the same plan
// the executor builds for this query, assembled by hand so that the one
// function under suspicion can be swapped.
Outcome run_join(Catalog& cat, const std::string& events, const std::string& sessions,
                 JoinHashTable::MixFn mix) {
  // events e: (eid, session, kind) -- e.session is position 1
  // sessions s: (id, user)          -- s.id is position 0
  HashJoin hj(std::make_unique<SeqScan>(cat, TableRef{events, "e"}),
              std::make_unique<SeqScan>(cat, TableRef{sessions, "s"}),
              {{1, 0}}, {}, "e.session = s.id", mix);
  const double t0 = now_s();
  hj.open();
  Row r;
  std::size_t n = 0;
  while (hj.next(r)) ++n;
  hj.close();
  return {n, now_s() - t0, hj.table().occupancy()};
}

void report(const char* keys, const char* mixer, const Outcome& o) {
  std::printf("  %-26s %-9s %7zu   %7.3f s   %5zu of %-6zu %9zu\n", keys, mixer, o.rows,
              o.seconds, o.occ.used, o.occ.buckets, o.occ.longest);
}

void histogram(const char* title, const JoinHashTable::Occupancy& o) {
  static const char* classes[] = {"0", "1", "2", "3-4", "5-8", "9-16", "17-32", "33-64",
                                  "65-128", "129-256", "257-512", "513-1024", "1025-2048",
                                  "2049-4096", "4097-8192", "8193-16384", "16385-32768"};
  std::printf("  %s\n", title);
  for (std::size_t c = 0; c < o.chain_hist.size(); ++c)
    if (o.chain_hist[c])
      std::printf("    buckets holding %-11s rows: %6zu\n", classes[c], o.chain_hist[c]);
}

}  // namespace

int main() {
  const char* path = "u09_hash_collapse_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 4096);
  Catalog cat(pool);
  Executor e(cat);

  const int kSessions = 20000, kEvents = 100000;
  std::mt19937_64 rng(95);

  // Two session tables with the same shape and the same 20,000 rows, except
  // for how the ids were made: 0, 1, 2, ... in one; snowflake ids in the other.
  e.run_sql("CREATE TABLE sessions_dense (id INT64 NOT NULL, user INT32)");
  e.run_sql("CREATE TABLE sessions_snow (id INT64 NOT NULL, user INT32)");
  e.run_sql("CREATE TABLE events_dense (eid INT64 NOT NULL, session INT64 NOT NULL, kind INT32)");
  e.run_sql("CREATE TABLE events_snow (eid INT64 NOT NULL, session INT64 NOT NULL, kind INT32)");
  std::vector<std::int64_t> snow;
  std::uint64_t ms = 120'000'000'000ULL;  // milliseconds since the service's epoch
  for (int i = 0; i < kSessions; ++i) {
    ms += 1 + rng() % 50;                                  // one session every few ms
    snow.push_back(static_cast<std::int64_t>((ms << 22) | (1u << 12)));  // worker 1, seq 0
    const std::string user = std::to_string(rng() % 5000);
    e.run_sql("INSERT INTO sessions_dense VALUES (" + std::to_string(i) + ", " + user + ")");
    e.run_sql("INSERT INTO sessions_snow VALUES (" + std::to_string(snow.back()) + ", " + user + ")");
  }
  for (int j = 0; j < kEvents; ++j) {
    const int s = static_cast<int>(rng() % kSessions);
    const std::string kind = std::to_string(rng() % 8);
    e.run_sql("INSERT INTO events_dense VALUES (" + std::to_string(j) + ", " +
              std::to_string(s) + ", " + kind + ")");
    e.run_sql("INSERT INTO events_snow VALUES (" + std::to_string(j) + ", " +
              std::to_string(snow[s]) + ", " + kind + ")");
  }

  std::printf("what snowflake ids look like -- the low 22 bits never change:\n");
  for (int i = 0; i < 3; ++i)
    std::printf("  %lld = 0x%016llx\n", static_cast<long long>(snow[i]),
                static_cast<unsigned long long>(snow[i]));

  std::printf("\n%d events joined to %d sessions, hash join with the sessions as the build side:\n\n",
              kEvents, kSessions);
  std::printf("  keys                       mixer        rows       time   buckets used   longest chain\n");
  const Outcome dense_bad = run_join(cat, "events_dense", "sessions_dense", no_mix);
  report("dense ids 0, 1, 2, ...", "none", dense_bad);
  const Outcome dense_good = run_join(cat, "events_dense", "sessions_dense", mix64);
  report("dense ids 0, 1, 2, ...", "mix64", dense_good);
  const Outcome snow_good = run_join(cat, "events_snow", "sessions_snow", mix64);
  report("snowflake ids", "mix64", snow_good);
  const Outcome snow_bad = run_join(cat, "events_snow", "sessions_snow", no_mix);
  report("snowflake ids", "none", snow_bad);
  REQUIRE(dense_bad.rows == static_cast<std::size_t>(kEvents) && snow_bad.rows == dense_bad.rows);
  REQUIRE(snow_good.rows == snow_bad.rows && dense_good.rows == dense_bad.rows);

  std::printf("\n");
  histogram("snowflake ids, no mixer -- the build side's bucket occupancy:", snow_bad.occ);
  histogram("snowflake ids, mix64 -- the same rows:", snow_good.occ);

  // The fixed engine, through SQL: the plan printer shows the same statistics.
  e.run_sql("SELECT e.eid, s.user FROM events_snow e JOIN sessions_snow s ON e.session = s.id");
  std::printf("\nthe engine's own plan for the snowflake join, after running it:\n%s",
              e.explain_last().c_str());

  std::printf(
      "\nEvery run returned all %zu rows: the answer never changed, only its cost. Without a "
      "mixer the bucket is key & %zu, the low %d bits of the key -- and on snowflake ids those "
      "bits are the worker and sequence fields, identical on every row. All %zu sessions share "
      "one bucket, so each of the %d probes walks all of them: %.1f s instead of %.3f s, a %.0fx "
      "slowdown with no error and a correct result. The dense ids hid it: their low bits are "
      "the part that changes, so the unmixed table looked perfect in every test.\n",
      snow_bad.rows, snow_bad.occ.buckets - 1, __builtin_ctzll(snow_bad.occ.buckets), snow.size(),
      kEvents, snow_bad.seconds, snow_good.seconds, snow_bad.seconds / snow_good.seconds);

  ::unlink(path);
  return 0;
}
