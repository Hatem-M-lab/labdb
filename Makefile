# labdb -- everything builds with the book's exact flags. No dependencies
# beyond a C++20 compiler and POSIX.
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra
INC      := -Isrc -Itests
ENGINE   := src/io.cpp src/pager.cpp
HDRS     := $(wildcard src/*.hpp) tests/harness.hpp

TESTS := u01_page_layout_test u01_header_bench u01_slotted_test \
         u01_churn_bench u01_pager_test u01_fsync_bench \
         u01_scan_bench u01_torn_write_test

BINS := $(addprefix bin/,$(TESTS)) bin/u01_torn_write_demo \
        bin/u01_c12_smoke bin/u01_c14_smoke

all: $(BINS)

bin:
	mkdir -p bin

bin/%: tests/%.cpp $(ENGINE) $(HDRS) | bin
	$(CXX) $(CXXFLAGS) $(INC) $< $(ENGINE) -o $@

bin/u01_torn_write_demo: tools/u01_torn_write_demo.cpp | bin
	$(CXX) $(CXXFLAGS) $< -o $@

# Book snapshots: the intermediate Challenge 1.2 / 1.4 listings must also
# compile and pass smoke tests. -I order makes the snapshot header shadow
# the final one.
bin/u01_c12_smoke: snapshots/c12_smoke.cpp snapshots/c12/slotted_page.hpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -Isnapshots/c12 $(INC) $< -o $@

bin/u01_c14_smoke: snapshots/c14_smoke.cpp snapshots/c14/pager.cpp snapshots/c14/pager.hpp src/io.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -Isnapshots/c14 $(INC) $< snapshots/c14/pager.cpp src/io.cpp -o $@

test: bin/u01_page_layout_test bin/u01_slotted_test bin/u01_pager_test \
      bin/u01_torn_write_test bin/u01_c12_smoke bin/u01_c14_smoke
	./bin/u01_page_layout_test
	./bin/u01_slotted_test
	./bin/u01_pager_test
	./bin/u01_torn_write_test
	./bin/u01_c12_smoke
	./bin/u01_c14_smoke

bench: bin/u01_header_bench bin/u01_churn_bench bin/u01_fsync_bench bin/u01_scan_bench
	./bin/u01_header_bench
	./bin/u01_churn_bench
	./bin/u01_fsync_bench
	./bin/u01_scan_bench

clean:
	rm -rf bin *.db *.bin *.img

.PHONY: all test bench clean
