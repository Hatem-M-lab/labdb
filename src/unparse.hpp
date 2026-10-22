#pragma once
// Unparse -- an AST back into normalized SQL text. Unit 7; joins in Unit 9.
//
// The reverse of the parser: given a Statement, produce a canonical SQL string
// for it. This is how the payoff (Challenge 7.5) shows the parse was faithful
// -- parse a statement, print it back, and read that the meaning survived the
// round trip -- and it is a genuinely useful tool (logging, debugging) beyond
// the demo.

#include <sstream>
#include <string>

#include "ast.hpp"
#include "record.hpp"

namespace labdb {

inline std::string type_keyword(ColType t) {
  switch (t) {
    case ColType::kInt64: return "INT64";
    case ColType::kInt32: return "INT32";
    case ColType::kBool: return "BOOL";
    case ColType::kText: return "TEXT";
  }
  return "?";
}

// A literal as SQL source: strings re-quoted with '' escaping, so the output
// is itself parseable.
inline std::string literal_sql(const Field& f) {
  if (f.is_null) return "NULL";
  switch (f.type) {
    case ColType::kBool: return f.i64 ? "TRUE" : "FALSE";
    case ColType::kText: {
      std::string out = "'";
      for (char c : f.text) { if (c == '\'') out.push_back('\''); out.push_back(c); }
      out.push_back('\'');
      return out;
    }
    default: return std::to_string(f.i64);
  }
}

// Unit 9: the pieces a join query is made of.
inline std::string column_ref_sql(const ColumnRef& r) {
  return r.table.empty() ? r.column : r.table + "." + r.column;
}

inline std::string condition_sql(const std::vector<Comparison>& terms) {
  std::string out;
  for (std::size_t i = 0; i < terms.size(); ++i) {
    const Comparison& c = terms[i];
    if (i) out += " AND ";
    out += column_ref_sql(c.left) + " " + op_symbol(c.op) + " " +
           (c.right ? column_ref_sql(*c.right) : literal_sql(c.literal));
  }
  return out;
}

inline std::string table_ref_sql(const TableRef& t) {
  return t.alias.empty() ? t.name : t.name + " " + t.alias;
}

inline std::string unparse(const Statement& s) {
  std::ostringstream os;
  switch (s.kind) {
    case Statement::Kind::kCreate: {
      os << "CREATE TABLE " << s.create.table << " (";
      for (std::size_t i = 0; i < s.create.schema.columns.size(); ++i) {
        const Column& c = s.create.schema.columns[i];
        if (i) os << ", ";
        os << c.name << " " << type_keyword(c.type);
        if (!c.nullable) os << " NOT NULL";
      }
      os << ")";
      break;
    }
    case Statement::Kind::kInsert: {
      os << "INSERT INTO " << s.insert.table << " VALUES (";
      for (std::size_t i = 0; i < s.insert.values.size(); ++i) {
        if (i) os << ", ";
        os << literal_sql(s.insert.values[i]);
      }
      os << ")";
      break;
    }
    case Statement::Kind::kSelect: {
      os << "SELECT ";
      if (s.select.star) {
        os << "*";
      } else {
        for (std::size_t i = 0; i < s.select.columns.size(); ++i) {
          if (i) os << ", ";
          os << column_ref_sql(s.select.columns[i]);
        }
      }
      os << " FROM " << table_ref_sql(s.select.from);
      for (const JoinClause& j : s.select.joins)
        os << " JOIN " << table_ref_sql(j.table) << " ON " << condition_sql(j.on);
      if (!s.select.where.empty()) os << " WHERE " << condition_sql(s.select.where);
      break;
    }
  }
  return os.str();
}

}  // namespace labdb
