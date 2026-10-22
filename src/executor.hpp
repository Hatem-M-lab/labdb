#pragma once
// Executor -- walking a syntax tree into engine calls. Unit 8; plans of
// operators since Unit 9.
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
// for every JOIN, a Filter for the WHERE, a Project for the column list --
// and then the root of that tree is drained, one row per next(). Planning is
// where every name is resolved and every literal typed, once, so that
// running the plan is nothing but moving rows.
//
// The executor is where *semantic* errors live, as opposed to the parser's
// syntactic ones: a table that does not exist, a column that is not in the
// schema or is ambiguous between two tables, a value of the wrong type, the
// wrong number of values for an INSERT. The parser guarantees the statement
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

  // How a JOIN is executed (Unit 9). kAuto picks a hash join when the ON
  // condition equates a column of each side, and a nested loop otherwise;
  // the other two force one algorithm, for experiments and tests.
  enum class JoinAlgorithm { kAuto, kNestedLoop, kHash };
  void set_join_algorithm(JoinAlgorithm a) { join_algorithm_ = a; }

  // A SELECT's plan: the operator tree, and the column names of its result.
  struct Plan {
    OpPtr root;
    std::vector<std::string> column_names;
  };
  Plan plan(const SelectStmt& s);

  // The operator tree of the last SELECT that ran, with the rows each
  // operator produced -- what the engine actually did, not what it intended.
  std::string explain_last() const;

 private:
  Catalog& cat_;
  JoinAlgorithm join_algorithm_ = JoinAlgorithm::kAuto;
  OpPtr last_plan_;

  ResultSet run_create(const CreateTableStmt& s);
  ResultSet run_insert(const InsertStmt& s);
  ResultSet run_select(const SelectStmt& s);

  // Reconcile a parsed literal to a target column type for an INSERT: narrow
  // an Int64 literal to Int32, retype a NULL, and check everything else.
  static Field coerce(const Field& literal, const Column& col);

  // Bind a comparison to positions in `schema`: resolve its column names,
  // check that the two sides can be compared, and type its literal -- once.
  static BoundComparison bind(const OutSchema& schema, const Comparison& c);

  // The rows of one table: an index lookup if some WHERE term is
  // `key = literal` (that term is consumed), a full scan otherwise.
  OpPtr plan_source(const TableRef& t, OutSchema schema,
                    std::vector<const Comparison*>& where);

  // Join `right` onto `left` by `j`'s ON condition: a hash join on its
  // equalities, or a nested loop.
  OpPtr plan_join(OpPtr left, OpPtr right, const JoinClause& j);
};

}  // namespace labdb
