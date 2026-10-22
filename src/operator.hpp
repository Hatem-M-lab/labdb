#pragma once
// Operators -- a query plan as a tree of iterators. Unit 9.
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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast.hpp"
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
  bool right_is_column = false;
  std::size_t right = 0;         // a column position, when right_is_column
  Field literal;                 // otherwise
};

// Evaluate against one row. A NULL on either side satisfies no comparison:
// in SQL, NULL = NULL is not true, and neither is NULL < 5.
bool eval(const BoundComparison& c, const Row& row);

// The same, for a pair of rows a join is considering: positions below
// `nleft` are read from `l`, the rest from `r`, so no joined row has to be
// built just to find out that it does not qualify.
bool eval_pair(const BoundComparison& c, const Row& l, const Row& r,
               std::size_t nleft);

// --------------------------------------------------------------- Operator
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
  virtual void close() = 0;

  const OutSchema& schema() const { return schema_; }
  std::uint64_t rows_out() const { return rows_out_; }

  // One line saying what this operator does, for plan_text, and optionally
  // a second line of what it measured while running (a join's table sizes).
  virtual std::string describe() const = 0;
  virtual std::string detail() const { return {}; }
  virtual std::vector<const Operator*> children() const { return {}; }

 protected:
  virtual bool produce(Row& out) = 0;

  OutSchema schema_;
  std::uint64_t rows_out_ = 0;
};
using OpPtr = std::unique_ptr<Operator>;

// The operator tree, one operator per line, children indented under their
// parent, each with the number of rows it produced.
std::string plan_text(const Operator& root);

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

// --------------------------------------------------------------- one input

// The input rows that satisfy every comparison (they are AND-ed).
class Filter : public Operator {
 public:
  Filter(OpPtr child, std::vector<BoundComparison> preds, std::string text);
  void open() override { child_->open(); }
  void close() override { child_->close(); }
  std::string describe() const override { return "Filter " + text_; }
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 protected:
  bool produce(Row& out) override;

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
  std::vector<const Operator*> children() const override { return {child_.get()}; }

 protected:
  bool produce(Row& out) override;

 private:
  OpPtr child_;
  std::vector<std::size_t> cols_;
  std::string text_;
  Row in_;
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
  std::vector<const Operator*> children() const override {
    return {left_.get(), right_.get()};
  }

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
};

// Equality join through a hash table. open() reads the right input (the
// build side) into a JoinHashTable keyed on its join columns; then each left
// row (the probe side) is hashed and compared only with the rows in its
// bucket. `keys` pairs a left position with a right position (right
// positions are relative to the right input); `residual` holds any other ON
// comparisons, checked after the keys match.
class HashJoin : public Operator {
 public:
  using KeyPair = std::pair<std::size_t, std::size_t>;
  HashJoin(OpPtr left, OpPtr right, std::vector<KeyPair> keys,
           std::vector<BoundComparison> residual, std::string text,
           JoinHashTable::MixFn mix = mix64);
  void open() override;
  void close() override;
  std::string describe() const override;
  std::string detail() const override;
  std::vector<const Operator*> children() const override {
    return {left_.get(), right_.get()};
  }
  const JoinHashTable& table() const { return table_; }

 protected:
  bool produce(Row& out) override;

 private:
  std::uint64_t key_hash(const Row& row, const std::vector<std::size_t>& cols) const;
  bool null_key(const Row& row, const std::vector<std::size_t>& cols) const;
  bool keys_equal(const Row& l, const Row& r) const;

  OpPtr left_, right_;
  std::vector<std::size_t> lkeys_, rkeys_;
  std::vector<bool> key_text_;
  std::vector<BoundComparison> residual_;
  std::string text_;
  JoinHashTable::MixFn mix_;
  std::size_t nleft_;
  JoinHashTable table_;
  Row outer_;
  bool probing_ = false;
  std::uint64_t probe_hash_ = 0;
  std::uint32_t pos_ = 0, end_ = 0;
};

// The joined row: all of the left row's fields, then all of the right's.
inline void join_rows(const Row& l, const Row& r, Row& out) {
  out.assign(l.begin(), l.end());
  out.insert(out.end(), r.begin(), r.end());
}

}  // namespace labdb
