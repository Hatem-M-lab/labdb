#include "executor.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

ResultSet Executor::run_create_index(const CreateIndexStmt& s) {
  const std::optional<TableInfo> t = cat_.find_table(s.table);
  if (!t) throw ExecError{"no such table: " + s.table};
  const std::vector<Column>& cols = t->schema.columns;
  std::size_t col = 0;
  while (col < cols.size() && cols[col].name != s.column) ++col;
  if (col == cols.size()) throw ExecError{"no such column: " + s.table + "." + s.column};
  if (col == 0)
    throw ExecError{"column " + s.column + " is the key of " + s.table +
                    "; the table's own B+Tree is already its index"};
  if (t->indexed[col]) throw ExecError{"column " + s.table + "." + s.column + " already has an index"};
  cat_.create_index(s.table, col);
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
  if (!cat_.insert_row(s.table, row))
    throw ExecError{"duplicate key: " + s.table + " already has a row with " +
                    schema.columns[0].name + " = " + std::to_string(row[0].i64)};
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
  if (algo != JoinAlgorithm::kNestedLoop && algo != JoinAlgorithm::kHash)
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

namespace {

// What fetching one row by its RID costs, for a table whose heap spans `pages`
// pages, through a pool of `frames`: a page is in the pool with probability
// frames / pages, at best -- the share of the heap the pool can hold.
double fetch_cost(const TableInfo& t, std::size_t frames) {
  const double held = t.heap_pages == 0 ? 1.0 : std::min(1.0, static_cast<double>(frames) / t.heap_pages);
  return cost::kRandomRow + (1.0 - held) * cost::kMissRow;
}

// The fraction of rows a comparison keeps when no index has counted it:
// PostgreSQL's defaults for a column it has no statistics on.
double default_selectivity(TokenType op) {
  switch (op) {
    case TokenType::kEq: return 0.005;
    case TokenType::kNe: return 0.995;
    default: return 1.0 / 3.0;
  }
}

// The values a column may hold, given the WHERE terms comparing it with a
// literal: a lower and an upper bound, each inclusive or not, or absent.
struct ValueRange {
  std::optional<Field> lo, hi;
  bool lo_incl = true, hi_incl = true;
  bool empty = false;  // a term that is never true (`= NULL`)
};

// Tighten `r` by `column op v`, for v not NULL.
void narrow(ValueRange& r, TokenType op, const Field& v) {
  const auto raise = [&](bool incl) {
    const int c = r.lo ? compare_values(v, *r.lo) : 1;
    if (c > 0 || (c == 0 && !incl)) {
      r.lo = v;
      r.lo_incl = incl;
    }
  };
  const auto lower = [&](bool incl) {
    const int c = r.hi ? compare_values(v, *r.hi) : -1;
    if (c < 0 || (c == 0 && !incl)) {
      r.hi = v;
      r.hi_incl = incl;
    }
  };
  switch (op) {
    case TokenType::kEq: raise(true); lower(true); break;
    case TokenType::kGt: raise(false); break;
    case TokenType::kGe: raise(true); break;
    case TokenType::kLt: lower(false); break;
    case TokenType::kLe: lower(true); break;
    default: break;
  }
}

// The keys [lo, hi] of the index entries a range can hold, or false if it can
// hold none. For an exact key, an exclusive bound becomes the next key in;
// a TEXT bound is a prefix, so it stays inclusive, and the comparisons are
// checked again on each row.
bool range_keys(const ValueRange& r, ColType type, Key& lo, Key& hi) {
  if (r.empty) return false;
  if (r.lo && r.hi) {
    const int c = compare_values(*r.lo, *r.hi);
    if (c > 0 || (c == 0 && !(r.lo_incl && r.hi_incl))) return false;
  }
  const bool exact = order_key_exact(type);
  lo = 0;
  hi = ~Key{0};
  if (r.lo) {
    lo = order_key(*r.lo);
    if (!r.lo_incl && exact) {
      if (lo == ~Key{0}) return false;
      ++lo;
    }
  }
  if (r.hi) {
    hi = order_key(*r.hi);
    if (!r.hi_incl && exact) {
      if (hi == 0) return false;
      --hi;
    }
  }
  return lo <= hi;
}

}  // namespace

struct Executor::Access {
  OpPtr op;
  double rows = 0;         // estimated rows out
  double cost = 0;         // estimated nanoseconds
  bool key_order = false;  // rows come out in key (column 0) order
  bool bare = false;       // the whole table, unfiltered: a SeqScan and nothing else
};

struct Executor::Source {
  OpPtr root;
  bool key_order = false;  // one table, read in key order
  bool bare = false;       // one table, read whole, with no WHERE at all
};

// One table's access path: the cheapest way to produce the rows that its own
// terms -- the WHERE and ON terms that name no other table -- allow.
Executor::Access Executor::plan_access(const TableRef& t, const TableInfo& info,
                                       OutSchema schema,
                                       const std::vector<const Comparison*>& terms) {
  const double n = static_cast<double>(info.rows);
  std::vector<BoundComparison> bound;
  bound.reserve(terms.size());
  for (const Comparison* c : terms) bound.push_back(bind(schema, *c));
  // A term that a range on `col` can use: the column compared with a literal
  // by =, <, <=, > or >=, the literal not a decimal (which is compared, never
  // looked up).
  const auto in_range = [&](const BoundComparison& b, std::size_t col) {
    return b.left == col && !b.right_is_column && b.op != TokenType::kNe &&
           b.literal.type != ColType::kDouble;
  };

  // Every indexed column that some term bounds is a candidate: the range of
  // keys its terms allow, and what each row through that index costs.
  struct Candidate {
    std::size_t col = 0;
    Key lo = 1, hi = 0;  // empty unless range_keys says otherwise
    double per_row = 0;
    std::uint64_t count = 0;
  };
  std::array<Candidate, kCatalogMaxColumns> cands;
  std::size_t ncand = 0;
  for (std::size_t col = 0; col < schema.size(); ++col) {
    if (!info.indexed[col]) continue;
    ValueRange range;
    bool any = false;
    for (const BoundComparison& b : bound) {
      if (!in_range(b, col)) continue;
      if (b.literal.is_null) range.empty = true;  // never true: nothing to look up
      else narrow(range, b.op, b.literal);
      any = true;
    }
    if (!any) continue;
    Candidate& k = cands[ncand++];
    k.col = col;
    if (!range_keys(range, schema[col].column.type, k.lo, k.hi)) k.lo = 1, k.hi = 0;
    // Through the table's own tree, rows come in key order -- the order of the
    // heap; through any other index, in the column's order.
    k.per_row = col == 0 ? cost::kSeqRow : fetch_cost(info, cat_.pool_frames());
  }

  // Choose. The whole table costs n rows of a scan. A candidate costs one
  // search and a row per entry in its range. For the key, the range itself
  // bounds that number: keys are unique, so w keys hold at most w rows. For any
  // other column the entries are counted through its index -- but only up to
  // the count at which it would cost more than the best plan found so far;
  // past that, the exact number is not needed.
  double best = n * cost::kSeqRow;
  int pick = -1;
  if (access_path_ != AccessPath::kScan) {
    for (std::size_t i = 0; i < ncand; ++i) {
      Candidate& k = cands[i];
      if (k.lo <= k.hi && k.col == 0) {
        Key lo = k.lo, hi = k.hi;
        if (static_cast<double>(hi - lo) + 1.0 > n) {  // wider than the table: clip it to the keys there are
          const auto ends = cat_.index_bounds(t.name, 0);
          lo = ends ? std::max(lo, ends->first) : 1;
          hi = ends ? std::min(hi, ends->second) : 0;
        }
        k.count = lo > hi ? 0 : static_cast<std::uint64_t>(std::min(n, static_cast<double>(hi - lo) + 1.0));
      } else if (k.lo <= k.hi) {
        const double room = (best - cost::kSeek) / k.per_row;
        std::uint64_t cap = static_cast<std::uint64_t>(std::max(0.0, std::min(room, n))) + 1;
        if (access_path_ == AccessPath::kIndex) cap = info.rows + 1;  // forced: count them all
        k.count = cat_.count_index(t.name, k.col, k.lo, k.hi, cap);
      }
      const double c = cost::kSeek + static_cast<double>(k.count) * k.per_row;
      const bool better = access_path_ == AccessPath::kIndex
                              ? pick < 0 || k.count < cands[static_cast<std::size_t>(pick)].count
                              : c < best;
      if (better) {
        best = c;
        pick = static_cast<int>(i);
      }
    }
  }

  // Build the path. Its range is exactly its terms when the key is exact, so
  // those terms are consumed; every other term goes to a Filter above it.
  Access a;
  double rows = n;
  std::size_t chosen = schema.size();  // the chosen index's column, if any
  bool consumes = false;
  if (pick < 0) {
    a.op = std::make_unique<SeqScan>(cat_, t, std::move(schema));
    a.key_order = true;
  } else {
    const Candidate& k = cands[static_cast<std::size_t>(pick)];
    chosen = k.col;
    consumes = order_key_exact(schema[k.col].column.type);
    if (k.col == 0 && k.lo == k.hi) {  // one key: Unit 8's point lookup
      a.op = std::make_unique<IndexLookup>(cat_, t, std::move(schema), static_cast<Key>(k.lo ^ table_key(0)));
    } else {
      std::vector<Comparison> shown;
      for (std::size_t i = 0; i < terms.size(); ++i)
        if (in_range(bound[i], k.col)) shown.push_back(*terms[i]);
      a.op = std::make_unique<IndexScan>(cat_, t, std::move(schema), k.col, k.lo, k.hi, condition_sql(shown));
    }
    a.key_order = k.col == 0;
    rows = static_cast<double>(k.count);
  }
  a.op->set_estimate(rows);

  // The rows out: the chosen path's, times the share of rows each remaining
  // term keeps -- counted through its index if it had one, a default if not.
  for (std::size_t i = 0; i < ncand; ++i)  // the other candidates' counts
    if (static_cast<int>(i) != pick && n > 0)
      rows *= std::min(1.0, static_cast<double>(cands[i].count) / n);
  std::vector<BoundComparison> preds;
  std::vector<Comparison> shown;
  for (std::size_t i = 0; i < terms.size(); ++i) {
    const BoundComparison& b = bound[i];
    if (!(consumes && in_range(b, chosen))) {
      preds.push_back(b);
      shown.push_back(*terms[i]);
    }
    if (!(info.indexed[b.left] && in_range(b, b.left))) rows *= default_selectivity(b.op);
  }
  if (!preds.empty()) {
    a.op = std::make_unique<Filter>(std::move(a.op), std::move(preds), condition_sql(shown));
    a.op->set_estimate(rows);
  }
  a.rows = rows;
  a.cost = best;
  a.bare = pick < 0 && shown.empty();
  return a;
}

// The FROM, the JOINs and the WHERE, as a tree of operators chosen by cost.
Executor::Source Executor::plan_from(const SelectStmt& s, std::vector<OutSchema>& schemas,
                                     const std::vector<TableInfo>& infos) {
  if (s.joins.empty()) {  // one table: every term is its own
    std::vector<const Comparison*> mine;
    mine.reserve(s.where.size());
    for (const Comparison& c : s.where) mine.push_back(&c);
    Access a = plan_access(s.from, infos[0], std::move(schemas[0]), mine);
    return {std::move(a.op), a.key_order, a.bare};
  }
  std::vector<const TableRef*> tables{&s.from};
  for (const JoinClause& j : s.joins) tables.push_back(&j.table);
  const std::size_t nt = tables.size();
  std::vector<std::size_t> offset(nt + 1, 0);
  for (std::size_t i = 0; i < nt; ++i) offset[i + 1] = offset[i] + schemas[i].size();
  const auto owner = [&](std::size_t pos) {
    std::size_t i = 0;
    while (pos >= offset[i + 1]) ++i;
    return i;
  };

  // Every term of every ON and of the WHERE, with the tables it names. They
  // are all AND-ed (every join is an inner join), so where a term is checked
  // is the planner's choice. Each is bound in the scope it was written in --
  // an ON sees the tables joined so far -- so the errors stay those of Unit 9.
  struct Term {
    const Comparison* c;
    std::uint64_t tables;  // bit i: names a column of table i
  };
  std::vector<Term> terms;
  const auto add = [&](const OutSchema& scope, const Comparison& c) {
    const BoundComparison b = bind(scope, c);
    std::uint64_t m = std::uint64_t{1} << owner(b.left);
    if (b.right_is_column) m |= std::uint64_t{1} << owner(b.right);
    terms.push_back({&c, m});
  };
  OutSchema scope = schemas[0];
  for (std::size_t j = 0; j < s.joins.size(); ++j) {
    scope = concat(scope, schemas[j + 1]);
    for (const Comparison& c : s.joins[j].on) add(scope, c);
  }
  for (const Comparison& c : s.where) add(scope, c);

  // Pushdown: a term that names one table is checked as that table is read.
  std::vector<std::vector<const Comparison*>> local(nt);
  std::vector<Term> cross;
  for (const Term& t : terms) {
    if (std::popcount(t.tables) == 1) local[static_cast<std::size_t>(std::countr_zero(t.tables))].push_back(t.c);
    else cross.push_back(t);
  }
  std::vector<Access> acc;
  for (std::size_t i = 0; i < nt; ++i) acc.push_back(plan_access(*tables[i], infos[i], schemas[i], local[i]));

  // The joins, left-deep, in the order written; each by the cheapest algorithm.
  OpPtr root = std::move(acc[0].op);
  double rows = acc[0].rows, total = acc[0].cost;
  std::uint64_t have = 1;
  std::vector<bool> placed(cross.size(), false);
  for (std::size_t i = 1; i < nt; ++i) {
    have |= std::uint64_t{1} << i;
    std::vector<Comparison> here;  // the cross terms that table i makes checkable
    for (std::size_t k = 0; k < cross.size(); ++k)
      if (!placed[k] && (cross[k].tables & ~have) == 0) {
        here.push_back(*cross[k].c);
        placed[k] = true;
      }
    const OutSchema both = concat(root->schema(), schemas[i]);
    const std::size_t nleft = root->schema().size();
    std::vector<BoundComparison> bound;
    std::vector<HashJoin::KeyPair> keys;
    std::vector<std::size_t> key_term;
    for (std::size_t k = 0; k < here.size(); ++k) {
      const BoundComparison b = bind(both, here[k]);
      bound.push_back(b);
      if (b.op == TokenType::kEq && b.right_is_column && ((b.left < nleft) != (b.right < nleft))) {
        keys.emplace_back(std::min(b.left, b.right), std::max(b.left, b.right) - nleft);
        key_term.push_back(k);
      }
    }
    const std::string text = here.empty() ? "TRUE" : condition_sql(here);

    // How many rows the join makes. A key pair that names a table's own key
    // (column 0) means each row on the other side matches at most one row of
    // that table, and one of the rows its terms keep in that share. Without
    // one, the planner has no count of distinct values, and assumes what
    // PostgreSQL assumes with no statistics: 200 of them, so that a pair of
    // rows matches one time in 200.
    const double el = rows, er = acc[i].rows;
    const double ni = std::max(1.0, static_cast<double>(infos[i].rows));
    double out = el * er / cost::kDistinct;
    if (keys.empty()) {
      out = el * er;
      for (const BoundComparison& b : bound) out *= default_selectivity(b.op);
    }
    for (const auto& [l, r] : keys) {
      if (r == 0) { out = el * (er / ni); break; }
      const std::size_t tl = owner(l);
      if (l == offset[tl]) { out = er * (el / std::max(1.0, static_cast<double>(infos[tl].rows))); break; }
    }
    // Rows of table `t` that share one value of its column `col`: one for its
    // key; for any other column, as many as each row of the other side's
    // table has if that side is a key, and a default guess if not.
    const auto per_value = [&](std::size_t t, std::size_t col, bool other_is_key, double other_n) {
      if (col == 0) return 1.0;
      const double nt_rows = static_cast<double>(infos[t].rows);
      return other_is_key ? std::max(1.0, nt_rows / std::max(1.0, other_n))
                          : std::max(1.0, nt_rows / cost::kDistinct);
    };

    // The options, and what each is expected to cost.
    enum class Algo { kNestedLoop, kHashRight, kHashLeft, kIndexRight, kIndexLeft };
    struct Option {
      Algo algo;
      double cost;
      std::size_t key = 0;
    };
    std::vector<Option> options;
    const double cl = total, cr = acc[i].cost;
    options.push_back({Algo::kNestedLoop, cl + cr + el * er * cost::kPairRow});
    if (!keys.empty()) {
      options.push_back({Algo::kHashRight, cl + cr + er * cost::kBuildRow + el * cost::kProbeRow});
      options.push_back({Algo::kHashLeft, cl + cr + el * cost::kBuildRow + er * cost::kProbeRow});
    }
    for (std::size_t k = 0; k < keys.size(); ++k) {
      const auto [l, r] = keys[k];
      const std::size_t tl = owner(l);
      if (infos[i].indexed[r]) {  // search table i's index once per row on the left
        const double m = per_value(i, r, l == offset[tl], static_cast<double>(infos[tl].rows));
        options.push_back({Algo::kIndexRight,
                           cl + el * (cost::kSeek + m * fetch_cost(infos[i], cat_.pool_frames())), k});
      }
      if (i == 1 && infos[0].indexed[l]) {  // the first join: table 0 can be the searched one
        const double m = per_value(0, l, r == 0, static_cast<double>(infos[1].rows));
        options.push_back({Algo::kIndexLeft,
                           cr + er * (cost::kSeek + m * fetch_cost(infos[0], cat_.pool_frames())), k});
      }
    }
    const auto first_of = [&](std::initializer_list<Algo> want) -> const Option* {
      for (Algo a : want)
        for (const Option& o : options)
          if (o.algo == a) return &o;
      return nullptr;
    };
    const Option* pick = nullptr;
    switch (join_algorithm_) {
      case JoinAlgorithm::kNestedLoop: pick = first_of({Algo::kNestedLoop}); break;
      case JoinAlgorithm::kHash: pick = first_of({Algo::kHashRight}); break;
      case JoinAlgorithm::kHashLeft: pick = first_of({Algo::kHashLeft}); break;
      case JoinAlgorithm::kIndexNestedLoop: pick = first_of({Algo::kIndexRight, Algo::kIndexLeft}); break;
      case JoinAlgorithm::kAuto:
        for (const Option& o : options)
          if (!pick || o.cost < pick->cost) pick = &o;
        break;
    }
    if (!pick) {
      if (keys.empty())
        throw ExecError{"a hash join needs an ON equality between a column of each table: " + text};
      throw ExecError{"no index to search for an index nested-loop join: " + text};
    }

    // Build it. Every term at this join that is not the algorithm's key is
    // checked on each joined row.
    const auto residual_without = [&](std::optional<std::size_t> key) {
      std::vector<BoundComparison> out_terms;
      for (std::size_t k = 0; k < bound.size(); ++k)
        if (!key || k != key_term[*key]) out_terms.push_back(bound[k]);
      return out_terms;
    };
    const auto table_terms = [&](std::size_t t) {
      std::vector<BoundComparison> f;
      for (const Comparison* c : local[t]) f.push_back(bind(schemas[t], *c));
      return f;
    };
    switch (pick->algo) {
      case Algo::kNestedLoop:
        root = std::make_unique<NestedLoopJoin>(std::move(root), std::move(acc[i].op), bound, text);
        break;
      case Algo::kHashRight:
      case Algo::kHashLeft: {
        std::vector<BoundComparison> residual;
        for (std::size_t k = 0; k < bound.size(); ++k)
          if (std::find(key_term.begin(), key_term.end(), k) == key_term.end()) residual.push_back(bound[k]);
        root = std::make_unique<HashJoin>(std::move(root), std::move(acc[i].op), keys,
                                          std::move(residual), text, mix64,
                                          pick->algo == Algo::kHashLeft);
        break;
      }
      case Algo::kIndexRight: {
        const auto [l, r] = keys[pick->key];
        root = std::make_unique<IndexNestedLoopJoin>(std::move(root), cat_, *tables[i], schemas[i], l, r,
                                                     table_terms(i), residual_without(pick->key),
                                                     false, text);
        break;
      }
      case Algo::kIndexLeft: {
        const auto [l, r] = keys[pick->key];
        root = std::make_unique<IndexNestedLoopJoin>(std::move(acc[1].op), cat_, *tables[0], schemas[0], r, l,
                                                     table_terms(0), residual_without(pick->key),
                                                     true, text);
        break;
      }
    }
    root->set_estimate(out);
    rows = out;
    total = pick->cost;
  }
  return {std::move(root), false, false};
}

Executor::Plan Executor::plan(const SelectStmt& s) {
  // Every table must exist, and every label (alias or name) must be distinct,
  // or `t.col` could mean two different tables. Each table's schema is read
  // from the catalog once, here, and handed to the operators that need it.
  std::vector<const TableRef*> tables{&s.from};
  for (const JoinClause& j : s.joins) tables.push_back(&j.table);
  std::vector<OutSchema> schemas;
  std::vector<TableInfo> infos;
  for (std::size_t i = 0; i < tables.size(); ++i) {
    std::optional<TableInfo> found = cat_.find_table(tables[i]->name);
    if (!found) throw ExecError{"no such table: " + tables[i]->name};
    for (std::size_t k = 0; k < i; ++k)
      if (tables[k]->label() == tables[i]->label())
        throw ExecError{"the name " + tables[i]->label() +
                        " refers to two tables; give one an alias"};
    schemas.push_back(table_schema(found->schema, *tables[i]));
    infos.push_back(std::move(*found));
  }
  if (tables.size() > 64) throw ExecError{"a query may join at most 64 tables"};

  // The FROM, the JOINs and the WHERE: chosen by cost (Unit 11), or as Unit
  // 10 planned them, literally.
  Source src;
  if (!literal_) {
    src = plan_from(s, schemas, infos);
  } else {
    std::vector<const Comparison*> where;
    for (const Comparison& c : s.where) where.push_back(&c);
    if (s.joins.empty()) {
      src.root = plan_source(s.from, std::move(schemas[0]), where);
    } else {
      src.root = std::make_unique<SeqScan>(cat_, s.from, std::move(schemas[0]));
      for (std::size_t i = 0; i < s.joins.size(); ++i)
        src.root = plan_join(std::move(src.root),
                             std::make_unique<SeqScan>(cat_, s.joins[i].table,
                                                       std::move(schemas[i + 1])),
                             s.joins[i]);
    }
    if (!where.empty()) {  // the rest of the WHERE, above everything it may refer to
      std::vector<BoundComparison> preds;
      std::vector<Comparison> shown;
      for (const Comparison* c : where) {
        preds.push_back(bind(src.root->schema(), *c));
        shown.push_back(*c);
      }
      src.root = std::make_unique<Filter>(std::move(src.root), std::move(preds),
                                          condition_sql(shown));
    }
  }
  OpPtr root = std::move(src.root);

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
    // Unit 11: MIN and MAX of indexed columns over a whole table are the two
    // ends of their indexes; nothing else needs reading.
    const bool ends = src.bare && group_cols.empty() && access_path_ != AccessPath::kScan &&
                      std::all_of(aggs.begin(), aggs.end(), [&](const AggSpec& a) {
                        return (a.func == AggFunc::kMin || a.func == AggFunc::kMax) &&
                               infos[0].indexed[a.col];
                      });
    if (ends)
      root = std::make_unique<IndexMinMax>(cat_, s.from, std::move(aggs), text);
    else
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
  // Unit 11: one table read in key order is already sorted by its key, and a
  // key is unique, so ORDER BY key needs no Sort -- only the LIMIT, if any.
  const bool sorted = src.key_order && !aggregating && !order.empty() && !order[0].desc &&
                      !is_aggregate(order[0].value) && resolve(root->schema(), order[0].value) == 0;
  if (sorted) {
    if (s.limit) root = std::make_unique<Limit>(std::move(root), static_cast<std::uint64_t>(*s.limit));
  } else if (!order.empty()) {
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

std::string Executor::explain_last(bool estimates) const {
  return last_plan_ ? plan_text(*last_plan_, estimates) : "(no SELECT has run)\n";
}

ResultSet Executor::run(const Statement& stmt) {
  switch (stmt.kind) {
    case Statement::Kind::kCreate: return run_create(stmt.create);
    case Statement::Kind::kCreateIndex: return run_create_index(stmt.create_index);
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
