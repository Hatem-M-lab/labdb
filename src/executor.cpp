#include "executor.hpp"

#include <cstdint>

#include "parser.hpp"
#include "unparse.hpp"

namespace labdb {

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

// --------------------------------------------------------------- planning

namespace {

// Values compare within a class: integers with integers (INT64 and INT32
// alike), booleans with booleans, text with text.
enum class TypeClass { kInteger, kBool, kText };

TypeClass type_class(ColType t) {
  switch (t) {
    case ColType::kText: return TypeClass::kText;
    case ColType::kBool: return TypeClass::kBool;
    default: return TypeClass::kInteger;
  }
}

}  // namespace

BoundComparison Executor::bind(const OutSchema& schema, const Comparison& c) {
  BoundComparison b;
  b.left = resolve(schema, c.left);
  b.op = c.op;
  const Column& lcol = schema[b.left].column;
  b.as_text = lcol.type == ColType::kText;
  if (c.right) {
    b.right_is_column = true;
    b.right = resolve(schema, *c.right);
    const Column& rcol = schema[b.right].column;
    if (type_class(lcol.type) != type_class(rcol.type))
      throw ExecError{"cannot compare " + column_ref_sql(c.left) + " (" +
                      type_keyword(lcol.type) + ") with " +
                      column_ref_sql(*c.right) + " (" + type_keyword(rcol.type) + ")"};
  } else if (c.literal.is_null) {
    // Comparing with NULL is legal SQL; it is simply never true. (Unit 8
    // typed this NULL as 0 for integer columns, so `age = NULL` matched every
    // age of 0. eval() now refuses a NULL on either side.)
    b.literal = Field::Null(lcol.type);
  } else {
    if (type_class(c.literal.type) != type_class(lcol.type))
      throw ExecError{"type mismatch: " + column_ref_sql(c.left) + " is " +
                      type_keyword(lcol.type) + ", compared with " +
                      literal_sql(c.literal)};
    b.literal = c.literal;  // integers compare by value at any width
  }
  return b;
}

OpPtr Executor::plan_source(const TableRef& t, OutSchema schema,
                            std::vector<const Comparison*>& where) {
  // Unit 8's plan choice, kept exactly: `key = literal` is answered by one
  // index lookup, and that is the ONLY comparison a point lookup can answer
  // (Challenge 8.4). Any other WHERE leaves the table to a full scan.
  for (std::size_t i = 0; i < where.size(); ++i) {
    const BoundComparison b = bind(schema, *where[i]);  // validates it
    if (b.op == TokenType::kEq && !b.right_is_column && b.left == 0) {
      std::optional<Key> key;
      if (!b.literal.is_null) key = static_cast<Key>(b.literal.i64);
      where.erase(where.begin() + static_cast<std::ptrdiff_t>(i));
      return std::make_unique<IndexLookup>(cat_, t, std::move(schema), key);
    }
  }
  return std::make_unique<SeqScan>(cat_, t, std::move(schema));
}

OpPtr Executor::plan_join(OpPtr left, OpPtr right, const JoinClause& j) {
  const std::size_t nleft = left->schema().size();
  const OutSchema both = concat(left->schema(), right->schema());

  // Sort the ON terms: an equality between a column of each side is a key
  // the hash join can file rows under; anything else is checked per match.
  std::vector<BoundComparison> all, residual;
  std::vector<HashJoin::KeyPair> keys;
  for (const Comparison& c : j.on) {
    const BoundComparison b = bind(both, c);
    all.push_back(b);
    const bool key = b.op == TokenType::kEq && b.right_is_column &&
                     ((b.left < nleft) != (b.right < nleft));
    if (key) {
      const std::size_t l = b.left < nleft ? b.left : b.right;
      const std::size_t r = b.left < nleft ? b.right : b.left;
      keys.emplace_back(l, r - nleft);
    } else {
      residual.push_back(b);
    }
  }

  const std::string text = condition_sql(j.on);
  JoinAlgorithm algo = join_algorithm_;
  if (algo == JoinAlgorithm::kAuto)
    algo = keys.empty() ? JoinAlgorithm::kNestedLoop : JoinAlgorithm::kHash;
  if (algo == JoinAlgorithm::kHash) {
    if (keys.empty())
      throw ExecError{"a hash join needs an ON equality between a column of "
                      "each table: " + text};
    return std::make_unique<HashJoin>(std::move(left), std::move(right),
                                      std::move(keys), std::move(residual), text);
  }
  return std::make_unique<NestedLoopJoin>(std::move(left), std::move(right),
                                          std::move(all), text);
}

Executor::Plan Executor::plan(const SelectStmt& s) {
  // Every table must exist, and every label (alias or name) must be distinct,
  // or `t.col` could mean two different tables. Each table's schema is read
  // from the catalog once, here, and handed to the operators that need it.
  std::vector<const TableRef*> tables{&s.from};
  for (const JoinClause& j : s.joins) tables.push_back(&j.table);
  std::vector<OutSchema> schemas;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    const std::optional<Schema> found = cat_.find_schema(tables[i]->name);
    if (!found) throw ExecError{"no such table: " + tables[i]->name};
    for (std::size_t k = 0; k < i; ++k)
      if (tables[k]->label() == tables[i]->label())
        throw ExecError{"the name " + tables[i]->label() +
                        " refers to two tables; give one an alias"};
    schemas.push_back(table_schema(*found, *tables[i]));
  }

  std::vector<const Comparison*> where;
  for (const Comparison& c : s.where) where.push_back(&c);

  // Sources and joins, left-deep: ((from JOIN t1) JOIN t2) ...
  OpPtr root;
  if (s.joins.empty()) {
    root = plan_source(s.from, std::move(schemas[0]), where);
  } else {
    root = std::make_unique<SeqScan>(cat_, s.from, std::move(schemas[0]));
    for (std::size_t i = 0; i < s.joins.size(); ++i)
      root = plan_join(std::move(root),
                       std::make_unique<SeqScan>(cat_, s.joins[i].table,
                                                 std::move(schemas[i + 1])),
                       s.joins[i]);
  }

  // The rest of the WHERE, above everything it may refer to.
  if (!where.empty()) {
    std::vector<BoundComparison> preds;
    std::vector<Comparison> shown;
    for (const Comparison* c : where) {
      preds.push_back(bind(root->schema(), *c));
      shown.push_back(*c);
    }
    root = std::make_unique<Filter>(std::move(root), std::move(preds),
                                    condition_sql(shown));
  }

  // The column list. With joins, `*` names columns by table: o.oid, c.id.
  Plan p;
  if (s.star) {
    const bool qualify = !s.joins.empty();
    for (const OutColumn& c : root->schema())
      p.column_names.push_back(qualify ? c.table + "." + c.column.name
                                       : c.column.name);
  } else {
    std::vector<std::size_t> cols;
    std::string text;
    for (const ColumnRef& ref : s.columns) {
      cols.push_back(resolve(root->schema(), ref));
      p.column_names.push_back(column_ref_sql(ref));
      text += (text.empty() ? "" : ", ") + column_ref_sql(ref);
    }
    root = std::make_unique<Project>(std::move(root), std::move(cols), text);
  }
  p.root = std::move(root);
  return p;
}

// --------------------------------------------------------------- running

ResultSet Executor::run_select(const SelectStmt& s) {
  Plan p = plan(s);
  ResultSet out;
  out.columns = std::move(p.column_names);
  p.root->open();
  Row row;
  while (p.root->next(row)) out.rows.push_back(std::move(row));
  p.root->close();
  last_plan_ = std::move(p.root);
  return out;
}

std::string Executor::explain_last() const {
  return last_plan_ ? plan_text(*last_plan_) : "(no SELECT has run)\n";
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
  return run(p.parse_single());
}

}  // namespace labdb
