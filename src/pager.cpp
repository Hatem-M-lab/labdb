#include "pager.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

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
//
constexpr char kMagic[8] = {'l', 'a', 'b', 'd', 'b', '0', '0', '1'};
constexpr std::size_t kMetaMagicOff = kPageHeaderSize;
constexpr std::size_t kMetaPageCountOff = kMetaMagicOff + sizeof kMagic;
constexpr std::size_t kMetaFreelistOff = kMetaPageCountOff + 4;

off_t page_offset(PageId id) {
  return static_cast<off_t>(id) * static_cast<off_t>(kPageSize);
}

}  // namespace

Pager::Pager(const std::string& path) {
  fd_ = open_or_create(path);

  struct stat st{};
  check_sys(::fstat(fd_, &st) == 0, "fstat");

  if (st.st_size == 0) {
    // Fresh database: page 0 is born here.
    page_count_ = 1;
    freelist_head_ = kNullPage;
    store_meta();
    check_sys(::fsync(fd_) == 0, "fsync");
    return;
  }

  check_that(st.st_size % static_cast<off_t>(kPageSize) == 0,
             "database size is not a multiple of the page size");

  Page meta;
  pread_exact(fd_, meta.data(), kPageSize, 0);
  check_that(std::memcmp(meta.data() + kMetaMagicOff, kMagic,
                         sizeof kMagic) == 0,
             "bad magic: this is not a labdb file");
  page_count_ = load_u32(meta.data() + kMetaPageCountOff);
  freelist_head_ = load_u32(meta.data() + kMetaFreelistOff);
  check_that(page_count_ ==
                 static_cast<std::uint32_t>(st.st_size / kPageSize),
             "meta page count disagrees with the file size");
}

Pager::~Pager() {
  if (fd_ >= 0) {
    ::fsync(fd_);  // best effort; sync() is the checked API
    ::close(fd_);
  }
}

void Pager::read_page(PageId id, Page& out) {
  check_that(id != 0 && id < page_count_, "read_page: page id out of range");
  pread_exact(fd_, out.data(), kPageSize, page_offset(id));
  // Self-check: a formatted page must know its own name. Catches offset
  // arithmetic bugs the moment they happen instead of three units later.
  check_that(out.id() == id || out.type() == PageType::kInvalid,
             "read_page: page header claims a different id (corruption?)");
}

void Pager::write_page(PageId id, const Page& in) {
  check_that(id != 0 && id < page_count_, "write_page: page id out of range");
  check_that(in.id() == id,
             "write_page: page header id does not match destination");
  pwrite_exact(fd_, in.data(), kPageSize, page_offset(id));
}

PageId Pager::allocate_page() {
  if (freelist_head_ != kNullPage) {
    const PageId id = freelist_head_;
    Page p;
    read_page(id, p);
    check_that(p.type() == PageType::kFree,
               "free-list contains a page not marked free");
    freelist_head_ = p.next_page();
    store_meta();
    return id;
  }
  // Free list empty: grow the file by one zeroed page so the file size
  // and the meta page never disagree.
  const PageId id = page_count_;
  Page zero;
  pwrite_exact(fd_, zero.data(), kPageSize, page_offset(id));
  ++page_count_;
  store_meta();
  return id;
}

void Pager::free_page(PageId id) {
  check_that(id != 0 && id < page_count_, "free_page: page id out of range");
  {
    Page current;
    read_page(id, current);
    check_that(current.type() != PageType::kFree,
               "free_page: double free (page is already on the free list)");
  }
  Page p;
  p.set_id(id);
  p.set_type(PageType::kFree);
  p.set_next_page(freelist_head_);
  write_page(id, p);
  freelist_head_ = id;
  store_meta();
}

void Pager::sync() {
  store_meta();
  check_sys(::fsync(fd_) == 0, "fsync");
}

std::uint32_t Pager::freelist_length() {
  std::uint32_t n = 0;
  PageId id = freelist_head_;
  Page p;
  while (id != kNullPage) {
    ++n;
    check_that(n <= page_count_, "free list contains a cycle");
    read_page(id, p);
    check_that(p.type() == PageType::kFree,
               "free-list contains a page not marked free");
    id = p.next_page();
  }
  return n;
}

void Pager::store_meta() {
  Page meta;
  meta.set_id(0);
  meta.set_type(PageType::kMeta);
  std::memcpy(meta.data() + kMetaMagicOff, kMagic, sizeof kMagic);
  store_u32(meta.data() + kMetaPageCountOff, page_count_);
  store_u32(meta.data() + kMetaFreelistOff, freelist_head_);
  pwrite_exact(fd_, meta.data(), kPageSize, 0);
}

}  // namespace labdb
