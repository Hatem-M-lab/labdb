#pragma once
// Unit 13's workload for crashing: a fixed sequence of transactions over two
// tables; for each prefix of it, what a database must then hold, worked out
// from a model of the rows, not by the engine; and the digest of what a
// database does hold, in the same form. Not part of the engine.
//
// The transactions, in order: CREATE TABLE a; a run of single-statement
// INSERTs; an explicit transaction of many; CREATE TABLE b; a transaction
// that ROLLs BACK; CREATE INDEX on a.v; INSERTs into both tables, one at a
// time and in transactions large enough to spill pages out of a small pool
// into the log before they commit; a transaction that fails halfway (an
// INSERT into a table that does not exist) and so rolls back whole; CREATE
// INDEX on b.a_id; more INSERTs. Texts of varied length make the heaps grow
// by whole pages, and the key counts make both trees split.

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "buffer_pool.hpp"
#include "executor.hpp"
#include "pager.hpp"
#include "wal.hpp"

namespace labdb::test {

using Txn = std::vector<std::string>;  // the statements of one transaction

struct Workload {
  std::vector<Txn> txns;
  std::vector<std::string> expected;  // [i]: the digest after the first i transactions
};

namespace detail {
// The model: each table's rows by key, rendered as digest() renders them.
struct Model {
  struct Table {
    bool exists = false, indexed = false;
    std::map<std::int64_t, std::string> rows;  // id -> "id,col,text,"
  } a, b;
  static std::string render(const Table& t, const char* name, const char* col) {
    if (!t.exists) return std::string("[no ") + name + "]";
    std::string d = std::string("[") + name + " " + std::to_string(t.rows.size()) + "]";
    std::int64_t sum = 0;
    for (const auto& [id, r] : t.rows) d += r + ";", sum += id;
    if (t.indexed) {
      const std::string got = std::to_string(t.rows.size()) + "/" + (t.rows.empty() ? "-" : std::to_string(sum));
      d += std::string(" index on ") + col + " agrees " + got;
    }
    return d;
  }
  std::string digest() const { return render(a, "a", "v") + render(b, "b", "a_id"); }
};
}  // namespace detail

inline Workload crash_workload() {
  Workload w;
  detail::Model m;
  w.expected.push_back(m.digest());
  std::mt19937_64 rng(1301);
  std::int64_t next_a = 0, next_b = 0;
  // a transaction's rows, staged until it is known to commit
  std::vector<std::pair<std::int64_t, std::string>> stage_a, stage_b;
  const auto text = [&](int n) { return std::string(static_cast<std::size_t>(n), static_cast<char>('a' + rng() % 26)); };
  const auto row_a = [&] {
    const std::int64_t id = next_a++, v = static_cast<std::int64_t>(rng() % 1000);
    const std::string s = text(static_cast<int>(rng() % 120));
    stage_a.push_back({id, std::to_string(id) + "," + std::to_string(v) + "," + s + ","});
    return "INSERT INTO a VALUES (" + std::to_string(id) + ", " + std::to_string(v) + ", '" + s + "')";
  };
  const auto row_b = [&] {
    const std::int64_t id = next_b++, ref = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(next_a + 1));
    const std::string s = text(static_cast<int>(rng() % 60));
    stage_b.push_back({id, std::to_string(id) + "," + std::to_string(ref) + "," + s + ","});
    return "INSERT INTO b VALUES (" + std::to_string(id) + ", " + std::to_string(ref) + ", '" + s + "')";
  };
  const auto done = [&](Txn t, bool commits) {  // the transaction, and the model after it
    if (commits) {
      for (auto& [id, r] : stage_a) m.a.rows[id] = r;
      for (auto& [id, r] : stage_b) m.b.rows[id] = r;
    }
    stage_a.clear();
    stage_b.clear();
    w.txns.push_back(std::move(t));
    w.expected.push_back(m.digest());
  };
  const auto many = [&](int na, int nb, bool commits) {
    Txn t{"BEGIN"};
    for (int i = 0; i < na; ++i) t.push_back(row_a());
    for (int i = 0; i < nb; ++i) t.push_back(row_b());
    t.push_back(commits ? "COMMIT" : "ROLLBACK");
    done(t, commits);
  };
  m.a.exists = true;
  done({"CREATE TABLE a (id INT64 NOT NULL, v INT64, s TEXT)"}, true);
  for (int i = 0; i < 40; ++i) done({row_a()}, true);
  many(300, 0, true);
  m.b.exists = true;
  done({"CREATE TABLE b (id INT64 NOT NULL, a_id INT64, note TEXT)"}, true);
  many(0, 120, false);
  next_b = 0;  // the rolled-back rows were never there
  m.a.indexed = true;
  done({"CREATE INDEX ON a (v)"}, true);
  for (int i = 0; i < 30; ++i) done({i % 2 ? row_b() : row_a()}, true);
  many(200, 250, true);
  {
    Txn t{"BEGIN", row_a(), row_b(), "INSERT INTO nowhere VALUES (1)", "COMMIT"};
    --next_a, --next_b;  // it fails, and takes its rows with it
    done(t, false);
  }
  m.b.indexed = true;
  done({"CREATE INDEX ON b (a_id)"}, true);
  for (int i = 0; i < 30; ++i) done({i % 3 ? row_b() : row_a()}, true);
  many(150, 150, true);
  for (int i = 0; i < 10; ++i) done({row_a()}, true);
  return w;
}

// Run the transactions in order on `e`, calling ack(i) once transaction i
// (1-based) is over -- committed, rolled back, or failed and rolled back.
inline void run_workload(Executor& e, const std::vector<Txn>& w, const std::function<void(int)>& ack) {
  for (std::size_t i = 0; i < w.size(); ++i) {
    try {
      for (const std::string& sql : w[i]) e.run_sql(sql);
    } catch (const ExecError&) {
      // the failing transaction: the executor has rolled it back
    }
    ack(static_cast<int>(i + 1));
  }
}

// What a database holds, as one string: every row of each table in key
// order, and for each indexed column the count and sum of ids found through
// the index and through a scan -- which must agree, or the string says so.
inline std::string digest(Executor& e, Catalog& cat) {
  std::string d;
  for (const char* t : {"a", "b"}) {
    const auto info = cat.find_table(t);
    if (!info) {
      d += std::string("[no ") + t + "]";
      continue;
    }
    d += std::string("[") + t + " " + std::to_string(info->rows) + "]";
    for (const auto& row : e.run_sql(std::string("SELECT * FROM ") + t).rows) {
      for (const Field& f : row) d += (f.is_null ? "NULL" : f.type == ColType::kText ? f.text : std::to_string(f.i64)) + ",";
      d += ";";
    }
    const char* col = std::string(t) == "a" ? "v" : "a_id";
    if (info->indexed[1]) {
      std::string got[2];
      int k = 0;
      for (auto path : {Executor::AccessPath::kIndex, Executor::AccessPath::kScan}) {
        e.set_access_path(path);
        const ResultSet r = e.run_sql(std::string("SELECT COUNT(*), SUM(id) FROM ") + t + " WHERE " + col + " >= 0");
        got[k++] = std::to_string(r.rows[0][0].i64) + "/" + (r.rows[0][1].is_null ? "-" : std::to_string(r.rows[0][1].i64));
      }
      e.set_access_path(Executor::AccessPath::kAuto);
      d += std::string(" index on ") + col + (got[0] == got[1] ? " agrees " : " DISAGREES: " + got[0] + " vs ") + got[1];
    }
  }
  return d;
}

}  // namespace labdb::test
