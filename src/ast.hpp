#pragma once
// AST -- the shape of a parsed SQL statement. Unit 7.
//
// The parser turns tokens into one of these. The nodes are deliberately close
// to what the executor (Unit 8) will need: a CreateTable carries a Schema the
// catalog can register directly; an Insert carries a Row of Fields the record
// codec can encode; a Select carries the column list, the table, and an
// optional WHERE comparison. Nothing here touches pages -- it is pure syntax,
// one step removed from the engine calls it will become.

#include <cstdint>
#include <memory>
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

// A WHERE comparison: column OP literal, e.g. `id >= 42`.
struct Comparison {
  std::string column;
  TokenType op;   // one of kEq, kNe, kLt, kLe, kGt, kGe
  Field literal;
};

// SELECT columns FROM name [WHERE comparison]
struct SelectStmt {
  bool star = false;                 // SELECT *
  std::vector<std::string> columns;  // used when !star
  std::string table;
  std::optional<Comparison> where;
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
