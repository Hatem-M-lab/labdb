#pragma once
// AST -- the shape of a parsed SQL statement. Unit 7; joins added in Unit 9.
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

// One comparison: a column against a literal (`id >= 42`) or against another
// column (`o.customer = c.id`). Exactly one of `literal` and `right` is used:
// `right` is set for column-to-column, and `literal` otherwise.
struct Comparison {
  ColumnRef left;
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

// SELECT columns FROM table { JOIN table ON condition } [WHERE condition]
struct SelectStmt {
  bool star = false;               // SELECT *
  std::vector<ColumnRef> columns;  // used when !star
  TableRef from;                   // the first (leftmost) table
  std::vector<JoinClause> joins;   // in the order written, left to right
  std::vector<Comparison> where;   // AND-ed; empty means no WHERE
};

// A parsed statement is exactly one of the three.
struct Statement {
  enum class Kind { kCreate, kInsert, kSelect } kind;
  CreateTableStmt create;
  InsertStmt insert;
  SelectStmt select;
};

inline const char* op_symbol(TokenType op) { return token_type_name(op); }

}  // namespace labdb
