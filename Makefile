# labdb -- everything builds with the book's exact flags. No dependencies
# beyond a C++20 compiler and POSIX.
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra
INC      := -Isrc -Itests
ENGINE   := src/io.cpp src/pager.cpp src/btree.cpp src/buffer_pool.cpp \
            src/heap.cpp src/catalog.cpp src/parser.cpp src/executor.cpp
HDRS     := $(wildcard src/*.hpp) tests/harness.hpp

TESTS := u01_page_layout_test u01_header_bench u01_slotted_test \
         u01_churn_bench u01_pager_test u01_fsync_bench \
         u01_scan_bench u01_torn_write_test \
         u02_btree_test u02_split_regression_test \
         u02_search_bench u02_insert_bench \
         u03_delete_test u03_scan_regression_test \
         u03_range_bench u03_delete_bench \
         u04_buffer_pool_test u04_cache_bench \
         u05_record_test u05_heap_test u05_end_to_end_bench \
         u06_catalog_test u06_catalog_bench \
         u07_parser_test u07_parse_bench \
         u08_sql_test u08_sql_bench

BINS := $(addprefix bin/,$(TESTS)) bin/u01_torn_write_demo \
        bin/u01_c12_smoke bin/u01_c14_smoke bin/u02_median_bug_demo \
        bin/u03_merge_chain_bug_demo bin/u04_lost_update_demo \
        bin/u05_null_shift_demo bin/u06_prefix_collision_demo \
        bin/u07_escape_swallow_demo bin/u08_key_range_demo

all: $(BINS)

bin:
	mkdir -p bin

bin/%: tests/%.cpp $(ENGINE) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE) -o $@

bin/u01_torn_write_demo: tools/u01_torn_write_demo.cpp | bin
	$(CXX) $(CXXFLAGS) $< -o $@

# In-memory forensic-trap demos: node views over Page objects, no pager, so
# they link nothing but need the engine headers.
bin/u02_median_bug_demo: tools/u02_median_bug_demo.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

bin/u03_merge_chain_bug_demo: tools/u03_merge_chain_bug_demo.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

# The record-layer trap demo works purely on encoded bytes in memory, so it
# links nothing but the record header.
bin/u05_null_shift_demo: tools/u05_null_shift_demo.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

# The prefix-collision demo drives a real catalog page, so it links the engine.
bin/u06_prefix_collision_demo: tools/u06_prefix_collision_demo.cpp $(ENGINE) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE) -o $@

# The escape-swallow demo works purely on strings via the header-only lexer.
bin/u07_escape_swallow_demo: tools/u07_escape_swallow_demo.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

# The key-range demo drives the real executor, so it links the engine.
bin/u08_key_range_demo: tools/u08_key_range_demo.cpp $(ENGINE) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE) -o $@

# The buffer-pool trap demo drives a real pool over a file, so unlike the
# in-memory demos above it links the engine.
bin/u04_lost_update_demo: tools/u04_lost_update_demo.cpp $(ENGINE) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE) -o $@

# Book snapshots: the intermediate Challenge 1.2 / 1.4 listings must also
# compile and pass smoke tests. -I order makes the snapshot header shadow
# the final one.
bin/u01_c12_smoke: snapshots/c12_smoke.cpp snapshots/c12/slotted_page.hpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -Isnapshots/c12 $(INC) $< -o $@

bin/u01_c14_smoke: snapshots/c14_smoke.cpp snapshots/c14/pager.cpp snapshots/c14/pager.hpp src/io.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -Isnapshots/c14 $(INC) $< snapshots/c14/pager.cpp src/io.cpp -o $@

test: bin/u01_page_layout_test bin/u01_slotted_test bin/u01_pager_test \
      bin/u01_torn_write_test bin/u01_c12_smoke bin/u01_c14_smoke \
      bin/u02_btree_test bin/u02_split_regression_test bin/u02_median_bug_demo \
      bin/u03_delete_test bin/u03_scan_regression_test bin/u03_merge_chain_bug_demo \
      bin/u04_buffer_pool_test bin/u04_lost_update_demo \
      bin/u05_record_test bin/u05_heap_test bin/u05_null_shift_demo \
      bin/u06_catalog_test bin/u06_prefix_collision_demo \
      bin/u07_parser_test bin/u07_escape_swallow_demo \
      bin/u08_sql_test bin/u08_key_range_demo
	./bin/u01_page_layout_test
	./bin/u01_slotted_test
	./bin/u01_pager_test
	./bin/u01_torn_write_test
	./bin/u01_c12_smoke
	./bin/u01_c14_smoke
	./bin/u02_btree_test
	./bin/u02_split_regression_test
	./bin/u02_median_bug_demo
	./bin/u03_delete_test
	./bin/u03_scan_regression_test
	./bin/u03_merge_chain_bug_demo
	./bin/u04_buffer_pool_test
	./bin/u04_lost_update_demo
	./bin/u05_record_test
	./bin/u05_heap_test
	./bin/u05_null_shift_demo
	./bin/u06_catalog_test
	./bin/u06_prefix_collision_demo
	./bin/u07_parser_test
	./bin/u07_escape_swallow_demo
	./bin/u08_sql_test
	./bin/u08_key_range_demo

bench: bin/u01_header_bench bin/u01_churn_bench bin/u01_fsync_bench \
       bin/u01_scan_bench bin/u02_search_bench bin/u02_insert_bench \
       bin/u03_range_bench bin/u03_delete_bench bin/u04_cache_bench \
       bin/u05_end_to_end_bench bin/u06_catalog_bench bin/u07_parse_bench \
       bin/u08_sql_bench
	./bin/u01_header_bench
	./bin/u01_churn_bench
	./bin/u01_fsync_bench
	./bin/u01_scan_bench
	./bin/u02_search_bench
	./bin/u02_insert_bench
	./bin/u03_range_bench
	./bin/u03_delete_bench
	./bin/u04_cache_bench
	./bin/u05_end_to_end_bench
	./bin/u06_catalog_bench
	./bin/u07_parse_bench
	./bin/u08_sql_bench

clean:
	rm -rf bin *.db *.bin *.img

.PHONY: all test bench clean
