#include "executor.hpp"

#include <cstdint>

#include "parser.hpp"

namespace labdb {

std::size_t Executor::column_index(const Schema& s, const std::string& name) {
  for (std::size_t i = 0; i < s.columns.size(); ++i)
    if (s.columns[i].name == name) return i;
  throw ExecError{"no such column: " + name};
}

// Reconcile a literal from the parser (Int64 for any integer, a placeholder
// type for NULL) to the column it is going into.
Field Executor::coerce(const Field& literal, const Column& col) {
  if (literal.is_null) {
    if (!col.nullable) throw ExecError{"NULL in non-nullable column " + col.name};
    return Field::Null(col.type);
  }
  switch (col.type) {
    case ColType::kInt64:
      if (literal.type != ColType::kInt64)
        throw ExecError{"type mismatch: column " + col.name + " expects INT64"};
      return literal;
    case ColType::kInt32:
      // an integer literal is parsed as Int64; narrow it, checking range
      if (literal.type != ColType::kInt64)
        throw ExecError{"type mismatch: column " + col.name + " expects INT32"};
      if (literal.i64 < INT32_MIN || literal.i64 > INT32_MAX)
        throw ExecError{"integer out of INT32 range for column " + col.name};
      return Field::Int32(static_cast<std::int32_t>(literal.i64));
    case ColType::kBool:
      if (literal.type != ColType::kBool)
        throw ExecError{"type mismatch: column " + col.name + " expects BOOL"};
      return literal;
    case ColType::kText:
      if (literal.type != ColType::kText)
        throw ExecError{"type mismatch: column " + col.name + " expects TEXT"};
      return literal;
  }
  throw ExecError{"unknown column type"};
}

ResultSet Executor::run_create(const CreateTableStmt& s) {
  if (cat_.table_exists(s.table))
    throw ExecError{"table already exists: " + s.table};
  cat_.create_table(s.table, s.schema);
  return {};
}

ResultSet Executor::run_insert(const InsertStmt& s) {
  if (!cat_.table_exists(s.table))
    throw ExecError{"no such table: " + s.table};
  const Schema schema = cat_.schema_of(s.table);
  if (s.values.size() != schema.columns.size())
    throw ExecError{"INSERT has " + std::to_string(s.values.size()) +
                    " values but table " + s.table + " has " +
                    std::to_string(schema.columns.size()) + " columns"};
  Row row;
  row.reserve(schema.columns.size());
  for (std::size_t i = 0; i < schema.columns.size(); ++i)
    row.push_back(coerce(s.values[i], schema.columns[i]));
  // Column 0 is the key: it must be a non-null Int64 (a stated limitation).
  if (row[0].is_null || schema.columns[0].type != ColType::kInt64)
    throw ExecError{"the key column (column 0) must be a non-null INT64"};
  cat_.insert_row(s.table, row);
  return {};
}

bool Executor::where_matches(const Schema& s, const Row& row, const Comparison& w) {
  const std::size_t ci = column_index(s, w.column);
  const Field& cell = row[ci];
  const Field lit = coerce(w.literal, s.columns[ci]);
  if (cell.is_null) return false;  // NULL matches no comparison (SQL-ish)

  // Compare within the column's type.
  int cmp;
  if (s.columns[ci].type == ColType::kText) {
    if (lit.is_null) return false;
    cmp = cell.text.compare(lit.text) < 0 ? -1 : (cell.text == lit.text ? 0 : 1);
  } else {
    const std::int64_t a = cell.i64, b = lit.i64;
    cmp = a < b ? -1 : (a == b ? 0 : 1);
  }
  switch (w.op) {
    case TokenType::kEq: return cmp == 0;
    case TokenType::kNe: return cmp != 0;
    case TokenType::kLt: return cmp < 0;
    case TokenType::kLe: return cmp <= 0;
    case TokenType::kGt: return cmp > 0;
    case TokenType::kGe: return cmp >= 0;
    default: throw ExecError{"unknown comparison operator"};
  }
}

ResultSet Executor::run_select(const SelectStmt& s) {
  if (!cat_.table_exists(s.table))
    throw ExecError{"no such table: " + s.table};
  const Schema schema = cat_.schema_of(s.table);

  // Resolve the projection: '*' is every column; otherwise the named columns,
  // each checked against the schema.
  std::vector<std::size_t> proj;
  ResultSet out;
  if (s.star) {
    for (std::size_t i = 0; i < schema.columns.size(); ++i) proj.push_back(i);
  } else {
    for (const std::string& name : s.columns) proj.push_back(column_index(schema, name));
  }
  for (std::size_t i : proj) out.columns.push_back(schema.columns[i].name);

  // Validate the WHERE column exists up front, so an unknown column errors
  // even on an empty table.
  if (s.where) (void)column_index(schema, s.where->column);

  const auto emit = [&](const Row& row) {
    if (s.where && !where_matches(schema, row, *s.where)) return;
    std::vector<Field> projected;
    projected.reserve(proj.size());
    for (std::size_t i : proj) projected.push_back(row[i]);
    out.rows.push_back(std::move(projected));
  };

  // Fast path: WHERE key = literal is a point lookup through the index.
  // Otherwise, scan the table in key order.
  if (s.where && s.where->op == TokenType::kEq &&
      s.where->column == schema.columns[0].name) {
    const Field k = coerce(s.where->literal, schema.columns[0]);
    if (!k.is_null) {
      if (auto row = cat_.get_by_key(s.table, static_cast<Key>(k.i64))) emit(*row);
    }
  } else {
    cat_.scan_table(s.table, emit);
  }
  return out;
}

ResultSet Executor::run(const Statement& stmt) {
  switch (stmt.kind) {
    case Statement::Kind::kCreate: return run_create(stmt.create);
    case Statement::Kind::kInsert: return run_insert(stmt.insert);
    case Statement::Kind::kSelect: return run_select(stmt.select);
  }
  throw ExecError{"unknown statement kind"};
}

ResultSet Executor::run_sql(const std::string& sql) {
  Parser p(sql);
  return run(p.parse_statement());
}

}  // namespace labdb
