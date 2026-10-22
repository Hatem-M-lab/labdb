#include "executor.hpp"

#include <algorithm>
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
    case ColType::kDouble:  // no table can declare one yet; AVG computes them
      if (literal.type != ColType::kDouble && literal.type != ColType::kInt64)
        throw ExecError{"type mismatch: column " + col.name + " expects DOUBLE"};
      return Field::Double(literal.as_double());
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

// Values compare within a class: numbers with numbers (INT64, INT32 and,
// since Unit 10, DOUBLE alike), booleans with booleans, text with text.
enum class TypeClass { kNumber, kBool, kText };

TypeClass type_class(ColType t) {
  switch (t) {
    case ColType::kText: return TypeClass::kText;
    case ColType::kBool: return TypeClass::kBool;
    default: return TypeClass::kNumber;
  }
}

bool is_aggregate(const SelectItem& v) { return v.func != AggFunc::kNone; }

// A plain column reference, as a select-list value.
SelectItem plain(const ColumnRef& r) {
  SelectItem v;
  static_cast<ColumnRef&>(v) = r;
  return v;
}

}  // namespace

BoundComparison Executor::bind(const OutSchema& schema, const Comparison& c) {
  // WHERE and ON are checked row by row, before any group exists; an
  // aggregate there has nothing to aggregate. (HAVING arrives here already
  // rewritten: its aggregates are columns of the HashAggregate's output.)
  if (is_aggregate(c.left))
    throw ExecError{"aggregate functions are not allowed in WHERE or ON: " +
                    value_sql(c.left)};
  BoundComparison b;
  b.left = resolve(schema, c.left);
  b.op = c.op;
  const Column& lcol = schema[b.left].column;
  b.as_text = lcol.type == ColType::kText;
  b.as_double = lcol.type == ColType::kDouble;
  if (c.right) {
    b.right_is_column = true;
    b.right = resolve(schema, *c.right);
    const Column& rcol = schema[b.right].column;
    if (type_class(lcol.type) != type_class(rcol.type))
      throw ExecError{"cannot compare " + column_ref_sql(c.left) + " (" +
                      type_keyword(lcol.type) + ") with " +
                      column_ref_sql(*c.right) + " (" + type_keyword(rcol.type) + ")"};
    b.as_double = b.as_double || rcol.type == ColType::kDouble;
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
    b.literal = c.literal;  // numbers compare by value at any width
    b.as_double = b.as_double || c.literal.type == ColType::kDouble;
  }
  return b;
}

OpPtr Executor::plan_source(const TableRef& t, OutSchema schema,
                            std::vector<const Comparison*>& where) {
  // Unit 8's plan choice, kept exactly: `key = literal` is answered by one
  // index lookup, and that is the ONLY comparison a point lookup can answer
  // (Challenge 8.4). Any other WHERE leaves the table to a full scan.
  // (A decimal literal, Unit 10, is a value to compare, not a key to look up:
  // `id = 2.5` is left to the Filter, which compares it as a number.)
  for (std::size_t i = 0; i < where.size(); ++i) {
    const BoundComparison b = bind(schema, *where[i]);  // validates it
    if (b.op == TokenType::kEq && !b.right_is_column && b.left == 0 && !b.as_double) {
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

  // ORDER BY may name a select-list alias -- ORDER BY total -- and an
  // unqualified name that matches one means that item, as in PostgreSQL.
  std::vector<OrderItem> order = s.order_by;
  for (OrderItem& o : order) {
    if (is_aggregate(o.value) || !o.value.table.empty()) continue;
    for (const SelectItem& item : s.columns)
      if (!item.alias.empty() && item.alias == o.value.column) {
        o.value = item;
        break;
      }
  }

  // Unit 10: a query aggregates when it groups, or when an aggregate appears
  // anywhere above the WHERE. Its rows are then groups, and every value above
  // the HashAggregate is either a grouping column or an aggregate's result.
  std::vector<SelectItem> items;   // the select list, as seen above the aggregate
  std::vector<Comparison> having;  // ... HAVING, likewise
  const bool aggregating =
      !s.group_by.empty() || !s.having.empty() ||
      std::any_of(s.columns.begin(), s.columns.end(), is_aggregate) ||
      std::any_of(order.begin(), order.end(),
                  [](const OrderItem& o) { return is_aggregate(o.value); });
  if (aggregating) {
    if (s.star)
      throw ExecError{"SELECT * cannot be used with GROUP BY or aggregates; "
                      "name the columns"};
    const OutSchema pre = root->schema();
    std::vector<std::size_t> group_cols;
    std::string group_text, agg_text;  // for the plan: GROUP BY c.name: SUM(o.cents)
    for (const ColumnRef& g : s.group_by) {
      const std::size_t p = resolve(pre, g);
      if (std::find(group_cols.begin(), group_cols.end(), p) != group_cols.end()) continue;
      group_cols.push_back(p);
      group_text += (group_text.empty() ? "GROUP BY " : ", ") + column_ref_sql(g);
    }

    // Each distinct aggregate is computed once, however often it is named.
    std::vector<AggSpec> aggs;
    auto agg_index = [&](const SelectItem& v) {
      AggSpec a;
      a.func = v.func;
      a.star = v.star;
      if (!v.star) {
        a.col = resolve(pre, v);
        a.type = pre[a.col].column.type;
        if ((a.func == AggFunc::kSum || a.func == AggFunc::kAvg) &&
            a.type != ColType::kInt64 && a.type != ColType::kInt32)
          throw ExecError{std::string(agg_name(a.func)) + " needs an integer column; " +
                          column_ref_sql(v) + " is " + type_keyword(a.type)};
      }
      for (std::size_t k = 0; k < aggs.size(); ++k)
        if (aggs[k].func == a.func && aggs[k].star == a.star && aggs[k].col == a.col)
          return k;
      aggs.push_back(a);
      agg_text += (agg_text.empty() ? "" : ", ") + value_sql(v);
      return aggs.size() - 1;
    };
    // A value as the HashAggregate's output names it: an aggregate becomes
    // its result column, #k; a plain column must be one of the groups.
    auto to_output = [&](const SelectItem& v) {
      SelectItem out;
      out.alias = v.alias;
      if (is_aggregate(v)) {
        out.column = "#" + std::to_string(agg_index(v));
        return out;
      }
      const std::size_t p = resolve(pre, v);
      if (std::find(group_cols.begin(), group_cols.end(), p) == group_cols.end())
        throw ExecError{"column " + column_ref_sql(v) +
                        " must appear in GROUP BY or be used in an aggregate"};
      out.table = pre[p].table;
      out.column = pre[p].column.name;
      return out;
    };
    for (const SelectItem& item : s.columns) items.push_back(to_output(item));
    for (const Comparison& c : s.having) {
      Comparison h = c;
      h.left = to_output(c.left);
      if (c.right) h.right = to_output(plain(*c.right));
      having.push_back(h);
    }
    for (OrderItem& o : order) o.value = to_output(o.value);
    const std::string text = group_text.empty() ? agg_text
                             : agg_text.empty() ? group_text
                                                : group_text + ": " + agg_text;
    root = std::make_unique<HashAggregate>(std::move(root), std::move(group_cols),
                                           std::move(aggs), text);
  }

  // HAVING: a filter over the groups.
  if (!having.empty()) {
    std::vector<BoundComparison> preds;
    for (const Comparison& c : having) preds.push_back(bind(root->schema(), c));
    root = std::make_unique<Filter>(std::move(root), std::move(preds),
                                    condition_sql(s.having));
  }

  // ORDER BY, below the projection, so it may sort by a column the select
  // list leaves out. With a LIMIT the sort keeps only the best rows (top n);
  // a LIMIT without ORDER BY simply stops pulling after n rows.
  if (!order.empty()) {
    std::vector<SortKey> keys;
    std::string text;
    for (std::size_t i = 0; i < order.size(); ++i) {
      keys.push_back({resolve(root->schema(), order[i].value), order[i].desc});
      text += (i ? ", " : "") + value_sql(s.order_by[i].value) +
              (order[i].desc ? " DESC" : "");
    }
    std::optional<std::size_t> top;
    if (s.limit) top = static_cast<std::size_t>(*s.limit);
    root = std::make_unique<Sort>(std::move(root), std::move(keys), top, text);
  } else if (s.limit) {
    root = std::make_unique<Limit>(std::move(root), static_cast<std::uint64_t>(*s.limit));
  }

  // The column list. With joins, `*` names columns by table: o.oid, c.id.
  // A named value is shown as its alias if it has one, else as written.
  Plan p;
  if (s.star) {
    const bool qualify = !s.joins.empty();
    for (const OutColumn& c : root->schema())
      p.column_names.push_back(qualify ? c.table + "." + c.column.name
                                       : c.column.name);
  } else {
    const std::vector<SelectItem>& shown = aggregating ? items : s.columns;
    std::vector<std::size_t> cols;
    std::string text;
    for (std::size_t i = 0; i < shown.size(); ++i) {
      cols.push_back(resolve(root->schema(), shown[i]));
      const SelectItem& written = s.columns[i];
      std::string name = value_sql(written);
      if (i) text += ", ";
      text += name;
      if (!written.alias.empty()) {
        text += " AS " + written.alias;
        name = written.alias;
      }
      p.column_names.push_back(std::move(name));
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
