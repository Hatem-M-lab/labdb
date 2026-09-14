# labdb

The SQL database engine built, one challenge at a time, in *Build a SQL
Database Engine in C++ -- Through Challenges*. C++20, standard library and
POSIX only. Everything compiles with:

    g++ -std=c++20 -O2 -Wall -Wextra

Layout:

    src/        the engine (grows monotonically, unit by unit)
    tests/      per-unit correctness tests and measurement programs
    tools/      forensic-trap demos and inspection tools
    snapshots/  mid-unit versions of files that later challenges replace,
                kept so every listing in the book compiles as printed
    scripts/    one-command reproduction of each unit's results

Commands: `make all`, `make test`, `make bench`, `make clean`.
Current state: Unit 2 (B+Tree insert and search).

- Unit 1 -- slotted pages and the pager: self-describing 4 KiB pages,
  variable-length records with stable slot ids, a pager with an intrusive
  free list, exact-or-die I/O.
- Unit 2 -- B+Tree insert and search: a logarithmic index over the pager
  (src/btree.{hpp,cpp}). Point lookup among a million keys reads 3 pages
  instead of scanning 16,950. Run `sh scripts/run_unit02.sh` for the
  numbers, or `./bin/u02_median_bug_demo` for the forensic trap.
