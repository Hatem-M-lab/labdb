#include "pager.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <random>

#include "io.hpp"

namespace labdb {

namespace {

// Meta page payload, byte-exact (page 0, after the standard 20-byte header):
//
//   offset  size  field
//   ------  ----  ------------------------------
//       20     8  magic: "labdb001"
//       28     4  u32 page_count (includes page 0)
//       32     4  u32 free-list head page id (0 = empty list)
//       36     4  u32 btree_root  page id (0 = no tree yet)   [Unit 2]
//       40     4  u32 heap_head   page id (0 = no table yet)  [Unit 5]
//       44     4  u32 catalog_root page id (0 = no catalog)   [Unit 6]
//       48     8  u64 database id, drawn at creation          [Unit 13]
//
constexpr char kMagic[8] = {'l', 'a', 'b', 'd', 'b', '0', '0', '1'};
constexpr std::size_t kMetaMagicOff = kPageHeaderSize;
constexpr std::size_t kMetaPageCountOff = kMetaMagicOff + sizeof kMagic;
constexpr std::size_t kMetaFreelistOff = kMetaPageCountOff + 4;
constexpr std::size_t kMetaBtreeRootOff = kMetaFreelistOff + 4;
constexpr std::size_t kMetaHeapHeadOff = kMetaBtreeRootOff + 4;
constexpr std::size_t kMetaCatalogRootOff = kMetaHeapHeadOff + 4;
constexpr std::size_t kMetaDbIdOff = kMetaCatalogRootOff + 4;

off_t page_offset(PageId id) {
  return static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
}

}  // namespace

// Reads the meta page -- or, for an empty file (or one whose creation never
// finished writing page 0), formats a new database and syncs it.
std::uint64_t Pager::open_database(int fd, Meta& meta) {
  if (file_size(fd) < static_cast<off_t>(kPageSize)) {
    std::random_device rd;
    const std::uint64_t id = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
    meta = Meta{};
    Page p;
    p.set_id(0);
    p.set_type(PageType::kMeta);
    std::memcpy(p.data() + kMetaMagicOff, kMagic, sizeof kMagic);
    store_u32(p.data() + kMetaPageCountOff, meta.page_count);
    store_u64(p.data() + kMetaDbIdOff, id);
    truncate_exact(fd, 0);
    pwrite_exact(fd, p.data(), kPageSize, 0);
    sync_exact(fd);
    return id;
  }
  Page p;
  pread_exact(fd, p.data(), kPageSize, 0);
  check_that(std::memcmp(p.data() + kMetaMagicOff, kMagic, sizeof kMagic) == 0,
             "bad magic: this is not a labdb file");
  meta.page_count = load_u32(p.data() + kMetaPageCountOff);
  meta.freelist_head = load_u32(p.data() + kMetaFreelistOff);
  meta.btree_root = load_u32(p.data() + kMetaBtreeRootOff);
  meta.heap_head = load_u32(p.data() + kMetaHeapHeadOff);
  meta.catalog_root = load_u32(p.data() + kMetaCatalogRootOff);
  return load_u64(p.data() + kMetaDbIdOff);
}

Page Pager::meta_page() const {
  Page p;
  p.set_id(0);
  p.set_type(PageType::kMeta);
  std::memcpy(p.data() + kMetaMagicOff, kMagic, sizeof kMagic);
  store_u32(p.data() + kMetaPageCountOff, meta_.page_count);
  store_u32(p.data() + kMetaFreelistOff, meta_.freelist_head);
  store_u32(p.data() + kMetaBtreeRootOff, meta_.btree_root);
  store_u32(p.data() + kMetaHeapHeadOff, meta_.heap_head);
  store_u32(p.data() + kMetaCatalogRootOff, meta_.catalog_root);
  store_u64(p.data() + kMetaDbIdOff, db_id_);
  return p;
}

void Pager::load_meta(const Page& p) {
  check_that(std::memcmp(p.data() + kMetaMagicOff, kMagic, sizeof kMagic) == 0 &&
                 load_u64(p.data() + kMetaDbIdOff) == db_id_,
             "recovery: the log's meta page is not this database's");
  meta_.page_count = load_u32(p.data() + kMetaPageCountOff);
  meta_.freelist_head = load_u32(p.data() + kMetaFreelistOff);
  meta_.btree_root = load_u32(p.data() + kMetaBtreeRootOff);
  meta_.heap_head = load_u32(p.data() + kMetaHeapHeadOff);
  meta_.catalog_root = load_u32(p.data() + kMetaCatalogRootOff);
}

Pager::Pager(const std::string& path)
    : fd_(open_or_create(path)),
      db_id_(open_database(fd_, meta_)),
      committed_meta_(meta_),
      wal_(path + "-wal", db_id_) {
  file_pages_ = static_cast<std::uint32_t>(file_size(fd_) / static_cast<off_t>(kPageSize));
  // Recovery. Every transaction the log holds a commit for is redone into the
  // database file by the checkpoint; whatever follows the last commit is
  // discarded. Either way the log then starts a new generation, so no frame
  // left in it can ever be read as part of a later transaction.
  recovery_ = wal_.recover();
  if (recovery_.transactions > 0) {
    Page m;
    check_that(wal_.read(0, m), "recovery: the log's commits hold no meta page");
    load_meta(m);
    committed_meta_ = meta_;
    checkpoint();
  } else if (recovery_.found) {
    wal_.reset();
  }
  check_that(file_pages_ >= meta_.page_count,
             "the database file is shorter than its meta page says");
}

Pager::~Pager() {
  try {
    commit();  // what a direct caller wrote and never committed
    checkpoint();
    wal_.truncate();
  } catch (...) {
    // A destructor cannot report an I/O error usefully; sync() is the checked
    // path. The log still holds whatever did not reach the file.
  }
  if (fd_ >= 0) ::close(fd_);
}

void Pager::read_page(PageId id, Page& out) {
  check_that(id != 0 && id < meta_.page_count, "read_page: page id out of range");
  ++reads_;
  if (!wal_.read(id, out)) {
    if (id < file_pages_) pread_exact(fd_, out.data(), kPageSize, page_offset(id));
    else out = Page{};  // allocated since the last checkpoint and never written
  }
  // Self-check: a formatted page must know its own name. Catches offset
  // arithmetic bugs the moment they happen instead of three units later.
  check_that(out.id() == id || out.type() == PageType::kInvalid,
             "read_page: page header claims a different id (corruption?)");
}

void Pager::write_page(PageId id, const Page& in) {
  check_that(id != 0 && id < meta_.page_count, "write_page: page id out of range");
  check_that(in.id() == id,
             "write_page: page header id does not match destination");
  wal_.write(id, in);
}

PageId Pager::allocate_page() {
  if (meta_.freelist_head != kNullPage) {
    const PageId id = meta_.freelist_head;
    Page p;
    read_page(id, p);
    check_that(p.type() == PageType::kFree,
               "free-list contains a page not marked free");
    meta_.freelist_head = p.next_page();
    touch_meta();
    return id;
  }
  // Free list empty: the database grows by one page. Unit 13: the file does
  // not -- the page exists in the log until a checkpoint copies it there.
  const PageId id = meta_.page_count++;
  touch_meta();
  return id;
}

void Pager::free_page(PageId id) {
  check_that(id != 0 && id < meta_.page_count, "free_page: page id out of range");
  {
    Page current;
    read_page(id, current);
    check_that(current.type() != PageType::kFree,
               "free_page: double free (page is already on the free list)");
  }
  Page p;
  p.set_id(id);
  p.set_type(PageType::kFree);
  p.set_next_page(meta_.freelist_head);
  write_page(id, p);
  meta_.freelist_head = id;
  touch_meta();
}

void Pager::commit() {
  if (!in_transaction()) return;  // a transaction that wrote nothing costs nothing
  wal_.commit(meta_page(), durability_ == Durability::kFull);
  committed_meta_ = meta_;
  meta_dirty_ = false;
  ++commits_;
  if (wal_.frames() >= checkpoint_frames_) checkpoint();
}

void Pager::rollback() {
  wal_.rollback();
  meta_ = committed_meta_;
  meta_dirty_ = false;
}

void Pager::checkpoint() {
  check_that(!in_transaction(), "checkpoint inside a transaction");
  const auto frames = wal_.committed_frames();
  if (frames.empty()) return;
  // The frames about to be copied must be durable before the file they are
  // copied into is touched: a power cut can tear a page of the file midway,
  // and then only its frame can put it back. kFull synced them at commit.
  if (durability_ == Durability::kNormal) wal_.sync();
  Page buf;
  for (const auto& [id, slot] : frames) {
    wal_.read_frame(slot, buf);
    pwrite_exact(fd_, buf.data(), kPageSize, page_offset(id));
  }
  pages_checkpointed_ += frames.size();
  const off_t want = page_offset(meta_.page_count);
  if (file_size(fd_) < want) truncate_exact(fd_, want);
  // Only once the file is durable may the log forget the frames.
  if (durability_ != Durability::kOff) sync_exact(fd_);
  file_pages_ = std::max(file_pages_, meta_.page_count);
  wal_.reset();
  ++checkpoints_;
}

void Pager::sync() {
  commit();
  checkpoint();
  sync_exact(fd_);
}

std::uint32_t Pager::freelist_length() {
  std::uint32_t n = 0;
  PageId id = meta_.freelist_head;
  Page p;
  while (id != kNullPage) {
    ++n;
    check_that(n <= meta_.page_count, "free list contains a cycle");
    read_page(id, p);
    check_that(p.type() == PageType::kFree,
               "free-list contains a page not marked free");
    id = p.next_page();
  }
  return n;
}

void Pager::set_btree_root(PageId id) {
  meta_.btree_root = id;
  touch_meta();
}

void Pager::set_heap_head(PageId id) {
  meta_.heap_head = id;
  touch_meta();
}

void Pager::set_catalog_root(PageId id) {
  meta_.catalog_root = id;
  touch_meta();
}

}  // namespace labdb
