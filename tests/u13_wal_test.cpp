// Unit 13: the write-ahead log and the transactions it makes possible, one
// property at a time. (tests/u13_crash_test.cpp crashes the whole engine;
// this test checks what each piece promises.)
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "buffer_pool.hpp"
#include "catalog.hpp"
#include "executor.hpp"
#include "harness.hpp"
#include "io.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

void wipe(const std::string& path) {
  ::unlink(path.c_str());
  ::unlink((path + "-wal").c_str());
}

Page marked(PageId id, std::uint8_t mark) {
  Page p;
  p.set_id(id);
  p.set_type(PageType::kSlotted);
  std::memset(p.data() + kPageHeaderSize, mark, kPageSize - kPageHeaderSize);
  return p;
}

std::uint8_t mark_of(Pager& pager, PageId id) {
  Page p;
  pager.read_page(id, p);
  return p.data()[kPageSize - 1];
}

std::int64_t count(Executor& e, const std::string& table) {
  return e.run_sql("SELECT COUNT(*) FROM " + table).rows[0][0].i64;
}

// Runs `body` in a child process, which must end it with _exit(0): the
// process dies there, inside the scope of its objects, so no destructor
// commits, checkpoints or closes anything.
template <class F>
void in_child_then_die(F body) {
  const pid_t pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    body();
    _exit(1);  // the body returned instead of dying
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// An I/O error, on demand: the n-th write from now throws.
struct FailingWrite : IoHook {
  int left;
  explicit FailingWrite(int n) : left(n) {}
  void before_write(int, const void*, std::size_t, off_t) override {
    if (--left == 0) fail("pwrite: injected I/O error");
  }
  void before_sync(int) override {}
};

// A torn page: the first page-sized write into the database file -- a
// checkpoint copying a page from the log -- writes half the page and dies.
struct TearCheckpoint : IoHook {
  void before_write(int fd, const void* buf, std::size_t n, off_t off) override {
    if (n == kPageSize && off >= static_cast<off_t>(kPageSize)) {
      const ssize_t w = ::pwrite(fd, buf, kPageSize / 2, off);
      (void)w;
      _exit(0);
    }
  }
  void before_sync(int) override {}
};

}  // namespace

int main() {
  const std::string path = "u13_wal_test.db";

  // ---- 1. pages: committed, rolled back, recovered ----
  wipe(path);
  {
    Pager pager(path);
    const PageId a = pager.allocate_page(), b = pager.allocate_page();
    pager.write_page(a, marked(a, 1));
    pager.write_page(b, marked(b, 1));
    pager.commit();
    pager.write_page(a, marked(a, 2));
    const PageId c = pager.allocate_page();
    pager.write_page(c, marked(c, 2));
    REQUIRE(mark_of(pager, a) == 2 && pager.page_count() == 4);  // the transaction sees its own writes
    pager.rollback();
    REQUIRE(mark_of(pager, a) == 1 && mark_of(pager, b) == 1);
    REQUIRE(pager.page_count() == 3);  // the allocation is undone with the rest
    pager.write_page(b, marked(b, 3));
    pager.commit();
  }
  {
    Pager pager(path);
    REQUIRE(mark_of(pager, 1) == 1 && mark_of(pager, 2) == 3 && pager.page_count() == 3);
  }
  std::printf("PASS pages: a rollback undoes the writes and the allocation; a commit survives a reopen\n");

  // ---- 2. a commit that fails is a rollback ----
  wipe(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64, s TEXT)");
    e.run_sql("CREATE INDEX ON t (v)");
    for (int i = 0; i < 50; ++i)
      e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 7) + ", 'x')");
    int failed = 0;
    for (int n = 1; n <= 4; ++n) {  // the 1st, 2nd, ... write of the commit fails
      FailingWrite hook(n);
      set_io_hook(&hook);
      bool threw = false;
      try {
        e.run_sql("INSERT INTO t VALUES (1000, 3, 'never')");
      } catch (const std::exception&) {
        threw = true;
      }
      set_io_hook(nullptr);
      failed += threw;
      REQUIRE(count(e, "t") == 50);  // nothing of it: not the row, not its index entry
      REQUIRE(e.run_sql("SELECT id FROM t WHERE v = 3").row_count() == 7);
    }
    REQUIRE(failed == 4);
    e.run_sql("INSERT INTO t VALUES (1000, 3, 'now')");
    REQUIRE(count(e, "t") == 51);
  }
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    REQUIRE(count(e, "t") == 51 && cat.find_table("t")->rows == 51);
  }
  std::printf("PASS atomic statements: an INSERT whose commit fails at its 1st to 4th write leaves nothing\n"
              "     behind, in the table or its index; the next one commits\n");

  // ---- 3. BEGIN, COMMIT, ROLLBACK ----
  wipe(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64)");
    e.run_sql("BEGIN");
    for (int i = 0; i < 100; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    REQUIRE(count(e, "t") == 100);  // the transaction sees its own rows
    e.run_sql("ROLLBACK");
    REQUIRE(count(e, "t") == 0);
    e.run_sql("BEGIN");
    e.run_sql("CREATE TABLE u (id INT64 NOT NULL)");
    e.run_sql("CREATE INDEX ON t (v)");
    e.run_sql("INSERT INTO u VALUES (1)");
    e.run_sql("ROLLBACK");
    REQUIRE(!cat.table_exists("u") && !cat.find_table("t")->indexed[1]);  // DDL rolls back too
    e.run_sql("BEGIN");
    for (int i = 0; i < 100; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
    e.run_sql("COMMIT");
    REQUIRE(count(e, "t") == 100);
    // an error inside a transaction rolls the whole of it back, and ends it
    e.run_sql("BEGIN");
    e.run_sql("INSERT INTO t VALUES (500, 5)");
    bool threw = false;
    try {
      e.run_sql("INSERT INTO nowhere VALUES (1)");
    } catch (const ExecError&) {
      threw = true;
    }
    REQUIRE(threw && !e.in_transaction() && count(e, "t") == 100);
    // misuse is an error, and changes nothing
    int errors = 0;
    for (const char* sql : {"COMMIT", "ROLLBACK"}) {
      try {
        e.run_sql(sql);
      } catch (const ExecError&) {
        ++errors;
      }
    }
    e.run_sql("BEGIN");
    try {
      e.run_sql("BEGIN");
    } catch (const ExecError&) {
      ++errors;
    }
    REQUIRE(errors == 3);
  }
  std::printf("PASS transactions: ROLLBACK undoes rows, tables and indexes; an error inside BEGIN rolls it all\n"
              "     back and ends it; COMMIT or ROLLBACK without BEGIN, or BEGIN inside one, is refused\n");

  // ---- 4. what each durability waits for ----
  for (Durability d : {Durability::kFull, Durability::kNormal, Durability::kOff}) {
    wipe(path);
    Pager pager(path);
    pager.set_durability(d);
    pager.set_checkpoint_frames(1'000'000);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL)");
    const std::uint64_t s0 = pager.log_syncs(), c0 = pager.commits();
    for (int i = 0; i < 20; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ")");
    for (int i = 0; i < 20; ++i) e.run_sql("SELECT * FROM t WHERE id = " + std::to_string(i));
    const std::uint64_t syncs = pager.log_syncs() - s0, commits = pager.commits() - c0;
    REQUIRE(commits == 20);  // the SELECTs wrote nothing, so committed nothing
    REQUIRE(syncs == (d == Durability::kFull ? 20u : 0u));
  }
  std::printf("PASS durability: 20 INSERTs and 20 SELECTs make 20 commits; kFull syncs the log at each,\n"
              "     kNormal and kOff at none\n");

  // ---- 5. a long transaction's log grows with its pages, not its writes ----
  wipe(path);
  {
    Pager pager(path);
    pager.set_checkpoint_frames(1'000'000);
    BufferPool pool(pager, 16);  // far smaller than the transaction: it spills
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64, s TEXT)");
    e.run_sql("CREATE INDEX ON t (v)");
    const std::uint64_t f0 = pager.log_frames(), w0 = pager.log_frames_written();
    const std::uint32_t pages0 = pager.page_count();
    e.run_sql("BEGIN");
    for (int i = 0; i < 5000; ++i)
      e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string((i * 37) % 1000) +
                ", 'row " + std::to_string(i) + "')");
    e.run_sql("COMMIT");
    const std::uint64_t frames = pager.log_frames() - f0, written = pager.log_frames_written() - w0;
    const std::uint32_t grew = pager.page_count() - pages0;
    std::printf("PASS a long transaction: 5,000 INSERTs through a pool of 16 frames wrote %llu frames, into\n"
                "     %llu places in the log -- %u new pages and the few it changed, and the commit\n",
                static_cast<unsigned long long>(written), static_cast<unsigned long long>(frames), grew);
    REQUIRE(frames <= grew + 8 && written > 4 * frames);
    REQUIRE(count(e, "t") == 5000);
  }

  // ---- 6. recovery: what was committed is there, what was not is not ----
  wipe(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
  }
  in_child_then_die([&] {
    Pager pager(path);
    pager.set_checkpoint_frames(1'000'000);
    BufferPool pool(pager, 8);  // small: the last transaction spills into the log before it ends
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, v INT64, s TEXT)");
    for (int i = 0; i < 30; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 1, 'one')");
    e.run_sql("BEGIN");
    for (int i = 30; i < 60; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 2, 'two')");
    e.run_sql("COMMIT");
    e.run_sql("BEGIN");  // never committed: the process dies inside it
    for (int i = 60; i < 2000; ++i)
      e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 3, 'three, never committed')");
    _exit(0);
  });
  {
    Pager pager(path);
    const RecoveryReport r = pager.recovery();
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    REQUIRE(count(e, "t") == 60 && cat.find_table("t")->rows == 60);
    REQUIRE(e.run_sql("SELECT id FROM t WHERE v = 3").row_count() == 0);
    REQUIRE(r.discarded > 0);  // the uncommitted transaction had reached the log
    std::printf("PASS recovery: after the process died inside a transaction, %llu frames read, %llu commits\n"
                "     redone, %llu frames of the unfinished one discarded -- 60 rows, none of the rest\n",
                static_cast<unsigned long long>(r.frames), static_cast<unsigned long long>(r.transactions),
                static_cast<unsigned long long>(r.discarded));
  }

  // ---- 7. Unit 1's torn page, put back from the log ----
  wipe(path);
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE t (id INT64 NOT NULL, s TEXT)");
  }
  in_child_then_die([&] {
    Pager pager(path);
    pager.set_checkpoint_frames(1'000'000);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    for (int i = 0; i < 200; ++i) e.run_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", 'payload " + std::to_string(i) + "')");
    TearCheckpoint tear;
    set_io_hook(&tear);
    pager.checkpoint();  // tears the first page it copies, and dies
  });
  {
    Pager pager(path);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    REQUIRE(pager.recovery().transactions > 0);
    REQUIRE(count(e, "t") == 200);
    const ResultSet r = e.run_sql("SELECT s FROM t WHERE id = 199");
    REQUIRE(r.row_count() == 1 && r.rows[0][0].text == "payload 199");
  }
  std::printf("PASS a torn page: a checkpoint that died halfway through writing a page of the database file;\n"
              "     recovery wrote the page again from the log, and all 200 rows read back\n");

  // ---- 8. a log is never applied to another database ----
  wipe(path);
  in_child_then_die([&] {
    Pager pager(path);
    pager.set_checkpoint_frames(1'000'000);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    Executor e(cat);
    e.run_sql("CREATE TABLE old (id INT64 NOT NULL)");
    e.run_sql("INSERT INTO old VALUES (1)");
    _exit(0);
  });
  ::unlink(path.c_str());  // the database file goes; its log stays behind
  {
    Pager pager(path);  // a new database, at the same name
    REQUIRE(pager.recovery().transactions == 0);
    BufferPool pool(pager, 64);
    Catalog cat(pool);
    REQUIRE(!cat.table_exists("old"));
  }
  std::printf("PASS identity: a new database at the name of a deleted one ignores the deleted one's log\n");

  wipe(path);
  std::printf("u13_wal_test: PASS\n");
  return 0;
}
