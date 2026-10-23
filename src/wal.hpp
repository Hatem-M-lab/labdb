#pragma once
// Wal -- the write-ahead log. Unit 13.
//
// Through Unit 12 a page reached the database file whenever the buffer pool
// evicted it or the pool was synced: in no particular order, one page at a
// time, any of them possibly torn by a crash in the middle of its write. A
// statement that changed four pages could reach the disk as one, two or three
// of them. This is the cure, in the form SQLite's WAL mode takes:
//
//   * Changed pages never go to the database file directly. They are appended
//     to a second file, the log, as frames: a 24-byte header and the whole
//     4 KiB page. The frames of one transaction end in a commit frame -- the
//     meta page, flagged -- and only then is the transaction committed; with
//     Durability::kFull the log is fsynced before commit() returns.
//   * A page is read from the log when the log holds a version of it, else
//     from the database file. So the database file holds the state as of the
//     last checkpoint, and the log every committed change since.
//   * A checkpoint copies the latest committed version of each page from the
//     log into the database file, makes that durable, and resets the log.
//   * Recovery, when the database is opened, reads the log from the start and
//     keeps every frame up to the last commit frame, stopping at the first
//     frame that is short, belongs to another generation of the log, or fails
//     its checksum: what follows it was never committed.
//
// File layout (byte-exact, little-endian), next to the database as
// "<database>-wal":
//
//   offset 0, 64 bytes: header
//       0   8  magic "labdbwal"
//       8   4  u32 version (1)
//      12   4  u32 page size
//      16   8  u64 generation: changes at every reset; every frame carries it
//      24   8  u64 database id: the meta page's, so a log is never applied to
//                another database
//      32   8  u64 checksum of bytes 0..32
//   then frame i at 64 + i * 4120:
//       0   4  u32 page id
//       4   4  u32 flags: bit 0 = commit (on the meta page's frame only)
//       8   8  u64 generation
//      16   8  u64 checksum of bytes 0..16 and of the page
//      24  4096 the page
//
// A frame of the transaction in progress -- a pending frame -- is overwritten
// in place when its page is written again in the same transaction, so a long
// transaction's log grows with the pages it touches, not with its writes.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "page.hpp"

namespace labdb {

// How much a commit waits for. kFull: commit() returns once the log is on
// stable storage -- a power cut loses nothing committed. kNormal: the log is
// synced only before a checkpoint -- a power cut can lose the last commits,
// never corrupt the database. kOff: never synced -- safe against the process
// dying, not against the power. (SQLite's synchronous = FULL, NORMAL, OFF.)
enum class Durability { kFull, kNormal, kOff };

// What recovery found in the log when the database was opened.
struct RecoveryReport {
  bool found = false;              // the log had a header of this database's
  std::uint64_t frames = 0;        // valid frames, in order from the start
  std::uint64_t transactions = 0;  // commit frames among them
  std::uint64_t pages = 0;         // distinct pages those transactions wrote
  std::uint64_t discarded = 0;     // valid frames after the last commit: a transaction cut off
  std::string stopped = "no log";  // why the scan stopped
};

inline constexpr std::size_t kWalHeaderSize = 64;
inline constexpr std::size_t kFrameHeaderSize = 24;
inline constexpr std::size_t kFrameSize = kFrameHeaderSize + kPageSize;  // 4120
inline constexpr std::uint32_t kFrameCommit = 1;

// A 64-bit checksum in the style of xxHash64: four lanes of multiply-rotate
// rounds, then an avalanche. Not cryptographic; it tells a torn or foreign
// frame from a whole one with odds of 2^-64 of being fooled.
std::uint64_t checksum64(const std::uint8_t* p, std::size_t n, std::uint64_t seed);

class Wal {
 public:
  // Open (creating) the log at `path` for the database whose id is `db_id`. A
  // log whose header is missing, torn or another database's is set aside.
  Wal(const std::string& path, std::uint64_t db_id);
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  // Scan the log from its first frame and keep every committed one.
  RecoveryReport recover();

  // The log's latest version of page `id` -- the pending one first, else the
  // last committed -- into `out`. False if the log holds none.
  bool read(PageId id, Page& out);
  // `page` becomes the transaction in progress's version of page `id`.
  void write(PageId id, const Page& page);
  bool has_pending() const { return !pending_.empty(); }
  bool is_pending(PageId id) const { return pending_.count(id) != 0; }
  std::vector<PageId> pending_pages() const {
    std::vector<PageId> v;
    for (const auto& kv : pending_) v.push_back(kv.first);
    return v;
  }

  // Append the commit frame (the meta page, flagged) and make every pending
  // frame committed; with `sync`, fsync the log before returning.
  void commit(const Page& meta, bool sync);
  // Forget every pending frame: the next write reuses their place in the log.
  void rollback();

  // The committed frames, by page: what a checkpoint copies.
  std::vector<std::pair<PageId, std::uint64_t>> committed_frames() const;
  void read_frame(std::uint64_t slot, Page& out);
  std::uint64_t frames() const { return end_; }  // frame slots in use
  // After a checkpoint: a new generation, no frames. The file is not cut: the
  // next frames overwrite the old ones, and the generation tells them apart.
  void reset();
  // At a clean close: no frames, no header, an empty file.
  void truncate();
  void sync();

  // Counters, since the log was opened.
  std::uint64_t frames_written() const { return frames_written_; }
  std::uint64_t syncs() const { return syncs_; }

  // Demonstration switch ONLY (Challenge 13.5's forensic trap): reset the log
  // without moving to a new generation. Real use never calls this.
  void demo_keep_generation() { keep_generation_ = true; }

 private:
  void write_header();
  void write_frame(std::uint64_t slot, PageId id, std::uint32_t flags, const Page& page);
  off_t slot_offset(std::uint64_t slot) const {
    return static_cast<off_t>(kWalHeaderSize + slot * kFrameSize);
  }

  int fd_ = -1;
  std::uint64_t db_id_;
  std::uint64_t generation_ = 0;
  bool header_ok_ = false;       // a valid header of this generation is in the file
  bool keep_generation_ = false;
  std::uint64_t end_ = 0;          // next free slot
  std::uint64_t commit_end_ = 0;   // slot after the last commit frame
  std::unordered_map<PageId, std::uint64_t> committed_;  // page -> slot of its latest committed frame
  std::unordered_map<PageId, std::uint64_t> pending_;    // page -> slot, the transaction in progress
  std::uint64_t frames_written_ = 0;
  std::uint64_t syncs_ = 0;
};

}  // namespace labdb
