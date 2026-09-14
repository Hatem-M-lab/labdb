#pragma once
// Executor -- walking a syntax tree into engine calls. Unit 8.
//
// This is the milestone the first seven units were built for. The parser
// produced an AST; this walks it. A CREATE registers a schema in the catalog;
// an INSERT reconciles literals to that schema and stores a row; a SELECT
// resolves column names, scans or point-looks-up rows, applies a WHERE, and
// projects the requested columns. Every operation the executor performs is a
// call into the catalog, the trees, the heaps, the record codec, and the
// buffer pool -- the seven units underneath, finally answering SQL.
//
// The executor is where *semantic* errors live, as opposed to the parser's
// syntactic ones: a table that does not exist, a column that is not in the
// schema, a value of the wrong type, the wrong number of values for an
// INSERT. The parser guarantees the statement is well-formed; the executor
// guarantees it is meaningful against the actual catalog.

#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"
#include "catalog.hpp"
#include "record.hpp"

namespace labdb {

// A semantic error: syntactically valid SQL that cannot run against this
// catalog (unknown table/column, type mismatch, wrong arity).
struct ExecError {
  std::string message;
};

// The result of a SELECT: the projected column names, and the matching rows
// as projected Field vectors, in key order. CREATE and INSERT return an empty
// ResultSet (their effect is on disk).
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

  // Convenience: parse `sql` (one statement) and run it. Parse and lex errors
  // propagate as ParseError/LexError; semantic errors as ExecError.
  ResultSet run_sql(const std::string& sql);

 private:
  Catalog& cat_;

  ResultSet run_create(const CreateTableStmt& s);
  ResultSet run_insert(const InsertStmt& s);
  ResultSet run_select(const SelectStmt& s);

  // Reconcile a parsed literal to a target column type: widen an Int64
  // literal to Int32, retype a NULL, and check everything else matches.
  static Field coerce(const Field& literal, const Column& col);
  // Column index by name in a schema, or throw ExecError.
  static std::size_t column_index(const Schema& s, const std::string& name);
  // Evaluate a WHERE comparison against a decoded row.
  static bool where_matches(const Schema& s, const Row& row, const Comparison& w);
};

}  // namespace labdb
