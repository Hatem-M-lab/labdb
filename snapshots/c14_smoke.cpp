// Compile + smoke-test the Challenge 1.4 snapshot of the pager (the
// extend() version, before the free list exists).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <span>

#include "harness.hpp"
#include "pager.hpp"
#include "slotted_page.hpp"

using namespace labdb;

int main() {
  const char* path = "u01_c14_smoke.db";
  ::unlink(path);
  {
    Pager pager(path);
    REQUIRE(pager.page_count() == 1);
    const PageId id = pager.extend();
    REQUIRE(id == 1);
    Page page;
    SlottedPage view(page);
    view.init(id);
    const char* msg = "hello, pager";
    REQUIRE(view.insert(std::span<const std::uint8_t>(
                            reinterpret_cast<const std::uint8_t*>(msg),
                            std::strlen(msg)))
                .has_value());
    pager.write_page(id, page);
    pager.sync();
  }
  {
    Pager pager(path);
    REQUIRE(pager.page_count() == 2);
    struct stat st{};
    REQUIRE(::stat(path, &st) == 0 && st.st_size == 2 * 4096);
    Page page;
    pager.read_page(1, page);
    SlottedPage view(page);
    auto r = view.read(0);
    REQUIRE(r && r->size() == 12);
    REQUIRE(std::memcmp(r->data(), "hello, pager", r->size()) == 0);
  }
  ::unlink(path);
  std::printf("c14 snapshot smoke test: PASS (page survives reopen, file = 8192 B)\n");
  return 0;
}
