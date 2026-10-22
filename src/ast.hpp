#pragma once
// AST -- the shape of a parsed SQL statement. Unit 7; joins added in Unit 9;
// aggregates, GROUP BY, HAVING, ORDER BY and LIMIT in Unit 10; CREATE INDEX in
// Unit 11.
//
// The parser turns tokens into one of these. The nodes are deliberately close
// to what the executor will need: a CreateTable carries a Schema the catalog
// can register directly; an Insert carries a Row of Fields the record codec
// can encode; a Select carries the column list, the tables, and the WHERE.
// Nothing here touches pages -- it is pure syntax, one step removed from the
// engine calls it will become.
//
// Unit 9 widened SELECT from one table to several. Three things changed, each
// the smallest change that a join forces: a column can be qualified by the
// table it belongs to (`o.customer`); a comparison's right-hand side can be a
// column as well as a literal (`o.customer = c.id` is what an ON clause is);
// and a WHERE or ON is a list of comparisons joined by AND rather than one.
//
// Unit 10 let a SELECT summarize. A value in the select list may now be an
// aggregate -- a function over a column, SUM(o.cents), or over every row,
// COUNT(*) -- and the statement gains GROUP BY, HAVING, ORDER BY and LIMIT. An
// aggregate is a column with something applied to it, so its node, SelectItem,
// extends ColumnRef: code that only needs the column still sees one.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "record.hpp"  // ColType, Field, Schema, Row
#include "token.hpp"   // TokenType, for comparison operators

namespace labdb {

// CREATE TABLE name (columns...)
struct CreateTableStmt {
  std::string table;
  Schema schema;
};

// Unit 11: CREATE INDEX ON table (column). An index has no name of its own: it
// is the index on that column, and a column has at most one.
struct CreateIndexStmt {
  std::string table;
  std::string column;
};

// INSERT INTO name VALUES (literals...)
struct InsertStmt {
  std::string table;
  Row values;  // each Field is a literal; the executor matches them to the schema
};

// A column reference as written: `name`, or `t.name` where t is a table name
// or an alias from the FROM clause. Which column it means is decided later,
// by the executor, against the tables the query actually reads.
struct ColumnRef {
  std::string table;   // the qualifier; empty when the column is unqualified
  std::string column;
};

// Unit 10: the aggregate functions.
enum class AggFunc : std::uint8_t { kNone, kCount, kSum, kAvg, kMin, kMax };

inline const char* agg_name(AggFunc f) {
  switch (f) {
    case AggFunc::kCount: return "COUNT";
    case AggFunc::kSum: return "SUM";
    case AggFunc::kAvg: return "AVG";
    case AggFunc::kMin: return "MIN";
    case AggFunc::kMax: return "MAX";
    case AggFunc::kNone: return "";
  }
  return "";
}

// A value in a select list, a HAVING, or an ORDER BY: a column; an aggregate
// over a column, SUM(o.cents); or COUNT(*), which counts rows and names none.
struct SelectItem : ColumnRef {
  AggFunc func = AggFunc::kNone;  // kNone: the column itself
  bool star = false;              // COUNT(*)
  std::string alias;              // `AS name`, in a select list; empty if none
};

// One comparison: a value against a literal (`id >= 42`) or against a column
// (`o.customer = c.id`). Exactly one of `literal` and `right` is used: `right`
// is set for column-to-column, and `literal` otherwise. The left side may be
// an aggregate (Unit 10) -- the executor allows that only in HAVING.
struct Comparison {
  SelectItem left;
  TokenType op;                     // one of kEq, kNe, kLt, kLe, kGt, kGe
  std::optional<ColumnRef> right;   // set: column OP column
  Field literal;                    // used when `right` is empty
};

// A table in FROM or JOIN, with an optional alias: `orders`, `orders o`, or
// `orders AS o`. The alias, when present, is the only name the rest of the
// query may use to qualify that table's columns.
struct TableRef {
  std::string name;    // the catalog's table name
  std::string alias;   // empty when none was given
  const std::string& label() const { return alias.empty() ? name : alias; }
};

// `JOIN table ON condition` -- the condition is AND-ed comparisons.
struct JoinClause {
  TableRef table;
  std::vector<Comparison> on;
};

// Unit 10: `ORDER BY value [ASC | DESC]`
struct OrderItem {
  SelectItem value;
  bool desc = false;
};

// SELECT items FROM table { JOIN table ON condition } [WHERE condition]
//   [GROUP BY columns] [HAVING condition] [ORDER BY items] [LIMIT n]
struct SelectStmt {
  bool star = false;                  // SELECT *
  std::vector<SelectItem> columns;    // used when !star; Unit 10: may be aggregates
  TableRef from;                      // the first (leftmost) table
  std::vector<JoinClause> joins;      // in the order written, left to right
  std::vector<Comparison> where;      // AND-ed; empty means no WHERE
  std::vector<ColumnRef> group_by;    // Unit 10
  std::vector<Comparison> having;     // Unit 10: AND-ed, may use aggregates
  std::vector<OrderItem> order_by;    // Unit 10
  std::optional<std::int64_t> limit;  // Unit 10
};

// A parsed statement is exactly one of the four.
struct Statement {
  enum class Kind { kCreate, kInsert, kSelect, kCreateIndex } kind;
  CreateTableStmt create;
  InsertStmt insert;
  SelectStmt select;
  CreateIndexStmt create_index;  // Unit 11
};

inline const char* op_symbol(TokenType op) { return token_type_name(op); }

}  // namespace labdb
