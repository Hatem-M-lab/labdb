#pragma once
// Operators -- a query plan as a tree of iterators. Unit 9; aggregation,
// sorting and LIMIT added in Unit 10; index scans and the index nested-loop
// join in Unit 11.
//
// Unit 8 ran a SELECT as one function: scan the table, and for each row test
// the WHERE and copy out the requested columns. That shape cannot hold a
// join, which reads two tables at once. So the executor's work is now split
// into operators, each doing one job behind one interface:
//
//     open()     get ready: open the inputs, build anything that must be built
//     next(r)    put the next row in r; return false when there are no more
//     close()    release what open() acquired
//
// A plan is a tree of these. A parent pulls rows from its children one at a
// time by calling their next(); no operator knows what sits above or below
// it, so they compose freely -- a Filter over a SeqScan, a HashJoin over two
// scans, a Project over the join. This is the iterator, or "Volcano", model
// (Graefe, 1994), and most row-at-a-time engines are built this way. Its
// price is a virtual call per row per operator, measured in Challenge 9.1.
//
// Unit 12 gives every operator a second way to run, beside the first:
//
//     next_batch(b)   put up to a thousand rows in b, column by column
//                     (batch.hpp); return false when there are no more
//
// A plan runs one way or the other, all of it (set_batch_rows). The row
// path is Unit 9's, unchanged, and kept so the two can be measured side by
// side; the batch path is what the executor runs. An operator with no batch
// path of its own (an index lookup, a nested loop) gets one for free: the
// base class fills batches from its rows, and reads its inputs' batches back
// into rows for it. A batch handed out always holds at least one live row --
// an empty batch is never a result, and Challenge 12.5 is what happens when
// one is taken for the end.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "batch.hpp"
#include "catalog.hpp"
#include "join_hash.hpp"
#include "record.hpp"

namespace labdb {

// A semantic error: SQL that is well-formed but cannot run against this
// catalog -- an unknown table or column, an ambiguous column, a type
// mismatch. Defined here, not in the executor, because planning raises it.
struct ExecError {
  std::string message;
};

// One column of an operator's output: the label of the table it came from
// (the alias the query gave it, or its name) and the column itself. Which
// column `o.customer` or `name` refers to is decided against these.
struct OutColumn {
  std::string table;
  Column column;  // name, type, nullable
};
using OutSchema = std::vector<OutColumn>;

// The output schema of a join: the left input's columns, then the right's.
OutSchema concat(const OutSchema& left, const OutSchema& right);

// A table's columns as an operator outputs them, labeled with the name the
// query uses for the table (its alias, or its name).
OutSchema table_schema(const Schema& s, const TableRef& t);

// The position of a column reference in a schema. `t.c` must match both the
// table label and the column name; a bare `c` must match exactly one column,
// so a name two joined tables share has to be qualified. Throws ExecError.
std::size_t resolve(const OutSchema& s, const ColumnRef& ref);

// A comparison bound to column positions -- what an operator evaluates per
// row, with every name already resolved and the literal already coerced to
// the left column's type, once, at plan time.
struct BoundComparison {
  std::size_t left = 0;
  TokenType op = TokenType::kEq;
  bool as_text = false;          // compare as strings (else by integer value)
  bool as_double = false;        // Unit 10: compare as doubles (a side is DOUBLE)
  bool right_is_column = false;
  std::size_t right = 0;         // a column position, when right_is_column
  Field literal;                 // otherwise
};

// Unit 10: -1, 0, or 1 for two non-NULL values of one column's type: text by
// bytes, DOUBLE by value, everything else by integer value. Used by MIN, MAX,
// GROUP BY's key comparison, and ORDER BY.
int compare_values(const Field& a, const Field& b);

// Evaluate against one row. A NULL on either side satisfies no comparison:
// in SQL, NULL = NULL is not true, and neither is NULL < 5.
bool eval(const BoundComparison& c, const Row& row);

// The same, for a pair of rows a join is considering: positions below
// `nleft` are read from `l`, the rest from `r`, so no joined row has to be
// built just to find out that it does not qualify.
bool eval_pair(const BoundComparison& c, const Row& l, const Row& r,
               std::size_t nleft);

// Unit 12: eval() for a whole batch. Of the batch's live rows, keep those for
// which `c` holds, and mark the batch selected. The comparison is typed once
// for the batch -- integers, doubles or text; a column or a literal on the
// right -- and then run as one loop over the rows, with the same NULL rule.
void select(const BoundComparison& c, Batch& b);

// --------------------------------------------------------------- Operator
class BatchReader;

class Operator {
 public:
  virtual ~Operator() = default;

  virtual void open() = 0;
  // Put the next row in `out`; false when exhausted. Not virtual itself: it
  // counts the rows each operator produces (for plan_text) and forwards to
  // produce(), so there is still exactly one virtual call per row.
  bool next(Row& out) {
    if (!produce(out)) return false;
    ++rows_out_;
    return true;
  }
  // Unit 12: put the next rows in `out` -- at least one live row, at most
  // `max` -- and return true; or return false when there are no more. Counts
  // them, as next() counts its row: one virtual call per batch.
  bool next_batch(Batch& out, std::size_t max = SIZE_MAX) {
    if (!produce_batch(out, max)) return false;
    check_that(out.count() > 0 && out.count() <= max,
               "a batch handed out must hold at least one row, and no more than asked for");
    rows_out_ += out.count();
    return true;
  }
  virtual void close() = 0;

  // Unit 12: run this operator, and every operator below it, in batches of up
  // to `n` rows; 0 runs them a row at a time, as Units 9 to 11 did. Set before
  // open(). An operator built and drained directly runs a row at a time.
  void set_batch_rows(std::size_t n);
  std::size_t batch_rows() const { return batch_rows_; }

  const OutSchema& schema() const { return schema_; }
  std::uint64_t rows_out() const { return rows_out_; }

  // One line saying what this operator does, for plan_text, and optionally
  // a second line of what it measured while running (a join's table sizes).
  virtual std::string describe() const = 0;
  virtual std::string detail() const { return {}; }
  // The operators this one reads from, in order (Unit 12: kept by the base
  // class, which sets a whole plan's batch size through them; a view of them,
  // so that walking a plan allocates nothing).
  std::span<Operator* const> children() const { return inputs_; }

  // Unit 11: how many rows the planner expected this operator to produce, when
  // it estimated -- printed beside rows_out() by plan_text(op, true).
  std::optional<double> estimate() const { return estimate_; }
  void set_estimate(double rows) { estimate_ = rows; }

 protected:
  virtual bool produce(Row& out) = 0;
  // Unit 12: the batch path. The default is the adapter that lets an operator
  // with only a row path run inside a batch plan: rows from produce(), packed
  // into a batch. Operators with a batch path of their own override it.
  virtual bool produce_batch(Batch& out, std::size_t max);
  // Unit 12: one row from input `in`, the way this plan runs -- from in.next(),
  // or, in a batch plan, out of in's batches through `reader`. What the
  // operators that still work a row at a time read their inputs with.
  bool pull(Operator& in, BatchReader& reader, Row& out);
  // Unit 12: the rows this call may produce -- at most `max`, and at most the
  // batch size -- and the column types of schema_, what a batch of this
  // operator's rows is formatted with. A batch is formatted for the rows about
  // to be written, when that is known, and grows as it is filled when it is
  // not (Batch::ensure): its columns are as long as the rows it holds.
  std::size_t cap(std::size_t max) const { return max < batch_rows_ ? max : batch_rows_; }
  const std::vector<ColType>& types() const { return types_; }

  OutSchema schema_;
  std::vector<Operator*> inputs_;  // set by each constructor; owned by the subclass
  std::uint64_t rows_out_ = 0;
  std::optional<double> estimate_;
  std::size_t batch_rows_ = 0;
  std::vector<ColType> types_;  // schema_'s column types, set with the batch size
  Row pack_;                    // the default produce_batch's row
};
using OpPtr = std::unique_ptr<Operator>;

// Unit 12: rows read out of an input's batches, one at a time -- the other
// half of the adapter, for an operator with only a row path that reads an
// input running in batches.
class BatchReader {
 public:
  bool next(Operator& in, Row& out) {
    while (k_ >= batch_.count()) {
      if (!in.next_batch(batch_)) return false;
      k_ = 0;
    }
    batch_.get_row(batch_.row(k_++), out);
    return true;
  }
  void reset() {
    batch_.size = 0;
    batch_.selected = false;
    k_ = 0;
  }

 private:
  Batch batch_;
  std::size_t k_ = 0;
};

// The operator tree, one operator per line, children indented under their
// parent, each with the number of rows it produced -- and, with `estimates`
// (Unit 11), the number the planner expected.
std::string plan_text(const Operator& root, bool estimates = false);

// ---------------------------------------------------------------- sources

// Every row of a table, in key order. The planner passes the schema it has
// already read; the two-argument form reads it from the catalog itself.
class SeqScan : public Operator {
 public:
  SeqScan(Catalog& cat, const TableRef& table, OutSchema schema);
  SeqScan(Catalog& cat, const TableRef& table);
  void open() override;
  void close() override;
  std::string describe() const override;

 protected:
  bool produce(Row& out) override;
  bool produce_batch(Batch& out, std::size_t max) override;  // decoded from pinned pages

 private:
  Catalog& cat_;
  TableRef table_;
  std::optional<TableCursor> cur_;
};

// The one row whose key equals a value, through the table's B+Tree -- Unit
// 8's point-lookup plan, as an operator. An empty key means "no row" (the
// query asked for key = NULL).
class IndexLookup : public Operator {
 public:
  IndexLookup(Catalog& cat, const TableRef& table, OutSchema schema,
              std::optional<Key> key);
  void open() override { done_ = false; }
  void close() override {}
  std::string describe() const override;

 protected:
  bool produce(Row& out) override;

 private:
  Catalog& cat_;
  TableRef table_;
  std::optional<Key> key_;
  bool done_ = false;
};

// Unit 11: the rows whose value in one indexed column has a key in [lo, hi]
// (keys are order_key's), through that column's index: the table's own B+Tree
// for the key (column 0), a secondary index for any other column. The index is
// read in key order and each entry's row fetched from the heap by its RID, so
// rows come out in the column's order -- and the heap is visited in whatever
// order that is. lo > hi is an empty range: nothing is read at all. `rule`
// overrides the index's descent rule (Challenge 11.5); nullptr keeps its own.
class IndexScan : public Operator {
 public:
  IndexScan(Catalog& cat, const TableRef& table, OutSchema schema, std::size_t col,
            Key lo, Key hi, std::string range_text, ChildRule rule = nullptr);
  void open() override;
  void close() override { cur_.reset(); }
  std::string describe() const override;

 protected:
  bool produce(Row& out) override { return cur_ && cur_->next(out); }
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  Catalog& cat_;
  TableRef table_;
  std::size_t col_;
  Key lo_, hi_;
  std::string range_text_;
  ChildRule rule_;
  std::optional<IndexCursor> cur_;
};

// Rows already in memory. Lets a pipeline be measured with no storage under
// it (Challenge 9.1), and lets tests feed operators exact inputs.
class RowsScan : public Operator {
 public:
  RowsScan(OutSchema schema, const std::vector<Row>& rows);
  void open() override { i_ = 0; }
  void close() override {}
  std::string describe() const override;

 protected:
  bool produce(Row& out) override;

 private:
  const std::vector<Row>& rows_;
  std::size_t i_ = 0;
};

// Unit 12: RowsScan's twin for batches -- `rows` rows in memory, held as
// columns, the layout a batch has. A batch plan over it measures the
// operators with no storage under them, as RowsScan does for the row path.
class ColumnsScan : public Operator {
 public:
  ColumnsScan(OutSchema schema, const std::vector<ColumnVector>& cols, std::size_t rows);
  void open() override { i_ = 0; }
  void close() override {}
  std::string describe() const override;

 protected:
  bool produce(Row& out) override;
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  const std::vector<ColumnVector>& cols_;
  std::size_t rows_;
  std::size_t i_ = 0;
};

// --------------------------------------------------------------- one input

// The input rows that satisfy every comparison (they are AND-ed).
class Filter : public Operator {
 public:
  Filter(OpPtr child, std::vector<BoundComparison> preds, std::string text);
  void open() override { child_->open(); }
  void close() override { child_->close(); }
  std::string describe() const override { return "Filter " + text_; }

 protected:
  bool produce(Row& out) override;
  bool produce_batch(Batch& out, std::size_t max) override;  // select() per term

 private:
  OpPtr child_;
  std::vector<BoundComparison> preds_;
  std::string text_;
};

// Each input row cut down to the chosen columns, in the chosen order.
class Project : public Operator {
 public:
  Project(OpPtr child, std::vector<std::size_t> cols, std::string text);
  void open() override { child_->open(); }
  void close() override { child_->close(); }
  std::string describe() const override { return "Project " + text_; }

 protected:
  bool produce(Row& out) override;
  bool produce_batch(Batch& out, std::size_t max) override;  // columns moved, not copied

 private:
  OpPtr child_;
  std::vector<std::size_t> cols_;
  std::string text_;
  Row in_;
  Batch in_batch_;
};

// -------------------------------------------------------------- two inputs

// For every left row, every right row that satisfies the ON condition. The
// right input is read once, in open(), and kept in memory, so the loop pays
// only for comparisons: |left| x |right| of them, whatever the data.
class NestedLoopJoin : public Operator {
 public:
  // `on` is bound against concat(left schema, right schema).
  NestedLoopJoin(OpPtr left, OpPtr right, std::vector<BoundComparison> on,
                 std::string text);
  void open() override;
  void close() override;
  std::string describe() const override;
  std::string detail() const override;

 protected:
  bool produce(Row& out) override;

 private:
  OpPtr left_, right_;
  std::vector<BoundComparison> on_;
  std::string text_;
  std::size_t nleft_;
  std::vector<Row> inner_;  // the right input, buffered
  Row outer_;
  bool have_outer_ = false;
  std::size_t j_ = 0;
  BatchReader lread_, rread_;  // Unit 12: its inputs' batches, read back as rows
};

// Equality join through a hash table. open() reads the right input (the
// build side) into a JoinHashTable keyed on its join columns; then each left
// row (the probe side) is hashed and compared only with the rows in its
// bucket. `keys` pairs a left position with a right position (right
// positions are relative to the right input); `residual` holds any other ON
// comparisons, checked after the keys match. Unit 11: with `build_left` the
// roles swap -- the left input is hashed and the right one streamed past it --
// and the output is still the left row's fields, then the right's.
class HashJoin : public Operator {
 public:
  using KeyPair = std::pair<std::size_t, std::size_t>;
  HashJoin(OpPtr left, OpPtr right, std::vector<KeyPair> keys,
           std::vector<BoundComparison> residual, std::string text,
           JoinHashTable::MixFn mix = mix64, bool build_left = false);
  void open() override;
  void close() override;
  std::string describe() const override;
  std::string detail() const override;
  const JoinHashTable& table() const { return table_; }

 protected:
  bool produce(Row& out) override;
  // Unit 12: the build side filed as columns; each probe batch hashed in one
  // pass, its matches collected as pairs of positions, the output gathered.
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  std::uint64_t key_hash(const Row& row, const std::vector<std::size_t>& cols) const;
  bool null_key(const Row& row, const std::vector<std::size_t>& cols) const;
  bool keys_equal(const Row& l, const Row& r) const;
  void build_batches();
  std::uint64_t batch_key_hash(const Batch& b, std::size_t i, const std::vector<std::size_t>& cols) const;

  OpPtr left_, right_;
  std::vector<std::size_t> lkeys_, rkeys_;
  std::vector<bool> key_text_;
  std::vector<BoundComparison> residual_;
  std::string text_;
  JoinHashTable::MixFn mix_;
  bool build_left_;
  std::size_t nleft_;
  JoinHashTable table_;
  Row outer_;
  bool probing_ = false;
  std::uint64_t probe_hash_ = 0;
  std::uint32_t pos_ = 0, end_ = 0;
  // Unit 12, the batch path: the build side's rows as columns (their hashes
  // and buckets in table_), the probe batch being worked through, and the
  // pairs of positions -- probe row, build row -- that matched.
  std::vector<ColumnVector> built_;
  Batch probe_batch_;
  std::vector<std::uint64_t> probe_hashes_;
  std::size_t pk_ = 0;  // the probe batch's live row being matched
  bool in_bucket_ = false;
  std::vector<std::uint32_t> match_probe_, match_build_;
};

// Unit 11: for each row of the outer input, the rows of a table whose indexed
// column equals the outer row's join column, found through that index -- one
// index search per outer row, and the table is never scanned. A row fetched
// from the table must still pass the table's own WHERE terms (`inner_filter`,
// bound against the table's columns) and the join's other terms (`residual`,
// bound against the output). The output is the outer row's fields, then the
// table's -- or, with `inner_first`, the table's and then the outer's, so that
// `a JOIN b` keeps a's columns first whichever side drives the join.
class IndexNestedLoopJoin : public Operator {
 public:
  IndexNestedLoopJoin(OpPtr outer, Catalog& cat, const TableRef& inner, OutSchema inner_schema,
                      std::size_t outer_col, std::size_t inner_col,
                      std::vector<BoundComparison> inner_filter,
                      std::vector<BoundComparison> residual, bool inner_first,
                      std::string text, ChildRule rule = nullptr);
  void open() override;
  void close() override;
  std::string describe() const override;
  std::string detail() const override;

 protected:
  bool produce(Row& out) override;
  // Unit 12: for each outer row of a batch, the index searched and its matches
  // decoded straight into a batch of the inner table's rows; then the output
  // gathered, outer columns and inner, as the hash join gathers its own.
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  OpPtr outer_;
  Catalog& cat_;
  TableRef inner_;
  std::size_t outer_col_, inner_col_;
  std::vector<BoundComparison> inner_filter_, residual_;
  bool inner_first_;
  std::string text_;
  ChildRule rule_;
  std::string index_name_;  // for describe(): orders(customer)
  bool text_key_;           // a TEXT key is a prefix: compare the strings too
  std::optional<IndexCursor> cur_;
  Row outer_row_, inner_row_;
  bool have_outer_ = false;
  std::uint64_t searches_ = 0, fetched_ = 0;
  BatchReader read_;  // Unit 12: the outer input's batches, read back as rows
  std::vector<ColType> inner_types_;
  Batch outer_batch_, inner_batch_;  // the batch path's outer rows, and its fetched inner rows
  std::size_t ok_ = 0;               // the outer batch's live row being searched for
  std::vector<std::uint32_t> match_outer_, oidx_, iidx_;
};

// ------------------------------------------------------ aggregation (Unit 10)

// One aggregate to compute: which function, over which input column.
struct AggSpec {
  AggFunc func = AggFunc::kCount;
  bool star = false;               // COUNT(*): reads no column
  std::size_t col = 0;             // input position of the argument
  ColType type = ColType::kInt64;  // the argument's type: MIN and MAX return it
};

// The running state of one aggregate in one group.
struct AggState {
  std::int64_t count = 0;  // values seen -- or rows, for COUNT(*)
  std::int64_t sum = 0;    // SUM and AVG
  Field best;              // MIN and MAX: the best value seen so far
};

// Fold one input value into a state. Every aggregate except COUNT(*) skips a
// NULL: COUNT(col) counts values, not rows, and a missing value is not a 0 to
// be summed, averaged, or compared. Challenge 10.4 is what forgetting that
// costs.
void agg_update(const AggSpec& a, AggState& s, const Field& v);

// A finished group's answer. Over no values at all, COUNT is 0 and every other
// aggregate is NULL.
Field agg_result(const AggSpec& a, const AggState& s);

// agg_result's type: COUNT and SUM are INT64, AVG is DOUBLE, MIN and MAX keep
// their argument's type.
ColType agg_type(const AggSpec& a);

// Unit 11: MIN and MAX of indexed columns over a whole table, read from the
// ends of their indexes instead of from every row. An index's first entry
// holds the column's smallest value and its last entry the largest; NULLs are
// not in the index, and MIN and MAX skip them anyway. A TEXT key is only a
// prefix, so for TEXT the entries sharing the end key are fetched and compared.
// The output is one row, one value per aggregate, named #0, #1, ... exactly as
// HashAggregate names its results, so the operators above cannot tell which
// of the two produced it.
class IndexMinMax : public Operator {
 public:
  IndexMinMax(Catalog& cat, const TableRef& table, std::vector<AggSpec> aggs, std::string text);
  void open() override;
  void close() override {}
  std::string describe() const override { return "IndexMinMax " + text_; }
  std::string detail() const override;

 protected:
  bool produce(Row& out) override;

 private:
  Catalog& cat_;
  TableRef table_;
  std::vector<AggSpec> aggs_;
  std::string text_;
  Row values_;
  bool done_ = false;
  std::uint64_t fetched_ = 0;
};

// GROUP BY through a hash table. open() reads the whole input and folds each
// row into its group's states; next() then hands out one row per group, in the
// order the groups first appeared: the group's key columns, then each
// aggregate's result, named "#0", "#1", ... for the planner to refer to. With
// no GROUP BY columns there is exactly one group, even over an empty input.
// Rows whose keys are NULL form one group, as SQL's GROUP BY requires.
//
// `update` folds one value into one state; it is agg_update everywhere except
// in Challenge 10.4's demonstration, which swaps in the version with the bug.
class HashAggregate : public Operator {
 public:
  using UpdateFn = void (*)(const AggSpec&, AggState&, const Field&);
  HashAggregate(OpPtr child, std::vector<std::size_t> group_cols,
                std::vector<AggSpec> aggs, std::string text,
                UpdateFn update = agg_update);
  void open() override;
  void close() override {}  // the input was read to its end, and closed, in open()
  std::string describe() const override { return "HashAggregate " + text_; }
  std::string detail() const override;
  std::size_t groups() const { return batch_rows_ ? ngroups_ : keys_.size(); }
  std::size_t slots() const { return slots_.size(); }
  std::size_t bytes() const;  // what the table holds, by sizeof (the allocator's overhead aside)

 protected:
  bool produce(Row& out) override;
  // Unit 12: each input batch's rows given their groups in one pass, then
  // each aggregate folded over the batch in a loop of its own, into states
  // kept as one array per aggregate.
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  std::uint64_t key_hash(const Row& row) const;
  bool same_key(const Row& row, std::uint32_t g) const;
  std::uint32_t find_or_add(const Row& row);
  void grow();
  void open_batches();
  std::uint32_t find_or_add(const Batch& b, std::size_t i, std::uint64_t h);
  bool same_key(const Batch& b, std::size_t i, std::uint32_t g) const;
  void fold(const AggSpec& a, std::size_t ai, const Batch& b);

  OpPtr child_;
  std::vector<std::size_t> group_cols_;
  std::vector<AggSpec> aggs_;
  std::string text_;
  UpdateFn update_;
  // Group g: its key fields keys_[g], its key's raw hash hashes_[g], and its
  // aggregate states at states_[g * aggs_.size()] onward.
  std::vector<Row> keys_;
  std::vector<std::uint64_t> hashes_;
  std::vector<AggState> states_;
  std::vector<std::uint32_t> slots_;  // open addressing: group id + 1, 0 if empty
  std::size_t mask_ = 0;
  std::size_t longest_probe_ = 0;
  std::size_t next_ = 0;  // the next group to hand out
  // Unit 12, the batch path. Group g's key is row g of gkeys_ (one column per
  // GROUP BY column) and its hash hashes_[g]; aggregate a's state for group g
  // is counts_[a][g], with sums_[a][g] for SUM and AVG and row g of best_[a]
  // for MIN and MAX -- only what each aggregate needs, 8 to 16 bytes a group.
  std::size_t ngroups_ = 0;
  std::vector<ColumnVector> gkeys_;
  std::vector<std::vector<std::int64_t>> counts_, sums_;
  std::vector<ColumnVector> best_;
  std::vector<std::uint32_t> gids_;  // the group of each live row of the batch being folded
  std::vector<std::uint64_t> batch_hashes_;  // ... and its key's hash
  bool int_key_ = false;  // one GROUP BY column, an integer one: keys compared in one step
};

// ------------------------------------------------- ORDER BY and LIMIT (Unit 10)

struct SortKey {
  std::size_t col = 0;
  bool desc = false;
};

// ORDER BY: read the whole input, then hand it out in order. NULLs sort after
// every value ascending and before every value descending, as PostgreSQL does
// by default, and rows that tie keep their input order, so the output is fully
// determined. With a limit n, only the best n rows are ever kept -- a bounded
// heap, O(N log n) time and O(n) memory, instead of sorting all N rows.
//
// What gets sorted is not the rows but small entries: a row's index and its
// first sort key folded into 64 bits whose unsigned order is the sort order
// (PostgreSQL's tuplesort carries its first key beside each tuple the same
// way). Two entries with different prefixes are ordered without touching
// their rows; only equal prefixes fall through to the full comparison.
// `prefixes` = false makes every prefix 0, so every comparison goes to the
// rows -- Challenge 10.3 measures the difference.
class Sort : public Operator {
 public:
  Sort(OpPtr child, std::vector<SortKey> keys, std::optional<std::size_t> limit,
       std::string text, bool prefixes = true);
  void open() override;
  void close() override {}  // the input was read to its end, and closed, in open()
  std::string describe() const override;
  std::string detail() const override;

 protected:
  bool produce(Row& out) override;
  bool produce_batch(Batch& out, std::size_t max) override;

 private:
  struct Entry {
    std::uint64_t prefix;  // the first key, folded: unsigned order = sort order
    std::uint32_t row;     // index into rows_
  };
  std::uint64_t prefix_of(const Row& r) const;
  bool before(std::uint32_t a, std::uint32_t b) const;  // rows_[a] sorts first?
  bool entry_before(const Entry& a, const Entry& b) const {
    return a.prefix != b.prefix ? a.prefix < b.prefix : before(a.row, b.row);
  }
  // Unit 12, the batch path: the rows held as columns (cols_), compared there.
  std::uint64_t prefix_of(const std::vector<ColumnVector>& c, std::size_t i) const;
  bool before_cols(std::uint32_t a, std::uint32_t b) const;
  bool entry_before_cols(const Entry& a, const Entry& b) const {
    return a.prefix != b.prefix ? a.prefix < b.prefix : before_cols(a.row, b.row);
  }
  void open_batches();

  OpPtr child_;
  std::vector<SortKey> keys_;
  std::optional<std::size_t> limit_;
  std::string text_;
  bool prefixes_;
  std::vector<Row> rows_;
  std::vector<std::uint64_t> seq_;  // each held row's input position: the tie-break
  std::vector<Entry> order_;
  std::size_t next_ = 0;
  std::uint64_t read_ = 0;
  std::size_t held_ = 0;
  std::uint64_t replaced_ = 0;  // top n: rows that displaced a held row
  std::vector<ColumnVector> cols_;  // Unit 12: the rows held, one column per input column
  std::vector<std::uint32_t> seq_index_;  // the rows a batch of output is gathered from
};

// LIMIT n: pass on at most n rows, then stop asking for more. A scan below it
// stops reading after n rows; a sort below it has already read everything.
class Limit : public Operator {
 public:
  Limit(OpPtr child, std::uint64_t n);
  void open() override {
    child_->open();
    given_ = 0;
  }
  void close() override { child_->close(); }
  std::string describe() const override { return "Limit " + std::to_string(n_); }

 protected:
  bool produce(Row& out) override {
    if (given_ >= n_ || !child_->next(out)) return false;
    ++given_;
    return true;
  }
  // Asks its input for no more rows than it still needs -- and next_batch()
  // guarantees no batch holds more than asked for -- so a scan below a LIMIT n
  // reads n rows in a batch plan too.
  bool produce_batch(Batch& out, std::size_t max) override {
    if (given_ >= n_) return false;
    const std::uint64_t want = std::min<std::uint64_t>(max, n_ - given_);
    if (!child_->next_batch(out, static_cast<std::size_t>(want))) return false;
    given_ += out.count();
    return true;
  }

 private:
  OpPtr child_;
  std::uint64_t n_;
  std::uint64_t given_ = 0;
};

// The joined row: all of the left row's fields, then all of the right's.
inline void join_rows(const Row& l, const Row& r, Row& out) {
  out.assign(l.begin(), l.end());
  out.insert(out.end(), r.begin(), r.end());
}

}  // namespace labdb
