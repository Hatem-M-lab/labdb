#pragma once
// Executor -- walking a syntax tree into engine calls. Unit 8; plans of
// operators since Unit 9; a planner that chooses, since Unit 11.
//
// This is the milestone the first seven units were built for. The parser
// produced an AST; this walks it. A CREATE registers a schema in the catalog;
// an INSERT reconciles literals to that schema and stores a row; a SELECT
// resolves column names, reads rows, applies a WHERE, and projects the
// requested columns. Every operation is a call into the catalog, the trees,
// the heaps, the record codec, and the buffer pool.
//
// Since Unit 9 a SELECT is not executed directly. It is first *planned* --
// turned into a tree of operators (operator.hpp): scans at the leaves, a join
// for every JOIN, a Filter for the WHERE, and, since Unit 10, a HashAggregate
// for GROUP BY and the aggregates, a Filter for HAVING, a Sort for ORDER BY
// and a Limit for LIMIT, with a Project for the column list on top -- and
// then the root of that tree is drained, one row per next(). Planning is
// where every name is resolved and every literal typed, once, so that
// running the plan is nothing but moving rows.
//
// Through Unit 10 the plan was the query, read literally: tables in the order
// written, every WHERE above every join, an index used only for `key =
// literal`. Since Unit 11 the planner chooses. Each WHERE or ON term that names
// one table goes down to that table; each table is read the cheapest way the
// planner can estimate -- a scan, a range of its key, or a secondary index
// (CREATE INDEX) -- and each join by the cheapest algorithm, building a hash
// table from the smaller input or searching an index once per outer row. The
// estimates come from the indexes themselves -- entries counted, or for the
// key the width of its range -- and the costs are per-row times measured on
// this book's machine (the cost namespace, below).
//
// The executor is where *semantic* errors live, as opposed to the parser's
// syntactic ones: a table that does not exist, a column that is not in the
// schema or is ambiguous between two tables, a value of the wrong type, the
// wrong number of values for an INSERT, a column that is neither grouped nor
// aggregated (Unit 10). The parser guarantees the statement
// is well-formed; the executor guarantees it is meaningful against the
// actual catalog.

#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "catalog.hpp"
#include "operator.hpp"  // ExecError, the operators a plan is built from
#include "record.hpp"

namespace labdb {

// Unit 11: the cost model. The planner compares plans by their estimated time
// in nanoseconds, built from these per-row costs, measured on this book's
// machine (Challenge 11.3). They are this machine's numbers, not universal
// ones; PostgreSQL ships constants of the same kind (seq_page_cost,
// random_page_cost, cpu_tuple_cost) as settings, for the same reason.
// Unit 12: measured again, for plans that run in batches, by
// tests/u12_cost_bench.cpp (Challenge 12.6) -- the medians of four runs,
// rounded. A scanned row went from 140 ns to 10, a fetched one from 310 to
// 58: the ratio between them, which decides when an index wins, from about 2
// to about 6.
namespace cost {
inline constexpr double kSeqRow = 10;     // a row of a scan: the next key, its heap page, decoded
inline constexpr double kRandomRow = 58;  // a row fetched by RID in another order, its page in the pool
inline constexpr double kMissRow = 900;   // ... and more, when that page has to come from the file
inline constexpr double kSeek = 280;      // one search of an index, root to leaf
inline constexpr double kBuildRow = 27;   // a row filed in a join's hash table
inline constexpr double kProbeRow = 18;   // a row looked up in it
inline constexpr double kPairRow = 4.5;   // a pair of rows a nested loop compares
inline constexpr double kDistinct = 200;  // distinct values assumed in a column nothing has counted
// Unit 12: the rows at which batches start to pay. A plan run in batches costs
// about a microsecond more to start than the same plan a row at a time, and
// saves on every row; a plan none of whose operators is expected to handle
// this many rows runs a row at a time (Challenge 12.6).
inline constexpr double kBatchFrom = 256;
}  // namespace cost

// The result of a SELECT: the column names, and the rows. CREATE and INSERT
// return an empty ResultSet (their effect is on disk).
struct ResultSet {
  std::vector<std::string> columns;
  std::vector<std::vector<Field>> rows;
  std::size_t row_count() const { return rows.size(); }
};

class Executor {
 public:
  explicit Executor(Catalog& catalog) : cat_(catalog) {}

  // Run one parsed statement. A SELECT returns rows; CREATE and INSERT return
  // an empty result and take effect on disk.
  ResultSet run(const Statement& stmt);

  // Convenience: parse `sql` -- exactly one statement -- and run it. Parse and
  // lex errors propagate as ParseError/LexError; semantic errors as ExecError.
  ResultSet run_sql(const std::string& sql);

  // How a JOIN is executed (Unit 9). kAuto chooses: since Unit 11 by estimated
  // cost, among a hash join built from either input and an index nested-loop
  // join, and a nested loop when there is no equality to use. The others force
  // one algorithm, for experiments and tests: kHash builds from the right
  // input, as Unit 9's did; kHashLeft from the left; kIndexNestedLoop searches
  // an index of the joined table once per row of the other input.
  enum class JoinAlgorithm { kAuto, kNestedLoop, kHash, kHashLeft, kIndexNestedLoop };
  void set_join_algorithm(JoinAlgorithm a) { join_algorithm_ = a; }

  // Unit 11: how a table is read. kAuto chooses by estimated cost; kScan
  // always reads the whole table and filters it; kIndex uses an index whenever
  // one can answer some of the WHERE, cost aside. For experiments and tests.
  enum class AccessPath { kAuto, kScan, kIndex };
  void set_access_path(AccessPath a) { access_path_ = a; }

  // Unit 11: plan exactly as Unit 10 did -- no term pushed below a join, an
  // index only for `key = literal` on a lone table, a hash join built from the
  // right input -- so that the old plans and the new can be timed side by
  // side, in one session, over the same data.
  void set_literal_planning(bool on) { literal_ = on; }

  // Unit 12: how a plan runs. By default (kChoose) the executor decides, plan
  // by plan: in batches of kDefaultBatchRows rows, column by column, when some
  // operator of the plan is expected to handle cost::kBatchFrom rows or more;
  // a row at a time when none is. set_batch_rows(n) decides instead, for every
  // plan: in batches of up to n rows, or, with 0, a row at a time, as Units 9
  // to 11 ran every plan -- kept so that the ways can be measured side by
  // side. The plan is the same either way, and so is its answer.
  static constexpr std::size_t kChoose = ~std::size_t{0};
  void set_batch_rows(std::size_t n) { batch_rows_ = n; }
  std::size_t batch_rows() const { return batch_rows_; }
  // How the last SELECT ran: in batches of this many rows, or 0, a row at a time.
  std::size_t last_batch_rows() const { return last_batch_rows_; }

  // A SELECT's plan: the operator tree, and the column names of its result.
  struct Plan {
    OpPtr root;
    std::vector<std::string> column_names;
  };
  Plan plan(const SelectStmt& s);

  // The operator tree of the last SELECT that ran, with the rows each
  // operator produced -- what the engine actually did, not what it intended.
  // With `estimates` (Unit 11), each operator the planner estimated also shows
  // the number of rows it expected.
  std::string explain_last(bool estimates = false) const;

 private:
  Catalog& cat_;
  JoinAlgorithm join_algorithm_ = JoinAlgorithm::kAuto;
  AccessPath access_path_ = AccessPath::kAuto;
  bool literal_ = false;
  std::size_t batch_rows_ = kChoose;
  std::size_t last_batch_rows_ = 0;
  OpPtr last_plan_;

  ResultSet run_create(const CreateTableStmt& s);
  ResultSet run_create_index(const CreateIndexStmt& s);
  ResultSet run_insert(const InsertStmt& s);
  ResultSet run_select(const SelectStmt& s);

  // Reconcile a parsed literal to a target column type for an INSERT: narrow
  // an Int64 literal to Int32, retype a NULL, and check everything else.
  static Field coerce(const Field& literal, const Column& col);

  // Bind a comparison to positions in `schema`: resolve its column names,
  // check that the two sides can be compared, and type its literal -- once.
  static BoundComparison bind(const OutSchema& schema, const Comparison& c);

  // Unit 10's planning, kept for set_literal_planning. The rows of one table:
  // an index lookup if some WHERE term is `key = literal` (that term is
  // consumed), a full scan otherwise.
  OpPtr plan_source(const TableRef& t, OutSchema schema,
                    std::vector<const Comparison*>& where);

  // Join `right` onto `left` by `j`'s ON condition: a hash join on its
  // equalities, or a nested loop.
  OpPtr plan_join(OpPtr left, OpPtr right, const JoinClause& j);

  // Unit 11: the FROM, the JOINs and the WHERE, planned by cost (executor.cpp).
  struct Source;
  struct Access;
  Source plan_from(const SelectStmt& s, std::vector<OutSchema>& schemas,
                   const std::vector<TableInfo>& infos);
  Access plan_access(const TableRef& t, const TableInfo& info, OutSchema schema,
                     const std::vector<const Comparison*>& terms);
};

}  // namespace labdb
