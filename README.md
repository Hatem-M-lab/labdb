# labdb

> **This is the companion source code for the book**
> **[Build a SQL Database Engine in C++ — Through Challenges](https://leanpub.com/buildasqldatabaseengineinc)**,
> covering Part I (Storage, Units 1–4), Part II (From Bytes to a Query,
> Units 5–8), and Part III (Making It Fast & Real, Units 9–12). Every listing
> in the book compiles exactly as printed; this repository is that code, one
> unit at a time.
>
> **Following along with the book?** `git checkout unit-04` (or any
> `unit-01` through `unit-12`) to get the exact code state that unit's
> chapter shows. The `main` branch always has the latest, full engine.

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
Current state: Unit 12 (vectorized execution) -- the engine joins tables, groups and summarizes them (`GROUP BY`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `HAVING`), orders and limits results (`ORDER BY`, `LIMIT`), has secondary indexes (`CREATE INDEX`) and a cost-based planner, and runs its plans in batches of 1,024 rows held as columns -- all but the smallest, which run a row at a time -- reading rows where they lie in the buffer pool.

- Unit 1 -- slotted pages and the pager: self-describing 4 KiB pages,
  variable-length records with stable slot ids, a pager with an intrusive
  free list, exact-or-die I/O.
- Unit 2 -- B+Tree insert and search: a logarithmic index over the pager.
  Point lookup among a million keys reads 3 pages instead of scanning 16,950.
- Unit 3 -- B+Tree delete and range scans: deletion with borrow/merge/
  root-collapse (freed pages return to the free list), and ordered range
  scans via a leaf-chain cursor whose cost scales with the result.
- Unit 4 -- the buffer pool: a fixed set of in-memory frames cached over the
  pager, with a clock (second-chance) replacement policy, pinning, and dirty
  write-back. The B+Tree sits on it by a change of one type. A lookup still
  costs 3 logical reads but touches the disk a fraction as often; the same
  workload runs ~2.5x faster warm. Run `sh scripts/run_unit04.sh` for the
  numbers, or `./bin/u04_lost_update_demo` for the forensic trap.
- Unit 5 -- the record layer: typed, variable-length rows. A schema and a
  fixed-slot codec (integers, booleans, nulls, text) encode rows to bytes and
  back, and can project one column without decoding the rest. A table heap
  stores rows as cells in slotted pages (revived from Unit 1), addressed by a
  RID that packs into the B+Tree's value -- so a key now finds a real typed
  row: 200,000 keyed lookups, 0 mismatches. Run `sh scripts/run_unit05.sh`, or
  `./bin/u05_null_shift_demo` for the forensic trap.
- Unit 6 -- the catalog: named tables. A fixed-slot directory page maps each
  table's name to its schema and its own heap head and B+Tree root, bootstrapped
  from a third meta-page root and durable across reopen. Heap and BTree each
  gained a small callback so a per-table root persists into the catalog instead
  of the one global slot. Rows are now inserted and looked up BY TABLE NAME:
  5 tables, 500k rows, 0 mismatches, at one extra page read per lookup. Run
  `sh scripts/run_unit06.sh`, or `./bin/u06_prefix_collision_demo` for the trap.
- Unit 7 -- the front end: a SQL tokenizer and recursive-descent parser. Text
  like `SELECT name FROM users WHERE id = 42` becomes an abstract syntax tree
  whose nodes carry engine types (a Schema for CREATE, a Row of Fields for
  INSERT). Case-insensitive keywords, the '' string escape, and compiler-quality
  caret-pointed errors. ~1M statements/s, every statement round-trips through a
  pretty-printer. No page code changed -- the front end sits entirely above the
  engine. Run `sh scripts/run_unit07.sh`, or `./bin/u07_escape_swallow_demo`.
- Unit 8 -- the executor (Part II milestone): SQL, end to end. An executor walks
  the parser's AST into engine calls -- CREATE registers a schema, INSERT
  reconciles literals to column types and stores a row, SELECT resolves a
  projection, chooses a point lookup or a scan, applies WHERE, and shapes the
  result. `run_sql("SELECT name FROM users WHERE id = 42")` returns the row,
  across all seven layers beneath. 220k rows built and queried in SQL; indexed
  lookups ~2.4us; a cold reopen answers the same. Run `sh scripts/run_unit08.sh`,
  or `./bin/u08_key_range_demo` for the trap.
- Unit 9 -- joins (Part III begins): the executor rebuilt as a pipeline of
  open/next/close operators (SeqScan, IndexLookup, Filter, Project), then
  `JOIN ... ON` with table aliases, qualified columns, and AND. Two joins: a
  nested loop (any condition, |L| x |R| comparisons) and a hash join (build a
  table of one side, probe it with the other), proven identical by a
  differential test. At 100,000 rows a side the hash join is ~1,300x faster.
  Run `sh scripts/run_unit09.sh` (about 5 minutes), or
  `./bin/u09_hash_collapse_demo` for the forensic trap: a hash join that is
  right on every test and 67x slower on one real key column.
- Unit 10 -- aggregation: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `GROUP BY`
  through a hash table of groups, `HAVING`, `ORDER BY` with a top-n heap, and
  `LIMIT` that stops the pipeline early. Run `sh scripts/run_unit10.sh`, or
  `./bin/u10_null_as_zero_demo` for the forensic trap.
- Unit 11 -- indexes and the planner: ranges of the key, `CREATE INDEX` over a
  B+Tree that keeps duplicate keys, a cost model with constants measured on
  the machine, and joins planned by cost (build side, index nested-loop join).
  Run `sh scripts/run_unit11.sh`, or `./bin/u11_split_run_demo` for the trap.
- Unit 12 -- vectorized execution (Part III ends): rows read where they lie in
  the buffer pool instead of copied out a page at a time, then every operator
  given a batch path beside its row path -- 1,024 rows a call, one array per
  column, filters that mark rows instead of moving them. A scanned row costs a
  tenth of what it did, the index break-even moves from half the table to a
  sixth, a group shrinks from 224 bytes to 51, and a plan too small to repay a
  batch runs a row at a time.
  Run `sh scripts/run_unit12.sh`, or `./bin/u12_empty_batch_demo` for the trap:
  a filter whose answer depends on the batch size.
