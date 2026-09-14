// Challenges 1.4 + 1.5 correctness: persistence across reopen, allocation,
// LIFO free-list reuse, double-free detection, and file-size accounting.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "harness.hpp"
#include "page.hpp"
#include "pager.hpp"
#include "slotted_page.hpp"

using namespace labdb;

namespace {

off_t file_size(const std::string& path) {
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_size;
}

std::vector<std::uint8_t> marker(std::uint32_t id) {
  std::vector<std::uint8_t> r(32);
  for (std::size_t i = 0; i < r.size(); ++i)
    r[i] = static_cast<std::uint8_t>(id * 131 + i * 7);
  return r;
}

}  // namespace

int main() {
  const std::string path = "u01_pager_test.db";
  ::unlink(path.c_str());

  // ---- create, allocate, write ----
  {
    Pager pager(path);
    REQUIRE(pager.page_count() == 1);  // just the meta page
    REQUIRE(file_size(path) == 4096);

    for (PageId want = 1; want <= 5; ++want) {
      const PageId id = pager.allocate_page();
      REQUIRE(id == want);  // fresh file: sequential ids
      Page page;
      SlottedPage view(page);
      view.init(id);
      auto m = marker(id);
      REQUIRE(view.insert(m).has_value());
      pager.write_page(id, page);
    }
    REQUIRE(pager.page_count() == 6);
    REQUIRE(file_size(path) == 6 * 4096);
    pager.sync();
  }  // destructor closes the file
  std::printf("PASS create+allocate: 5 data pages, file is %lld bytes\n",
              static_cast<long long>(file_size(path)));

  // ---- reopen: contents survived ----
  {
    Pager pager(path);
    REQUIRE(pager.page_count() == 6);
    for (PageId id = 1; id <= 5; ++id) {
      Page page;
      pager.read_page(id, page);
      REQUIRE(page.id() == id);
      REQUIRE(page.type() == PageType::kSlotted);
      SlottedPage view(page);
      auto got = view.read(0);
      auto expect = marker(id);
      REQUIRE(got && got->size() == expect.size());
      REQUIRE(std::memcmp(got->data(), expect.data(), expect.size()) == 0);
    }
    std::printf("PASS reopen: all 5 pages read back intact\n");

    // ---- free two pages: the file does NOT shrink ----
    pager.free_page(2);
    pager.free_page(4);
    REQUIRE(pager.freelist_head() == 4);  // LIFO
    REQUIRE(pager.freelist_length() == 2);
    REQUIRE(file_size(path) == 6 * 4096);
    std::printf("PASS free: free list = [4 -> 2], file still %lld bytes\n",
                static_cast<long long>(file_size(path)));

    // ---- double free is caught loudly ----
    bool caught = false;
    try {
      pager.free_page(4);
    } catch (const std::exception& e) {
      caught = true;
      std::printf("PASS double-free detected: %s\n", e.what());
    }
    REQUIRE(caught);
  }

  // ---- reopen again: the free list itself is durable ----
  {
    Pager pager(path);
    REQUIRE(pager.freelist_length() == 2);
    REQUIRE(pager.allocate_page() == 4);  // LIFO reuse, no growth
    REQUIRE(pager.allocate_page() == 2);
    REQUIRE(file_size(path) == 6 * 4096);
    REQUIRE(pager.allocate_page() == 6);  // list empty: extend
    REQUIRE(file_size(path) == 7 * 4096);
    std::printf(
        "PASS reuse: allocations returned 4, 2 (recycled), then 6 (extend)\n");
    std::printf("PASS file size: 24576 B during reuse, 28672 B after extend\n");
  }

  ::unlink(path.c_str());
  std::printf("u01_pager_test: PASS\n");
  return 0;
}
