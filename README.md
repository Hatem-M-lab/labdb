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
Current state: Unit 1 (slotted pages and the pager).
