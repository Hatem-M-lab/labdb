# labdb -- everything builds with the book's exact flags. No dependencies
# beyond a C++20 compiler and POSIX -- except Unit 14's comparisons with
# PostgreSQL and SQLite, which run when those are installed and are skipped
# when they are not.
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra
INC      := -Isrc -Itests
ENGINE   := src/io.cpp src/wal.cpp src/pager.cpp src/btree.cpp src/buffer_pool.cpp \
            src/heap.cpp src/catalog.cpp src/parser.cpp src/executor.cpp \
            src/operator.cpp
HDRS     := $(wildcard src/*.hpp) tests/harness.hpp tests/show.hpp tests/shop.hpp tests/anatomy.hpp \
            tests/interleave.hpp tests/crashsim.hpp tests/u13_workload.hpp tests/error_text.hpp
# Unit 13: the engine is compiled once, into objects, and every program is
# linked against them -- the same flags, the same code, a much shorter build.
OBJ      := $(patsubst src/%.cpp,obj/%.o,$(ENGINE))

# Unit 14: labdb against SQLite needs SQLite's library and header
# (libsqlite3-dev). Without them the benchmark is not built, and nothing else
# needs them.
HAVE_SQLITE := $(shell printf '\043include <sqlite3.h>\n' | $(CXX) -E -x c++ - >/dev/null 2>&1 && echo yes)
ifeq ($(HAVE_SQLITE),yes)
SQLITE_BENCH := bin/u14_sqlite_bench
endif

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
         u08_sql_test u08_sql_bench \
         u09_join_test u09_pipeline_bench u09_join_bench u09_walk_bench \
         u10_aggregate_test u10_sort_bench u10_group_bench \
         u11_index_test u11_range_bench u11_index_bench u11_cost_bench \
         u11_join_bench u11_session_bench \
         u12_batch_test u12_anatomy_bench u12_cost_bench u12_pipeline_bench \
         u12_group_bench u12_session_bench \
         u13_wal_test u13_crash_test u13_durability_bench \
         u14_pg_diff

BINS := $(addprefix bin/,$(TESTS)) bin/u01_torn_write_demo \
        bin/u01_c12_smoke bin/u01_c14_smoke bin/u02_median_bug_demo \
        bin/u03_merge_chain_bug_demo bin/u04_lost_update_demo \
        bin/u05_null_shift_demo bin/u06_prefix_collision_demo \
        bin/u07_escape_swallow_demo bin/u08_key_range_demo \
        bin/u09_hash_collapse_demo bin/u09_null_probe \
        bin/u10_null_as_zero_demo bin/u10_query_probe bin/u11_split_run_demo \
        bin/u11_insert_probe bin/u12_row_probe bin/u12_empty_batch_demo \
        bin/u13_stale_log_demo bin/labdb bin/u14_literal_probe

all: $(BINS) $(SQLITE_BENCH)

bin:
	mkdir -p bin

obj:
	mkdir -p obj

obj/%.o: src/%.cpp $(HDRS) | obj
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

bin/%: tests/%.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

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
bin/u06_prefix_collision_demo: tools/u06_prefix_collision_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The escape-swallow demo works purely on strings via the header-only lexer.
bin/u07_escape_swallow_demo: tools/u07_escape_swallow_demo.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@

# The key-range demo drives the real executor, so it links the engine.
bin/u08_key_range_demo: tools/u08_key_range_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The hash-collapse demo runs real joins over real tables, so it links the engine.
bin/u09_hash_collapse_demo: tools/u09_hash_collapse_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The NULL probe uses only run_sql, so it builds against any unit's executor.
bin/u09_null_probe: tools/u09_null_probe.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The NULL-as-zero demo runs real aggregates over a real table, so it links
# the engine; it prints its tables with tests/show.hpp.
bin/u10_null_as_zero_demo: tools/u10_null_as_zero_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The split-run demo runs real index scans over a real table, so it links the
# engine.
bin/u11_split_run_demo: tools/u11_split_run_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The insert probe runs INSERTs into a table with indexes, through run_sql.
bin/u11_insert_probe: tools/u11_insert_probe.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The empty-batch demo runs real plans in batches over the shop, so it links
# the engine.
bin/u12_empty_batch_demo: tools/u12_empty_batch_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The row probe runs one step of a scanned row (tests/anatomy.hpp) for
# scripts/u12_anatomy_counts.sh to count under valgrind.
bin/u12_row_probe: tools/u12_row_probe.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The query probe uses only Parser, plan and run_sql, so it builds against any
# unit's engine from Unit 9 on (scripts/u10_before_after.sh builds it twice).
bin/u10_query_probe: tools/u10_query_probe.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# The buffer-pool trap demo drives a real pool over a file, so unlike the
# in-memory demos above it links the engine.
bin/u04_lost_update_demo: tools/u04_lost_update_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

bin/u14_sqlite_bench: tests/u14_sqlite_bench.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -lsqlite3 -o $@

# Unit 14's trap probe uses only run_sql, so it builds against any unit's
# engine from Unit 8 on (scripts/u14_before_after.sh builds it twice).
bin/u14_literal_probe: tools/u14_literal_probe.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# Unit 14: the engine as a program -- SQL in, results out.
bin/labdb: tools/labdb.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

# Unit 13's forensic trap: a log reset without a new generation, replayed after a crash.
bin/u13_stale_log_demo: tools/u13_stale_log_demo.cpp $(OBJ) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(OBJ) -o $@

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
      bin/u08_sql_test bin/u08_key_range_demo \
      bin/u09_join_test bin/u09_hash_collapse_demo \
      bin/u10_aggregate_test bin/u10_null_as_zero_demo \
      bin/u11_index_test bin/u11_split_run_demo \
      bin/u12_batch_test bin/u12_empty_batch_demo \
      bin/u13_wal_test bin/u13_crash_test bin/u13_stale_log_demo \
      bin/u14_literal_probe bin/labdb bin/u14_pg_diff
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
	./bin/u09_join_test
	./bin/u09_hash_collapse_demo
	./bin/u10_aggregate_test
	./bin/u10_null_as_zero_demo
	./bin/u11_index_test
	./bin/u11_split_run_demo
	./bin/u12_batch_test
	./bin/u12_empty_batch_demo
	./bin/u13_wal_test
	./bin/u13_crash_test 25
	./bin/u13_stale_log_demo
	./bin/u14_literal_probe --check
	sh scripts/u14_shell_check.sh
	sh scripts/u14_postgres_check.sh 1000

bench: bin/u01_header_bench bin/u01_churn_bench bin/u01_fsync_bench \
       bin/u01_scan_bench bin/u02_search_bench bin/u02_insert_bench \
       bin/u03_range_bench bin/u03_delete_bench bin/u04_cache_bench \
       bin/u05_end_to_end_bench bin/u06_catalog_bench bin/u07_parse_bench \
       bin/u08_sql_bench bin/u09_pipeline_bench bin/u09_join_bench \
       bin/u09_walk_bench bin/u10_sort_bench bin/u10_group_bench \
       bin/u11_range_bench bin/u11_index_bench bin/u11_cost_bench \
       bin/u11_join_bench bin/u11_session_bench bin/u12_anatomy_bench \
       bin/u12_cost_bench bin/u12_pipeline_bench bin/u12_group_bench \
       bin/u12_session_bench bin/u13_durability_bench $(SQLITE_BENCH)
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
	./bin/u09_pipeline_bench
	./bin/u09_join_bench
	./bin/u09_walk_bench
	./bin/u10_sort_bench
	./bin/u10_group_bench
	./bin/u11_range_bench
	./bin/u11_index_bench
	./bin/u11_cost_bench
	./bin/u11_join_bench
	./bin/u11_session_bench
	./bin/u12_anatomy_bench
	./bin/u12_cost_bench
	./bin/u12_pipeline_bench
	./bin/u12_group_bench
	./bin/u12_session_bench
	./bin/u13_durability_bench
	@if [ -x bin/u14_sqlite_bench ]; then ./bin/u14_sqlite_bench; \
	 else echo "u14_sqlite_bench: SQLite's header and library (libsqlite3-dev) not found: skipped"; fi

clean:
	rm -rf bin obj *.db *.db-wal *.bin *.img

.PHONY: all test bench clean
