// Challenge 12.5, the forensic trap: an empty batch read as the end.
//
// next_batch() has two answers: true, with rows in the batch, or false, there
// are no more. A Filter written for batches first filters the batch it was
// handed and then has to say which -- and the obvious line,
//
//     return out.count() > 0;
//
// says "no more" whenever one batch had no row that passed. The operator above
// stops asking. No error, no crash: the answer is simply short, by however
// many rows were still to come. How short depends on where the first batch
// with no survivors falls, and so on the data and on the batch size.
//
// The demo asks the shop one question through two copies of the same plan --
// one with the engine's Filter, one with that line -- at six batch sizes, then
// checks both against the row path. It fails (exits non-zero) unless the checks
// catch the eager Filter and pass the engine's.
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "executor.hpp"
#include "operator.hpp"
#include "pager.hpp"
#include "shop.hpp"

using namespace labdb;

namespace {

// The Filter as it is easy to write for batches. Its row path is the engine's.
class EagerFilter : public Operator {
 public:
  EagerFilter(OpPtr child, std::vector<BoundComparison> preds, std::string text)
      : child_(std::move(child)), preds_(std::move(preds)), text_(std::move(text)) {
    schema_ = child_->schema();
    inputs_ = {child_.get()};
  }
  void open() override { child_->open(); }
  void close() override { child_->close(); }
  std::string describe() const override { return "EagerFilter " + text_; }

 protected:
  bool produce(Row& out) override {
    while (child_->next(out)) {
      bool pass = true;
      for (const BoundComparison& p : preds_) pass = pass && eval(p, out);
      if (pass) return true;
    }
    return false;
  }
  bool produce_batch(Batch& out, std::size_t max) override {
    if (!child_->next_batch(out, max)) return false;
    for (const BoundComparison& p : preds_) select(p, out);
    return out.count() > 0;  // the bug: "none passed in this batch" read as "no more input"
  }

 private:
  OpPtr child_;
  std::vector<BoundComparison> preds_;
  std::string text_;
};

// SELECT COUNT(*), SUM(cents) FROM orders WHERE cents > X, built by hand, with
// the Filter of one kind or the other.
OpPtr plan(Catalog& cat, std::int64_t x, bool eager) {
  OpPtr scan = std::make_unique<SeqScan>(cat, TableRef{"orders", ""});
  BoundComparison over{2, TokenType::kGt, false, false, false, 0, Field::Int64(x)};  // cents > x
  const std::string text = "cents > " + std::to_string(x);
  OpPtr filter = eager ? OpPtr(std::make_unique<EagerFilter>(std::move(scan), std::vector{over}, text))
                       : OpPtr(std::make_unique<Filter>(std::move(scan), std::vector{over}, text));
  return std::make_unique<HashAggregate>(std::move(filter), std::vector<std::size_t>{},
                                         std::vector<AggSpec>{{AggFunc::kCount, true, 0, ColType::kInt64},
                                                              {AggFunc::kSum, false, 2, ColType::kInt64}},
                                         "COUNT(*), SUM(cents)");
}

struct Answer {
  std::int64_t count = 0, sum = 0;
  std::uint64_t scanned = 0;  // rows the scan produced
  std::string plan;
};

Answer ask(Catalog& cat, std::int64_t x, bool eager, std::size_t batch) {
  OpPtr root = plan(cat, x, eager);
  root->set_batch_rows(batch);
  root->open();
  Answer a;
  if (batch == 0) {
    Row r;
    root->next(r);
    a.count = r[0].i64;
    a.sum = r[1].is_null ? 0 : r[1].i64;
  } else {
    Batch b;
    root->next_batch(b);
    a.count = b.cols[0].ints[0];
    a.sum = b.cols[1].nulls[0] ? 0 : b.cols[1].ints[0];
  }
  root->close();
  const Operator* leaf = root.get();
  while (!leaf->children().empty()) leaf = leaf->children()[0];
  a.scanned = leaf->rows_out();
  a.plan = plan_text(*root);
  return a;
}

}  // namespace

int main() {
  const char* path = "u12_empty_batch_demo.db";
  ::unlink(path);
  Pager pager(path);
  BufferPool pool(pager, 8192);
  Catalog cat(pool);
  Executor e(cat);
  test::build_shop(e);

  const std::size_t sizes[] = {1, 16, 64, 256, 1024, 4096};
  std::printf("\nthe shop's 200000 orders: how many above 900.00, and their total?\n");
  std::printf("  SELECT COUNT(*), SUM(cents) FROM orders WHERE cents > 90000\n\n");
  const Answer truth = ask(cat, 90000, false, 0);
  std::printf("  a row at a time: %lld orders, %lld cents, %llu rows scanned\n\n",
              static_cast<long long>(truth.count), static_cast<long long>(truth.sum),
              static_cast<unsigned long long>(truth.scanned));
  std::printf("  batch rows     the engine's Filter              the eager Filter\n");
  std::printf("                 orders  rows scanned             orders  rows scanned\n");
  int caught = 0, engine_wrong = 0;
  Answer shown;
  for (std::size_t n : sizes) {
    const Answer good = ask(cat, 90000, false, n);
    const Answer bad = ask(cat, 90000, true, n);
    std::printf("  %10zu  %9lld  %12llu  %18lld  %12llu%s\n", n, static_cast<long long>(good.count),
                static_cast<unsigned long long>(good.scanned), static_cast<long long>(bad.count),
                static_cast<unsigned long long>(bad.scanned), bad.count != truth.count ? "   <- short" : "");
    engine_wrong += good.count != truth.count || good.sum != truth.sum || good.scanned != truth.scanned;
    caught += bad.count != truth.count || bad.scanned != truth.scanned;
    if (n == 1024) shown = bad;
  }
  std::printf("\nthe eager Filter's plan in batches of 1024, after running:\n%s", shown.plan.c_str());

  // Where did it stop? The first batch of 1,024 orders, in key order, with no
  // order above 90000 in it.
  {
    TableCursor cur = cat.open_cursor("orders");
    Row r;
    std::uint64_t i = 0, first_empty = 0;
    std::uint64_t in_batch = 0;
    bool found = false;
    while (cur.next(r)) {
      in_batch += r[2].i64 > 90000;
      if (++i % 1024 == 0) {
        if (in_batch == 0 && !found) {
          first_empty = i / 1024;  // 1-based
          found = true;
        }
        in_batch = 0;
      }
    }
    if (found)
      std::printf("\nin key order, the first run of 1024 orders with none above 90000 is the %lluth: orders %llu to %llu\n",
                  static_cast<unsigned long long>(first_empty),
                  static_cast<unsigned long long>((first_empty - 1) * 1024),
                  static_cast<unsigned long long>(first_empty * 1024 - 1));
    else
      std::printf("\nin key order, every run of 1024 orders has one above 90000\n");
  }

  // The same Filter on a question whose every batch keeps rows: right at
  // every size. A test with this selectivity cannot see the bug.
  std::printf("\nthe eager Filter on WHERE cents < 50000, which most orders pass:\n  batch rows");
  for (std::size_t n : sizes) std::printf(" %7zu", n);
  std::printf("\n  orders    ");
  int hidden_ok = 0;
  {
    for (std::size_t n : sizes) {
      OpPtr scan = std::make_unique<SeqScan>(cat, TableRef{"orders", ""});
      BoundComparison under{2, TokenType::kLt, false, false, false, 0, Field::Int64(50000)};
      OpPtr root = std::make_unique<HashAggregate>(
          std::make_unique<EagerFilter>(std::move(scan), std::vector{under}, "cents < 50000"),
          std::vector<std::size_t>{}, std::vector<AggSpec>{{AggFunc::kCount, true, 0, ColType::kInt64}},
          "COUNT(*)");
      root->set_batch_rows(n);
      root->open();
      Batch b;
      root->next_batch(b);
      const std::int64_t got = b.cols[0].ints[0];
      root->close();
      e.set_batch_rows(0);
      const std::int64_t want = e.run_sql("SELECT COUNT(*) FROM orders WHERE cents < 50000").rows[0][0].i64;
      hidden_ok += got == want;
      std::printf(" %7lld", static_cast<long long>(got));
    }
    e.set_batch_rows(Executor::kChoose);
  }
  std::printf("   (right at %d of 6 sizes)\n", hidden_ok);

  std::printf("\nthe checks, against the row path, at %zu batch sizes:\n", std::size(sizes));
  std::printf("  the eager Filter: wrong at %d of them\n", caught);
  std::printf("  the engine's Filter: wrong at %d\n", engine_wrong);
  ::unlink(path);
  if (caught == 0 || engine_wrong != 0) {
    std::printf("\nu12_empty_batch_demo: FAIL\n");
    return 1;
  }
  std::printf("\nu12_empty_batch_demo: the checks catch the bug, and the engine passes them\n");
  return 0;
}
