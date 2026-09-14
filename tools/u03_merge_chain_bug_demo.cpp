// THE BUG, preserved for study: merging two leaves without repairing the
// sibling chain. Deletion merges an under-full leaf into a neighbor and
// frees the emptied page. The survivor must inherit the freed leaf's next
// pointer, or the chain that range scans walk is severed -- and every key
// past the merge point silently vanishes from ordered scans, even though
// point lookups still find them.
//
// This program builds three chained leaves A -> B -> C, merges B into A the
// two competing ways, then walks the chain from A (a range scan) and counts
// what it can reach. No pager, no disk: three Page objects in memory.
//
// build: g++ -std=c++20 -O2 -Wall -Wextra -Isrc -Itests
//        tools/u03_merge_chain_bug_demo.cpp -o bin/u03_merge_chain_bug_demo
#include <cstdio>

#include "btree.hpp"
#include "page.hpp"

using namespace labdb;

namespace {

// Walk the leaf chain starting at `start`, in the array of pages, printing
// every key -- exactly what a full range scan does via the cursor.
int walk(Page* pages, PageId start) {
  int seen = 0;
  PageId id = start;
  std::printf("    scan: ");
  while (id != kNullPage) {
    LeafNode n(pages[id]);
    for (std::uint16_t i = 0; i < n.count(); ++i) {
      std::printf("%llu ", (unsigned long long)n.key_at(i));
      ++seen;
    }
    id = n.next();
  }
  std::printf("\n");
  return seen;
}

// Build A(1,2)->B(3,4)->C(5,6) in pages[1..3].
void build(Page* pages) {
  LeafNode a(pages[1]), b(pages[2]), c(pages[3]);
  a.init(1);
  b.init(2);
  c.init(3);
  a.insert_at(0, 1, 10);
  a.insert_at(1, 2, 20);
  b.insert_at(0, 3, 30);
  b.insert_at(1, 4, 40);
  c.insert_at(0, 5, 50);
  c.insert_at(1, 6, 60);
  a.set_next(2);
  b.set_next(3);
  c.set_next(kNullPage);
}

}  // namespace

int main() {
  std::printf("Three leaves chained in key order: A(1,2) -> B(3,4) -> C(5,6). "
              "We delete from B until it merges into A.\n\n");

  // ================= THE BUGGY WAY: merge, forget the splice ============
  {
    Page pages[4];
    build(pages);
    LeafNode a(pages[1]), b(pages[2]);
    a.append_from(b);  // A becomes 1,2,3,4 ...
    // ...but A.next is left pointing at B. B is now freed; simulate the page
    // being recycled by clearing it, which drops its link to C.
    b.init(2);         // "freed": empty, next = kNullPage
    std::printf("  buggy merge (A.next NOT updated):\n");
    const int seen = walk(pages, 1);
    std::printf("    reached %d of 6 keys%s\n\n", seen,
                seen < 6 ? "  <-- keys 5 and 6 lost from the scan" : "");
  }

  // ================= THE CORRECT WAY: splice the chain ==================
  {
    Page pages[4];
    build(pages);
    LeafNode a(pages[1]), b(pages[2]);
    a.append_from(b);
    a.set_next(b.next());  // A inherits B's next (= C): the chain is repaired
    b.init(2);             // B freed
    std::printf("  correct merge (A.next = B.next = C):\n");
    const int seen = walk(pages, 1);
    std::printf("    reached %d of 6 keys\n", seen);
  }

  return 0;
}
