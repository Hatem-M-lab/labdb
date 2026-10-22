// THE BUG, preserved for study: an index that allows duplicate keys, searched
// with the descent rule of a tree that does not.
//
// Unit 2's B+Tree descends past every separator <= k, to the rightmost child
// that can hold k. With unique keys that is the only child that can: a key
// equal to a separator was copied up from the first entry of the child right of
// it. A secondary index has duplicates, and when a leaf splits inside a run of
// equal keys, entries equal to the new separator sit on BOTH sides of it. A
// search that descends to the right starts its walk after every one of them in
// the left leaf -- and a run that spans many leaves loses all but its last.
//
// The demo asks one question of an orders table with an index on customer --
// how many orders has this customer placed? -- through the engine's own plan,
// then through the same plan with the index scan descending by Unit 2's rule,
// and then runs the two checks that tell them apart. It exits non-zero unless
// the engine passes both checks and the variant fails both.
//
// build: see the Makefile (links the engine).
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "btree.hpp"
#include "buffer_pool.hpp"
#include "executor.hpp"
#include "operator.hpp"
#include "pager.hpp"

using namespace labdb;

namespace {

// The question's plan for one customer, assembled by hand -- the operators
// the planner builds for `SELECT COUNT(*) FROM orders WHERE customer = c`,
// which main() checks -- so that the descent rule can be swapped.
// orders: (oid, customer, cents, day); the index is on column 1.
OpPtr question_plan(Catalog& cat, const OutSchema& orders, std::int64_t c, ChildRule rule) {
  const Key k = order_key(Field::Int64(c));
  OpPtr root = std::make_unique<IndexScan>(cat, TableRef{"orders", ""}, orders, 1, k, k,
                                           "customer = " + std::to_string(c), rule);
  root = std::make_unique<HashAggregate>(std::move(root), std::vector<std::size_t>{},
                                         std::vector<AggSpec>{{AggFunc::kCount, true, 0, ColType::kInt64}},
                                         "COUNT(*)");
  return std::make_unique<Project>(std::move(root), std::vector<std::size_t>{0}, "COUNT(*)");
}

std::int64_t count_with(Catalog& cat, const OutSchema& orders, std::int64_t c, ChildRule rule) {
  OpPtr plan = question_plan(cat, orders, c, rule);
  plan->open();
  Row r;
  plan->next(r);
  plan->close();
  return r[0].i64;
}

}  // namespace

int main() {
  const char* path = "u11_split_run_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 4096);
  Catalog cat(pool);
  Executor e(cat);

  // 20,000 customers, and 200,000 orders in which low customer ids order far
  // more often than high ones -- the skew of Unit 10's session.
  constexpr int kCustomers = 20000, kOrders = 200000;
  std::mt19937_64 rng(1105);
  e.run_sql("CREATE TABLE orders (oid INT64 NOT NULL, customer INT64, cents INT64, day INT32)");
  for (int i = 0; i < kOrders; ++i) {
    const std::uint64_t u = rng() % kCustomers;
    e.run_sql("INSERT INTO orders VALUES (" + std::to_string(i) + ", " + std::to_string(u * u / kCustomers) +
              ", " + std::to_string(100 + rng() % 50000) + ", " + std::to_string(1 + rng() % 365) + ")");
  }
  e.run_sql("CREATE INDEX ON orders (customer)");
  const OutSchema orders = table_schema(cat.schema_of("orders"), TableRef{"orders", ""});
  std::printf("\n%d orders from %d customers, and an index on orders(customer)\n", kOrders, kCustomers);

  // The report: every customer's orders, counted by a scan. No index is used.
  std::map<std::int64_t, std::int64_t> report;
  for (const auto& row : e.run_sql("SELECT customer, COUNT(*) FROM orders GROUP BY customer").rows)
    report[row[0].i64] = row[1].i64;
  std::vector<std::pair<std::int64_t, std::int64_t>> top(report.begin(), report.end());
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

  // The question, through SQL: the engine's plan -- and the hand-built one with
  // the engine's descent rule, which must be the same plan, operator for
  // operator, with the same answer.
  const std::string q = "SELECT COUNT(*) FROM orders WHERE customer = 0";
  const ResultSet right = e.run_sql(q);
  OpPtr same = question_plan(cat, orders, 0, leftmost_child);
  same->open();
  Row same_row;
  same->next(same_row);
  same->close();
  bool ok = plan_text(*same) == e.explain_last() && same_row[0].i64 == right.rows[0][0].i64;
  std::printf("\nthe question, for the customer who orders most:\n  %s\nthe engine's plan, after running:\n%s",
              q.c_str(), e.explain_last().c_str());

  std::printf("\nthe ten customers with the most orders -- the report (a scan), the engine's index, "
              "and the index searched by Unit 2's rule:\n");
  std::printf("  customer   report   engine   Unit 2's rule\n");
  for (int i = 0; i < 10; ++i) {
    const std::int64_t c = top[static_cast<std::size_t>(i)].first;
    std::printf("  %8lld %8lld %8lld %10lld\n", static_cast<long long>(c),
                static_cast<long long>(top[static_cast<std::size_t>(i)].second),
                static_cast<long long>(count_with(cat, orders, c, leftmost_child)),
                static_cast<long long>(count_with(cat, orders, c, rightmost_child)));
  }

  // The checks. (1) Every customer's count through the index must equal the
  // report's, which a scan counted: two plans of one question must agree.
  // (2) The counts through the index, summed over every customer, must equal
  // the number of orders with a customer at all -- whatever the plan.
  const std::int64_t with_customer = e.run_sql("SELECT COUNT(customer) FROM orders").rows[0][0].i64;
  struct Outcome {
    std::size_t wrong = 0;
    std::int64_t missing = 0, sum = 0;
    std::size_t short_by_most_of_their_orders = 0;
  };
  const auto check = [&](ChildRule rule) {
    Outcome o;
    for (std::int64_t c = 0; c < kCustomers; ++c) {
      const std::int64_t n = count_with(cat, orders, c, rule);
      const std::int64_t want = report.count(c) ? report[c] : 0;
      o.sum += n;
      if (n != want) {
        ++o.wrong;
        o.missing += want - n;
        o.short_by_most_of_their_orders += 2 * n < want;
      }
    }
    return o;
  };
  const Outcome bad = check(rightmost_child);
  const Outcome good = check(leftmost_child);
  for (const auto& [name, o] : {std::pair<const char*, const Outcome&>{"Unit 2's rule", bad},
                                std::pair<const char*, const Outcome&>{"the engine", good}}) {
    std::printf("\nthe checks, against %s:\n", name);
    std::printf("  (1) customers whose count differs from the report: %zu of %d (%lld orders missing; "
                "%zu shown less than half their orders)\n",
                o.wrong, kCustomers, static_cast<long long>(o.missing), o.short_by_most_of_their_orders);
    std::printf("  (2) counts summed over every customer: %lld, against %lld orders with a customer%s\n",
                static_cast<long long>(o.sum), static_cast<long long>(with_customer),
                o.sum == with_customer ? "" : " -- they must be equal");
  }

  ok = ok && good.wrong == 0 && good.sum == with_customer && bad.wrong > 0 && bad.sum < with_customer;
  ::unlink(path);
  std::printf("\n%s\n", ok ? "u11_split_run_demo: the checks catch the bug, and the engine passes them"
                           : "u11_split_run_demo: UNEXPECTED RESULT");
  return ok ? 0 : 1;
}
