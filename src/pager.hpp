#pragma once
// Pager -- names pages and moves them between memory and one database file.
// Challenge 1.4 (open/read/write/sync) and 1.5 (allocate/free + free list).
//
// File layout: page i lives at byte offset i * kPageSize. Page 0 is the
// meta page: magic string, page count, free-list head, the roots, and (Unit
// 13) the database's id. It never leaves the pager.
//
// Unit 13: the pager is crash-safe. Through Unit 12 it wrote each page to the
// database file when asked, and the honesty note here said what that meant: a
// crash between two related page writes left the file inconsistent, and even
// a single page write could tear. Now every page written goes to the
// write-ahead log (src/wal.hpp) as part of the transaction in progress, and
// reaches the database file only at a checkpoint, from the log, after the log
// has made it safe. A transaction is everything written since the last
// commit(); commit() makes it durable and atomic, rollback() makes it never
// have happened, and opening the database recovers whatever the log holds.

#include <cstdint>
#include <string>

#include "page.hpp"
#include "wal.hpp"

namespace labdb {

class Pager {
 public:
  // Opens the database file, creating and formatting it if it is empty, and
  // recovers from its log: every transaction the log holds a commit for is
  // copied into the database file, and everything after it is discarded.
  explicit Pager(const std::string& path);
  ~Pager();  // commits what is left, checkpoints, and empties the log

  Pager(const Pager&) = delete;
  Pager& operator=(const Pager&) = delete;

  // Read/write one full page. Page 0 is off limits; ids must be in range.
  // write_page requires the page's own header id to match its destination.
  // Unit 13: a write goes to the log, as the transaction's version of the
  // page; a read finds the newest version, in the log or else in the file.
  void read_page(PageId id, Page& out);
  void write_page(PageId id, const Page& in);

  // Hand out a page id: reuse the free-list head if there is one, else
  // grow the database by one page. The page's old contents are
  // unspecified -- the caller must format it before use.
  PageId allocate_page();

  // Return a page to the free list. Detects double-frees.
  void free_page(PageId id);

  // Unit 13: end the transaction in progress. commit() writes the meta page
  // as its commit frame and, under Durability::kFull, returns only once the
  // log is on stable storage; with nothing written, it writes nothing.
  // rollback() discards every page written since the last commit, and the
  // meta page's changes with them.
  void commit();
  void rollback();
  bool in_transaction() const { return wal_.has_pending() || meta_dirty_; }
  bool is_pending(PageId id) const { return wal_.is_pending(id); }
  std::vector<PageId> pending_pages() const { return wal_.pending_pages(); }

  // Unit 13: copy every committed page from the log into the database file,
  // make the file durable, and start a new log. Runs by itself when the log
  // reaches checkpoint_frames() frames after a commit, and when the database
  // is opened or closed. Not inside a transaction.
  void checkpoint();

  // Commit, checkpoint, and fsync both files: everything written so far is
  // now on stable storage, in the database file itself.
  void sync();

  // Unit 13: what a commit waits for (wal.hpp). The default is kFull.
  void set_durability(Durability d) { durability_ = d; }
  Durability durability() const { return durability_; }
  void set_checkpoint_frames(std::uint64_t n) { checkpoint_frames_ = n; }
  std::uint64_t checkpoint_frames() const { return checkpoint_frames_; }

  std::uint32_t page_count() const { return meta_.page_count; }
  PageId freelist_head() const { return meta_.freelist_head; }

  // The B+Tree root lives in the meta page so it survives a reopen. A
  // fresh database reports kNullPage ("no tree yet"); Unit 2 uses this to
  // decide whether to plant a new tree or open an existing one. (When the
  // catalog arrives in Unit 7, this single-root field is replaced by a
  // per-table root; for now one tree is enough.)
  PageId btree_root() const { return meta_.btree_root; }
  void set_btree_root(PageId id);

  // A second persistent root, alongside the tree: the head page of the
  // table heap that stores real rows (Unit 5). kNullPage means no rows yet.
  PageId heap_head() const { return meta_.heap_head; }
  void set_heap_head(PageId id);

  // The one, well-known root of the table catalog (Unit 6): a fixed-slot
  // directory page listing every named table and its own heap/tree roots.
  PageId catalog_root() const { return meta_.catalog_root; }
  void set_catalog_root(PageId id);

  // Instrumentation: how many page reads have happened. Unit 2's headline
  // measurement counts the reads a point lookup costs; this is the meter.
  std::uint64_t read_count() const { return reads_; }
  void reset_read_count() { reads_ = 0; }

  // Unit 13: what recovery found when the database was opened, and counts of
  // what the log has done since.
  const RecoveryReport& recovery() const { return recovery_; }
  std::uint64_t commits() const { return commits_; }
  std::uint64_t checkpoints() const { return checkpoints_; }
  std::uint64_t pages_checkpointed() const { return pages_checkpointed_; }  // copied into the file
  std::uint64_t log_frames_written() const { return wal_.frames_written(); }
  std::uint64_t log_syncs() const { return wal_.syncs(); }
  std::uint64_t log_frames() const { return wal_.frames(); }
  std::uint64_t database_id() const { return db_id_; }

  // Demonstration switch ONLY (Challenge 13.5): reset the log without a new
  // generation, as the first version of this unit's log did.
  void demo_keep_log_generation() { wal_.demo_keep_generation(); }

  // Walks the free list and counts it. O(free pages) reads; for tests
  // and the stats output, not for hot paths.
  std::uint32_t freelist_length();

 private:
  struct Meta {
    std::uint32_t page_count = 1;
    PageId freelist_head = kNullPage;
    PageId btree_root = kNullPage;
    PageId heap_head = kNullPage;
    PageId catalog_root = kNullPage;
  };
  static std::uint64_t open_database(int fd, Meta& meta);  // the file's id; formats a new file
  Page meta_page() const;
  void load_meta(const Page& p);
  void touch_meta() { meta_dirty_ = true; }

  // (declared in the order the constructor fills them: the file, its meta
  // page and id, then the log, which needs the id)
  int fd_ = -1;
  Meta meta_;            // as the transaction in progress sees it
  std::uint64_t db_id_ = 0;
  Meta committed_meta_;  // as of the last commit: what rollback restores
  bool meta_dirty_ = false;
  std::uint32_t file_pages_ = 0;  // pages in the database file itself
  Wal wal_;
  Durability durability_ = Durability::kFull;
  std::uint64_t checkpoint_frames_ = 1000;
  RecoveryReport recovery_;
  std::uint64_t reads_ = 0;
  std::uint64_t commits_ = 0;
  std::uint64_t checkpoints_ = 0;
  std::uint64_t pages_checkpointed_ = 0;
};

}  // namespace labdb
