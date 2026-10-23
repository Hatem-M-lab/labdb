#include "operator.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <sstream>
#include <tuple>

namespace labdb {

// ------------------------------------------------------- names and values

OutSchema concat(const OutSchema& left, const OutSchema& right) {
  OutSchema out = left;
  out.insert(out.end(), right.begin(), right.end());
  return out;
}

std::size_t resolve(const OutSchema& s, const ColumnRef& ref) {
  const std::string shown =
      ref.table.empty() ? ref.column : ref.table + "." + ref.column;
  bool table_seen = ref.table.empty();
  std::size_t found = 0, matches = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!ref.table.empty()) {
      if (s[i].table != ref.table) continue;
      table_seen = true;
    }
    if (s[i].column.name == ref.column) {
      if (matches == 0) found = i;
      ++matches;
    }
  }
  if (!table_seen)
    throw ExecError{"no table named " + ref.table + " is in scope for " + shown};
  if (matches == 0) throw ExecError{"no such column: " + shown};
  if (matches > 1)
    throw ExecError{"ambiguous column: " + shown + " (qualify it with its table)"};
  return found;
}

namespace {

// -1, 0, or 1. Strings compare by bytes; when either side is a DOUBLE (Unit
// 10) both compare as doubles; everything else compares by integer value.
inline int compare_fields(const Field& a, const Field& b, bool as_text, bool as_double) {
  if (as_text) {
    const int c = a.text.compare(b.text);
    return (c > 0) - (c < 0);
  }
  if (as_double) {
    const double x = a.as_double(), y = b.as_double();
    return (x > y) - (x < y);
  }
  return (a.i64 > b.i64) - (a.i64 < b.i64);
}

inline bool holds(TokenType op, int c) {
  switch (op) {
    case TokenType::kEq: return c == 0;
    case TokenType::kNe: return c != 0;
    case TokenType::kLt: return c < 0;
    case TokenType::kLe: return c <= 0;
    case TokenType::kGt: return c > 0;
    case TokenType::kGe: return c >= 0;
    default: throw ExecError{"unknown comparison operator"};
  }
}

}  // namespace

int compare_values(const Field& a, const Field& b) {
  return compare_fields(a, b, a.type == ColType::kText,
                        a.type == ColType::kDouble || b.type == ColType::kDouble);
}

bool eval(const BoundComparison& c, const Row& row) {
  const Field& a = row[c.left];
  const Field& b = c.right_is_column ? row[c.right] : c.literal;
  if (a.is_null || b.is_null) return false;
  return holds(c.op, compare_fields(a, b, c.as_text, c.as_double));
}

bool eval_pair(const BoundComparison& c, const Row& l, const Row& r,
               std::size_t nleft) {
  const Field& a = c.left < nleft ? l[c.left] : r[c.left - nleft];
  const Field& b = !c.right_is_column ? c.literal
                   : c.right < nleft  ? l[c.right]
                                      : r[c.right - nleft];
  if (a.is_null || b.is_null) return false;
  return holds(c.op, compare_fields(a, b, c.as_text, c.as_double));
}

// ------------------------------------------------- batches of rows (Unit 12)

namespace {

// Of the batch's live rows, keep those for which pred(position) is true. The
// position is written to the selection whether or not the row passes, and the
// count moves on only if it does: no branch on the outcome, so a predicate
// that passes half the rows at random costs no more than one that passes all.
template <class Pred>
inline void keep_if(Batch& b, Pred pred) {
  std::uint32_t* sel = b.sel.data();
  std::size_t n = 0;
  if (b.selected) {
    const std::size_t live = b.live;
    for (std::size_t k = 0; k < live; ++k) {
      const std::uint32_t i = sel[k];
      sel[n] = i;
      n += pred(i) ? 1 : 0;
    }
  } else {
    const std::size_t size = b.size;
    for (std::size_t i = 0; i < size; ++i) {
      sel[n] = static_cast<std::uint32_t>(i);
      n += pred(i) ? 1 : 0;
    }
  }
  b.selected = true;
  b.live = n;
}

// Call f(k, i) for each live row of a batch: its index k among the live rows
// and its position i. The test of whether the batch is selected is made once,
// not once a row.
template <class F>
inline void for_live(const Batch& b, F f) {
  const std::size_t n = b.count();
  if (b.selected) {
    const std::uint32_t* s = b.sel.data();
    for (std::size_t k = 0; k < n; ++k) f(k, static_cast<std::size_t>(s[k]));
  } else {
    for (std::size_t k = 0; k < n; ++k) f(k, k);
  }
}

// Call f with the comparison `op` asks for, as a function object, so that the
// loop f builds is compiled once per operator with the comparison inlined.
template <class F>
inline void with_op(TokenType op, F f) {
  switch (op) {
    case TokenType::kEq: f(std::equal_to<>{}); return;
    case TokenType::kNe: f(std::not_equal_to<>{}); return;
    case TokenType::kLt: f(std::less<>{}); return;
    case TokenType::kLe: f(std::less_equal<>{}); return;
    case TokenType::kGt: f(std::greater<>{}); return;
    case TokenType::kGe: f(std::greater_equal<>{}); return;
    default: throw ExecError{"unknown comparison operator"};
  }
}

// A column's value as a double, for comparisons with a DOUBLE on one side.
inline double as_double_at(const ColumnVector& c, std::size_t i) {
  return c.type == ColType::kDouble ? c.doubles[i] : static_cast<double>(c.ints[i]);
}

// dst row j := src row idx[j], for j < m.
void gather(ColumnVector& dst, const ColumnVector& src, const std::uint32_t* idx, std::size_t m) {
  std::uint8_t* dn = dst.nulls.data();
  const std::uint8_t* sn = src.nulls.data();
  for (std::size_t j = 0; j < m; ++j) dn[j] = sn[idx[j]];
  if (dst.type == ColType::kText) {
    for (std::size_t j = 0; j < m; ++j) dst.texts[j].assign(src.texts[idx[j]]);
  } else if (dst.type == ColType::kDouble) {
    for (std::size_t j = 0; j < m; ++j) dst.doubles[j] = src.doubles[idx[j]];
  } else {
    std::int64_t* d = dst.ints.data();
    const std::int64_t* x = src.ints.data();
    for (std::size_t j = 0; j < m; ++j) d[j] = x[idx[j]];
  }
}

}  // namespace

void select(const BoundComparison& c, Batch& b) {
  if (b.count() == 0) return;
  const ColumnVector& L = b.cols[c.left];
  const std::uint8_t* ln = L.nulls.data();
  if (!c.right_is_column && c.literal.is_null) {  // compared with NULL: never true
    b.selected = true;
    b.live = 0;
    return;
  }
  const ColumnVector* R = c.right_is_column ? &b.cols[c.right] : nullptr;
  const std::uint8_t* rn = R ? R->nulls.data() : nullptr;
  if (c.as_text) {
    const std::string* a = L.texts.data();
    if (R) {
      const std::string* r = R->texts.data();
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) { return (ln[i] | rn[i]) == 0 && cmp(a[i].compare(r[i]), 0); });
      });
    } else {
      const std::string& v = c.literal.text;
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) { return ln[i] == 0 && cmp(a[i].compare(v), 0); });
      });
    }
  } else if (c.as_double) {
    if (R) {
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) {
          return (ln[i] | rn[i]) == 0 && cmp(as_double_at(L, i), as_double_at(*R, i));
        });
      });
    } else {
      const double v = c.literal.as_double();
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) { return ln[i] == 0 && cmp(as_double_at(L, i), v); });
      });
    }
  } else {
    const std::int64_t* a = L.ints.data();
    if (R) {
      const std::int64_t* r = R->ints.data();
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) { return ((ln[i] | rn[i]) == 0) & cmp(a[i], r[i]); });
      });
    } else {
      const std::int64_t v = c.literal.i64;
      with_op(c.op, [&](auto cmp) {
        keep_if(b, [&](std::size_t i) { return (ln[i] == 0) & cmp(a[i], v); });
      });
    }
  }
}

// -------------------------------------------------------------- plan text

namespace {

void plan_lines(const Operator& op, std::size_t depth, bool estimates, std::ostringstream& os) {
  std::string line(depth * 2, ' ');
  line += op.describe();
  const std::size_t width = 60;
  os << line << std::string(line.size() < width ? width - line.size() : 1, ' ')
     << "rows=" << op.rows_out();
  if (estimates && op.estimate()) {
    char buf[32];
    std::snprintf(buf, sizeof buf, " est=%.0f", *op.estimate());
    os << buf;
  }
  os << "\n";
  const std::string d = op.detail();
  if (!d.empty()) os << std::string(depth * 2 + 4, ' ') << d << "\n";
  for (const Operator* c : op.children()) plan_lines(*c, depth + 1, estimates, os);
}

std::string table_text(const TableRef& t) {
  return t.alias.empty() ? t.name : t.name + " AS " + t.alias;
}

}  // namespace

OutSchema table_schema(const Schema& s, const TableRef& t) {
  OutSchema out;
  out.reserve(s.columns.size());
  for (const Column& c : s.columns) out.push_back({t.label(), c});
  return out;
}

std::string plan_text(const Operator& root, bool estimates) {
  std::ostringstream os;
  plan_lines(root, 0, estimates, os);
  return os.str();
}

// ------------------------------------------------ both ways to run (Unit 12)

void Operator::set_batch_rows(std::size_t n) {
  batch_rows_ = n;
  types_.clear();
  for (const OutColumn& c : schema_) types_.push_back(c.column.type);
  for (Operator* in : inputs_) in->set_batch_rows(n);
}

bool Operator::produce_batch(Batch& out, std::size_t max) {
  check_that(batch_rows_ > 0, "next_batch on an operator not set to run in batches");
  const std::size_t n = cap(max);
  out.format(types_, 0);
  std::size_t i = 0, room = out.room();
  while (i < n && produce(pack_)) {
    if (i == room) out.ensure(i + 1), room = out.room();
    out.set_row(i++, pack_);
  }
  out.size = i;
  return i > 0;
}

bool Operator::pull(Operator& in, BatchReader& reader, Row& out) {
  return batch_rows_ ? reader.next(in, out) : in.next(out);
}

// ---------------------------------------------------------------- sources

SeqScan::SeqScan(Catalog& cat, const TableRef& table, OutSchema schema)
    : cat_(cat), table_(table) {
  schema_ = std::move(schema);
}
SeqScan::SeqScan(Catalog& cat, const TableRef& table)
    : SeqScan(cat, table, table_schema(cat.schema_of(table.name), table)) {}
void SeqScan::open() { cur_.emplace(cat_.open_cursor(table_.name)); }
void SeqScan::close() { cur_.reset(); }
bool SeqScan::produce(Row& out) { return cur_->next(out); }
bool SeqScan::produce_batch(Batch& out, std::size_t max) {
  out.format(types(), 0);  // the cursor makes room as it fills
  return cur_->fill(out, cap(max)) > 0;
}
std::string SeqScan::describe() const { return "SeqScan " + table_text(table_); }

IndexLookup::IndexLookup(Catalog& cat, const TableRef& table, OutSchema schema,
                         std::optional<Key> key)
    : cat_(cat), table_(table), key_(key) {
  schema_ = std::move(schema);
}
bool IndexLookup::produce(Row& out) {
  if (done_) return false;
  done_ = true;
  if (!key_) return false;  // key = NULL: no row has a NULL key
  std::optional<Row> row = cat_.get_by_key(table_.name, *key_);
  if (!row) return false;
  out = std::move(*row);
  return true;
}
std::string IndexLookup::describe() const {
  return "IndexLookup " + table_text(table_) + " key = " +
         (key_ ? std::to_string(static_cast<std::int64_t>(*key_)) : "NULL");
}

IndexScan::IndexScan(Catalog& cat, const TableRef& table, OutSchema schema, std::size_t col,
                     Key lo, Key hi, std::string range_text, ChildRule rule)
    : cat_(cat), table_(table), col_(col), lo_(lo), hi_(hi),
      range_text_(std::move(range_text)), rule_(rule) {
  schema_ = std::move(schema);
}

void IndexScan::open() {
  cur_.reset();
  if (lo_ > hi_) return;  // an empty range: the index is not even opened
  cur_.emplace(cat_.open_index(table_.name, col_, rule_));
  cur_->seek(lo_, hi_);
}

bool IndexScan::produce_batch(Batch& out, std::size_t max) {
  if (!cur_) return false;  // an empty range
  out.format(types(), 0);
  return cur_->fill(out, 0, cap(max)) > 0;
}

std::string IndexScan::describe() const {
  return "IndexScan " + table_text(table_) + " (" + schema_[col_].column.name + "): " +
         range_text_;
}

RowsScan::RowsScan(OutSchema schema, const std::vector<Row>& rows) : rows_(rows) {
  schema_ = std::move(schema);
}
bool RowsScan::produce(Row& out) {
  if (i_ >= rows_.size()) return false;
  out = rows_[i_++];
  return true;
}
std::string RowsScan::describe() const {
  return "RowsScan (" + std::to_string(rows_.size()) + " rows in memory)";
}

ColumnsScan::ColumnsScan(OutSchema schema, const std::vector<ColumnVector>& cols, std::size_t rows)
    : cols_(cols), rows_(rows) {
  schema_ = std::move(schema);
}
bool ColumnsScan::produce(Row& out) {
  if (i_ >= rows_) return false;
  out.resize(cols_.size());
  for (std::size_t c = 0; c < cols_.size(); ++c) cols_[c].get(i_, out[c]);
  ++i_;
  return true;
}
bool ColumnsScan::produce_batch(Batch& out, std::size_t max) {
  if (i_ >= rows_) return false;
  const std::size_t n = std::min(cap(max), rows_ - i_);
  out.format(types(), n);
  for (std::size_t c = 0; c < cols_.size(); ++c) {
    const ColumnVector& src = cols_[c];
    ColumnVector& dst = out.cols[c];
    std::copy_n(src.nulls.begin() + static_cast<std::ptrdiff_t>(i_), n, dst.nulls.begin());
    if (src.type == ColType::kText)
      for (std::size_t j = 0; j < n; ++j) dst.texts[j].assign(src.texts[i_ + j]);
    else if (src.type == ColType::kDouble)
      std::copy_n(src.doubles.begin() + static_cast<std::ptrdiff_t>(i_), n, dst.doubles.begin());
    else
      std::copy_n(src.ints.begin() + static_cast<std::ptrdiff_t>(i_), n, dst.ints.begin());
  }
  out.size = n;
  i_ += n;
  return true;
}
std::string ColumnsScan::describe() const {
  return "ColumnsScan (" + std::to_string(rows_) + " rows in memory)";
}

// ------------------------------------------------------------- one input

Filter::Filter(OpPtr child, std::vector<BoundComparison> preds, std::string text)
    : child_(std::move(child)), preds_(std::move(preds)), text_(std::move(text)) {
  schema_ = child_->schema();
  inputs_ = {child_.get()};
}

bool Filter::produce(Row& out) {
  while (child_->next(out)) {
    bool pass = true;
    for (const BoundComparison& p : preds_)
      if (!eval(p, out)) { pass = false; break; }
    if (pass) return true;
  }
  return false;
}

// The batch is filtered where it lies: each term narrows its selection, and
// no row is moved. A batch none of whose rows pass is not handed up -- the
// operator above would take a batch with no rows for the end of the input --
// so the Filter reads on until a batch keeps a row or the input ends.
bool Filter::produce_batch(Batch& out, std::size_t max) {
  for (;;) {
    if (!child_->next_batch(out, max)) return false;
    for (const BoundComparison& p : preds_) {
      select(p, out);
      if (out.count() == 0) break;
    }
    if (out.count() > 0) return true;
  }
}

Project::Project(OpPtr child, std::vector<std::size_t> cols, std::string text)
    : child_(std::move(child)), cols_(std::move(cols)), text_(std::move(text)) {
  for (std::size_t c : cols_) schema_.push_back(child_->schema()[c]);
  inputs_ = {child_.get()};
}

bool Project::produce(Row& out) {
  if (!child_->next(in_)) return false;
  out.resize(cols_.size());
  for (std::size_t i = 0; i < cols_.size(); ++i) out[i] = in_[cols_[i]];
  return true;
}

// A column is moved, not copied: its vectors are swapped into the output, and
// the input batch is handed whatever the output held, to be refilled. Only a
// column the select list names twice is copied, for its second use.
bool Project::produce_batch(Batch& out, std::size_t max) {
  if (!child_->next_batch(in_batch_, max)) return false;
  out.cols.resize(cols_.size());
  for (std::size_t i = 0; i < cols_.size(); ++i) {
    std::size_t first = i;
    for (std::size_t j = 0; j < i; ++j)
      if (cols_[j] == cols_[i]) {
        first = j;
        break;
      }
    if (first == i) std::swap(out.cols[i], in_batch_.cols[cols_[i]]);
    else out.cols[i] = out.cols[first];
  }
  out.size = in_batch_.size;
  out.selected = in_batch_.selected;
  out.live = in_batch_.live;
  std::swap(out.sel, in_batch_.sel);
  return true;
}

// ------------------------------------------------------------ two inputs

NestedLoopJoin::NestedLoopJoin(OpPtr left, OpPtr right,
                               std::vector<BoundComparison> on, std::string text)
    : left_(std::move(left)), right_(std::move(right)), on_(std::move(on)),
      text_(std::move(text)), nleft_(left_->schema().size()) {
  schema_ = concat(left_->schema(), right_->schema());
  inputs_ = {left_.get(), right_.get()};
}

void NestedLoopJoin::open() {
  inner_.clear();
  lread_.reset();
  rread_.reset();
  right_->open();  // read the right input once, and keep it
  Row r;
  while (pull(*right_, rread_, r)) inner_.push_back(std::move(r));
  right_->close();
  left_->open();
  have_outer_ = false;
}

void NestedLoopJoin::close() { left_->close(); }

bool NestedLoopJoin::produce(Row& out) {
  for (;;) {
    if (!have_outer_) {
      if (!pull(*left_, lread_, outer_)) return false;
      have_outer_ = true;
      j_ = 0;
    }
    while (j_ < inner_.size()) {
      const Row& in = inner_[j_++];
      bool match = true;
      for (const BoundComparison& c : on_)
        if (!eval_pair(c, outer_, in, nleft_)) { match = false; break; }
      if (match) {
        join_rows(outer_, in, out);
        return true;
      }
    }
    have_outer_ = false;  // this left row has met every right row
  }
}

std::string NestedLoopJoin::describe() const { return "NestedLoopJoin ON " + text_; }

std::string NestedLoopJoin::detail() const {
  const std::uint64_t pairs = left_->rows_out() * inner_.size();
  return "right input buffered: " + std::to_string(inner_.size()) +
         " rows; row pairs compared: " + std::to_string(pairs);
}

HashJoin::HashJoin(OpPtr left, OpPtr right, std::vector<KeyPair> keys,
                   std::vector<BoundComparison> residual, std::string text,
                   JoinHashTable::MixFn mix, bool build_left)
    : left_(std::move(left)), right_(std::move(right)),
      residual_(std::move(residual)), text_(std::move(text)), mix_(mix),
      build_left_(build_left), nleft_(left_->schema().size()), table_(mix) {
  schema_ = concat(left_->schema(), right_->schema());
  inputs_ = {left_.get(), right_.get()};
  for (const auto& [l, r] : keys) {
    lkeys_.push_back(l);
    rkeys_.push_back(r);
    key_text_.push_back(left_->schema()[l].column.type == ColType::kText);
  }
}

// The raw hash of a row's join key. An integer's raw hash is the integer
// itself, exactly as std::hash<int64_t> is in libstdc++; spreading it across
// buckets is the table's job (mix64), not this function's.
std::uint64_t HashJoin::key_hash(const Row& row,
                                 const std::vector<std::size_t>& cols) const {
  std::uint64_t h = 0;
  for (std::size_t k = 0; k < cols.size(); ++k) {
    const Field& f = row[cols[k]];
    const std::uint64_t v =
        key_text_[k] ? fnv1a(f.text) : static_cast<std::uint64_t>(f.i64);
    h = (k == 0) ? v : (h * 0x9E3779B97F4A7C15ULL) ^ v;
  }
  return h;
}

bool HashJoin::null_key(const Row& row, const std::vector<std::size_t>& cols) const {
  for (std::size_t c : cols)
    if (row[c].is_null) return true;
  return false;
}

bool HashJoin::keys_equal(const Row& l, const Row& r) const {
  for (std::size_t k = 0; k < lkeys_.size(); ++k) {
    const Field& a = l[lkeys_[k]];
    const Field& b = r[rkeys_[k]];
    if (key_text_[k] ? a.text != b.text : a.i64 != b.i64) return false;
  }
  return true;
}

void HashJoin::open() {
  if (batch_rows_) {  // Unit 12
    build_batches();
    (build_left_ ? right_ : left_)->open();
    probe_batch_.size = 0;
    probe_batch_.selected = false;
    pk_ = 0;
    in_bucket_ = false;
    return;
  }
  // Build: file every row of the build side under its key. A row whose key is
  // NULL can never match (NULL equals nothing, not even NULL), so it is not
  // filed. The build side is the right input, or the left with build_left.
  Operator& build = build_left_ ? *left_ : *right_;
  const std::vector<std::size_t>& bkeys = build_left_ ? lkeys_ : rkeys_;
  table_ = JoinHashTable(mix_);
  build.open();
  Row r;
  while (build.next(r)) {
    if (null_key(r, bkeys)) continue;
    const std::uint64_t h = key_hash(r, bkeys);
    table_.add(h, std::move(r));
  }
  build.close();
  table_.finish();
  // Probe: stream the other input past the table, one row at a time.
  (build_left_ ? right_ : left_)->open();
  probing_ = false;
}

void HashJoin::close() { (build_left_ ? right_ : left_)->close(); }  // the table stays, for detail()

bool HashJoin::produce(Row& out) {
  Operator& probe = build_left_ ? *right_ : *left_;
  const std::vector<std::size_t>& pkeys = build_left_ ? rkeys_ : lkeys_;
  for (;;) {
    if (!probing_) {
      if (!probe.next(outer_)) return false;
      if (null_key(outer_, pkeys)) continue;
      probe_hash_ = key_hash(outer_, pkeys);
      std::tie(pos_, end_) = table_.bucket_range(probe_hash_);
      probing_ = true;
    }
    while (pos_ < end_) {
      const std::uint32_t id = table_.id_at(pos_++);
      if (table_.hash_of(id) != probe_hash_) continue;  // another key, same bucket
      const Row& in = table_.row(id);
      const Row& l = build_left_ ? in : outer_;
      const Row& r = build_left_ ? outer_ : in;
      if (!keys_equal(l, r)) continue;                  // another key, same hash
      bool match = true;
      for (const BoundComparison& c : residual_)
        if (!eval_pair(c, l, r, nleft_)) { match = false; break; }
      if (!match) continue;
      join_rows(l, r, out);
      return true;
    }
    probing_ = false;  // this probe row's bucket is used up
  }
}

// ---- the batch path (Unit 12) ----

// key_hash(), for row i of a batch (or of the built columns: a batch's
// columns and built_ are read the same way).
std::uint64_t HashJoin::batch_key_hash(const Batch& b, std::size_t i,
                                       const std::vector<std::size_t>& cols) const {
  std::uint64_t h = 0;
  for (std::size_t k = 0; k < cols.size(); ++k) {
    const ColumnVector& c = b.cols[cols[k]];
    const std::uint64_t v = key_text_[k] ? fnv1a(c.texts[i]) : static_cast<std::uint64_t>(c.ints[i]);
    h = (k == 0) ? v : (h * 0x9E3779B97F4A7C15ULL) ^ v;
  }
  return h;
}

// The build side, read in batches and filed as columns: built_[c] holds column
// c of every filed row, row id i being the i-th filed. The hashes and the
// buckets are Unit 9's JoinHashTable's, so a probe meets the same rows in the
// same order as on the row path.
void HashJoin::build_batches() {
  Operator& build = build_left_ ? *left_ : *right_;
  const std::vector<std::size_t>& bkeys = build_left_ ? lkeys_ : rkeys_;
  table_ = JoinHashTable(mix_);
  built_.assign(build.schema().size(), ColumnVector{});
  for (std::size_t c = 0; c < built_.size(); ++c) built_[c].type = build.schema()[c].column.type;
  build.open();
  Batch b;
  while (build.next_batch(b)) {
    for (std::size_t k = 0; k < b.count(); ++k) {
      const std::size_t i = b.row(k);
      bool null = false;
      for (std::size_t col : bkeys) null |= b.cols[col].nulls[i] != 0;
      if (null) continue;  // a NULL key matches nothing: not filed
      table_.add_key(batch_key_hash(b, i, bkeys));
      for (std::size_t c = 0; c < built_.size(); ++c) built_[c].push_from(b.cols[c], i);
    }
  }
  build.close();
  table_.finish();
}

bool HashJoin::produce_batch(Batch& out, std::size_t max) {
  Operator& probe = build_left_ ? *right_ : *left_;
  const std::vector<std::size_t>& pkeys = build_left_ ? rkeys_ : lkeys_;
  const std::vector<std::size_t>& bkeys = build_left_ ? lkeys_ : rkeys_;
  const std::size_t want = cap(max);
  if (match_probe_.size() < want) {
    match_probe_.resize(want);
    match_build_.resize(want);
  }
  for (;;) {
    if (pk_ >= probe_batch_.count()) {  // the next probe batch, every key hashed in one pass
      if (!probe.next_batch(probe_batch_)) return false;
      probe_hashes_.resize(probe_batch_.count());
      for (std::size_t k = 0; k < probe_batch_.count(); ++k)
        probe_hashes_[k] = batch_key_hash(probe_batch_, probe_batch_.row(k), pkeys);
      pk_ = 0;
      in_bucket_ = false;
    }
    // Collect matches, as pairs of positions, until the output is full or the
    // probe batch is used up; a bucket half walked is resumed on the next call.
    std::size_t m = 0;
    while (pk_ < probe_batch_.count() && m < want) {
      const std::size_t i = probe_batch_.row(pk_);
      if (!in_bucket_) {
        bool null = false;
        for (std::size_t col : pkeys) null |= probe_batch_.cols[col].nulls[i] != 0;
        if (null) {  // NULL equals nothing: no probe at all
          ++pk_;
          continue;
        }
        std::tie(pos_, end_) = table_.bucket_range(probe_hashes_[pk_]);
        in_bucket_ = true;
      }
      const std::uint64_t h = probe_hashes_[pk_];
      while (pos_ < end_ && m < want) {
        const std::uint32_t id = table_.id_at(pos_++);
        if (table_.hash_of(id) != h) continue;  // another key, same bucket
        bool equal = true;
        for (std::size_t k = 0; k < pkeys.size() && equal; ++k) {
          const ColumnVector& p = probe_batch_.cols[pkeys[k]];
          const ColumnVector& q = built_[bkeys[k]];
          equal = key_text_[k] ? p.texts[i] == q.texts[id] : p.ints[i] == q.ints[id];
        }
        if (!equal) continue;  // another key, same hash
        match_probe_[m] = static_cast<std::uint32_t>(i);
        match_build_[m] = id;
        ++m;
      }
      if (pos_ >= end_) {
        in_bucket_ = false;
        ++pk_;
      }
    }
    if (m == 0) continue;  // nothing matched in the rest of this probe batch: read the next
    // The output, column by column: the left input's columns, then the right's.
    out.format(types(), m);
    for (std::size_t c = 0; c < out.cols.size(); ++c) {
      const bool left = c < nleft_;
      const std::size_t src = left ? c : c - nleft_;
      if (left != build_left_) gather(out.cols[c], probe_batch_.cols[src], match_probe_.data(), m);
      else gather(out.cols[c], built_[src], match_build_.data(), m);
    }
    out.size = m;
    for (const BoundComparison& c : residual_) {
      select(c, out);
      if (out.count() == 0) break;
    }
    if (out.count() > 0) return true;  // else: every pair failed the rest of the ON; go on
  }
}

std::string HashJoin::describe() const { return "HashJoin ON " + text_; }

std::string HashJoin::detail() const {
  const JoinHashTable::Occupancy o = table_.occupancy();
  return std::string("hash table (") + (build_left_ ? "left" : "right") + " input): " +
         std::to_string(o.rows) + " rows in " + std::to_string(o.buckets) + " buckets, " +
         std::to_string(o.used) + " used, longest chain " + std::to_string(o.longest);
}

// ------------------------------------------- index nested-loop join (Unit 11)

IndexNestedLoopJoin::IndexNestedLoopJoin(OpPtr outer, Catalog& cat, const TableRef& inner,
                                         OutSchema inner_schema, std::size_t outer_col,
                                         std::size_t inner_col,
                                         std::vector<BoundComparison> inner_filter,
                                         std::vector<BoundComparison> residual,
                                         bool inner_first, std::string text, ChildRule rule)
    : outer_(std::move(outer)), cat_(cat), inner_(inner), outer_col_(outer_col),
      inner_col_(inner_col), inner_filter_(std::move(inner_filter)),
      residual_(std::move(residual)), inner_first_(inner_first), text_(std::move(text)),
      rule_(rule) {
  inputs_ = {outer_.get()};
  for (const OutColumn& c : inner_schema) inner_types_.push_back(c.column.type);
  index_name_ = inner.name + "(" + inner_schema[inner_col].column.name + ")";
  text_key_ = inner_schema[inner_col].column.type == ColType::kText;
  schema_ = inner_first_ ? concat(inner_schema, outer_->schema())
                         : concat(outer_->schema(), inner_schema);
}

void IndexNestedLoopJoin::open() {
  cur_.emplace(cat_.open_index(inner_.name, inner_col_, rule_));
  read_.reset();
  outer_batch_.size = 0;
  outer_batch_.selected = false;
  ok_ = 0;
  outer_->open();
  have_outer_ = false;
  searches_ = fetched_ = 0;
}

void IndexNestedLoopJoin::close() {
  outer_->close();
  cur_.reset();
}

bool IndexNestedLoopJoin::produce(Row& out) {
  for (;;) {
    if (!have_outer_) {
      if (!pull(*outer_, read_, outer_row_)) return false;
      const Field& k = outer_row_[outer_col_];
      if (k.is_null) continue;  // NULL equals nothing: no search at all
      const Key key = order_key(k);
      cur_->seek(key, key);
      ++searches_;
      have_outer_ = true;
    }
    while (cur_->next(inner_row_)) {
      ++fetched_;
      if (text_key_ && inner_row_[inner_col_].text != outer_row_[outer_col_].text) continue;
      bool pass = true;
      for (const BoundComparison& c : inner_filter_)
        if (!eval(c, inner_row_)) { pass = false; break; }
      if (!pass) continue;
      if (inner_first_) join_rows(inner_row_, outer_row_, out);
      else join_rows(outer_row_, inner_row_, out);
      for (const BoundComparison& c : residual_)
        if (!eval(c, out)) { pass = false; break; }
      if (pass) return true;
    }
    have_outer_ = false;  // this outer row's matches are used up
  }
}

bool IndexNestedLoopJoin::produce_batch(Batch& out, std::size_t max) {
  const std::size_t want = cap(max);
  if (match_outer_.size() < want) {
    match_outer_.resize(want);
    oidx_.resize(want);
    iidx_.resize(want);
  }
  for (;;) {
    if (ok_ >= outer_batch_.count()) {
      if (!outer_->next_batch(outer_batch_)) return false;
      ok_ = 0;
      have_outer_ = false;
    }
    // Search the index for each outer row in turn, the matches decoded into
    // inner_batch_ one after another, until it is full or the outer batch ends.
    inner_batch_.format(inner_types_, 0);  // the cursor makes room as it fills
    std::size_t m = 0;
    while (ok_ < outer_batch_.count() && m < want) {
      const std::size_t i = outer_batch_.row(ok_);
      if (!have_outer_) {
        const ColumnVector& k = outer_batch_.cols[outer_col_];
        if (k.nulls[i]) {  // NULL equals nothing: no search at all
          ++ok_;
          continue;
        }
        const Key key = order_key_at(k, i);
        cur_->seek(key, key);
        ++searches_;
        have_outer_ = true;
      }
      const std::size_t got = cur_->fill(inner_batch_, m, want - m);
      fetched_ += got;
      for (std::size_t j = m; j < m + got; ++j) match_outer_[j] = static_cast<std::uint32_t>(i);
      const bool used_up = got < want - m;  // fewer than there was room for: no more matches
      m += got;
      if (used_up) {
        have_outer_ = false;
        ++ok_;
      }
    }
    if (m == 0) continue;
    inner_batch_.size = m;
    if (text_key_) {  // a TEXT key is a prefix: the strings themselves must match
      const ColumnVector& in = inner_batch_.cols[inner_col_];
      const ColumnVector& key = outer_batch_.cols[outer_col_];
      keep_if(inner_batch_, [&](std::size_t j) { return in.texts[j] == key.texts[match_outer_[j]]; });
    }
    for (const BoundComparison& c : inner_filter_) {
      select(c, inner_batch_);
      if (inner_batch_.count() == 0) break;
    }
    const std::size_t n = inner_batch_.count();
    if (n == 0) continue;
    for (std::size_t k = 0; k < n; ++k) {
      iidx_[k] = static_cast<std::uint32_t>(inner_batch_.row(k));
      oidx_[k] = match_outer_[iidx_[k]];
    }
    out.format(types(), n);
    const std::size_t no = outer_->schema().size(), ni = inner_types_.size();
    for (std::size_t c = 0; c < no; ++c)
      gather(out.cols[(inner_first_ ? ni : 0) + c], outer_batch_.cols[c], oidx_.data(), n);
    for (std::size_t c = 0; c < ni; ++c)
      gather(out.cols[(inner_first_ ? 0 : no) + c], inner_batch_.cols[c], iidx_.data(), n);
    out.size = n;
    for (const BoundComparison& c : residual_) {
      select(c, out);
      if (out.count() == 0) break;
    }
    if (out.count() > 0) return true;
  }
}

std::string IndexNestedLoopJoin::describe() const {
  return "IndexNestedLoopJoin ON " + text_ + " (index " + index_name_ + ")";
}

std::string IndexNestedLoopJoin::detail() const {
  return "index searches: " + std::to_string(searches_) + "; rows fetched: " +
         std::to_string(fetched_);
}

// ------------------------------------------------------ aggregation (Unit 10)

void agg_update(const AggSpec& a, AggState& s, const Field& v) {
  if (a.star) {  // COUNT(*): every row counts, whatever it holds
    ++s.count;
    return;
  }
  if (v.is_null) return;  // every other aggregate skips NULL
  ++s.count;
  switch (a.func) {
    case AggFunc::kSum:
    case AggFunc::kAvg:
      if ((v.i64 > 0 && s.sum > INT64_MAX - v.i64) || (v.i64 < 0 && s.sum < INT64_MIN - v.i64))
        throw ExecError{"integer overflow in " + std::string(agg_name(a.func))};
      s.sum += v.i64;
      break;
    case AggFunc::kMin: if (s.count == 1 || compare_values(v, s.best) < 0) s.best = v; break;
    case AggFunc::kMax: if (s.count == 1 || compare_values(v, s.best) > 0) s.best = v; break;
    case AggFunc::kCount:
    case AggFunc::kNone: break;
  }
}

IndexMinMax::IndexMinMax(Catalog& cat, const TableRef& table, std::vector<AggSpec> aggs,
                         std::string text)
    : cat_(cat), table_(table), aggs_(std::move(aggs)), text_(std::move(text)) {
  for (std::size_t a = 0; a < aggs_.size(); ++a)
    schema_.push_back({"", {"#" + std::to_string(a), agg_type(aggs_[a]), true}});
}

void IndexMinMax::open() {
  values_.clear();
  done_ = false;
  fetched_ = 0;
  for (const AggSpec& a : aggs_) {
    IndexCursor cur = cat_.open_index(table_.name, a.col);
    Field best = Field::Null(a.type);  // an empty index: no value, so NULL
    std::optional<Key> end;  // the key at the end the aggregate wants
    if (a.func == AggFunc::kMin) {
      end = 0;  // the first entry is the first one at or after key 0
    } else {
      end = cur.max_key();
    }
    if (end) {
      cur.seek(*end, ~Key{0});
      Row r;
      bool first = true;
      while (cur.next(r)) {
        if (!first && cur.key() != *end) break;  // past the entries with the end key
        if (first) end = cur.key();              // MIN: the smallest key there is
        ++fetched_;
        const Field& v = r[a.col];
        if (first || compare_values(v, best) * (a.func == AggFunc::kMin ? 1 : -1) < 0) best = v;
        first = false;
        if (order_key_exact(a.type)) break;  // an exact key is the value: one row is enough
        // A string shorter than 8 bytes is all inside its key, and any other
        // string with that key begins with it -- so none is smaller.
        if (a.func == AggFunc::kMin && best.text.size() < 8) break;
      }
    }
    values_.push_back(best);
  }
}

bool IndexMinMax::produce(Row& out) {
  if (done_) return false;
  done_ = true;
  out = values_;
  return true;
}

std::string IndexMinMax::detail() const {
  return "rows fetched: " + std::to_string(fetched_) + " (from the ends of the indexes)";
}

Field agg_result(const AggSpec& a, const AggState& s) {
  switch (a.func) {
    case AggFunc::kCount: return Field::Int64(s.count);
    case AggFunc::kSum: return s.count ? Field::Int64(s.sum) : Field::Null(ColType::kInt64);
    case AggFunc::kAvg:
      return s.count ? Field::Double(static_cast<double>(s.sum) / static_cast<double>(s.count))
                     : Field::Null(ColType::kDouble);
    case AggFunc::kMin:
    case AggFunc::kMax: return s.count ? s.best : Field::Null(a.type);
    case AggFunc::kNone: break;
  }
  return Field::Null(ColType::kInt64);
}

ColType agg_type(const AggSpec& a) {
  switch (a.func) {
    case AggFunc::kAvg: return ColType::kDouble;
    case AggFunc::kMin:
    case AggFunc::kMax: return a.type;
    default: return ColType::kInt64;  // COUNT, SUM
  }
}

HashAggregate::HashAggregate(OpPtr child, std::vector<std::size_t> group_cols,
                             std::vector<AggSpec> aggs, std::string text, UpdateFn update)
    : child_(std::move(child)), group_cols_(std::move(group_cols)),
      aggs_(std::move(aggs)), text_(std::move(text)), update_(update) {
  inputs_ = {child_.get()};
  for (std::size_t c : group_cols_) schema_.push_back(child_->schema()[c]);
  for (std::size_t a = 0; a < aggs_.size(); ++a)
    schema_.push_back({"", {"#" + std::to_string(a), agg_type(aggs_[a]), true}});
}

// A group key's raw hash, made the way HashJoin makes a join key's -- an
// integer is its own raw hash, text is FNV-1a -- with NULL as one fixed value,
// since every NULL key belongs to the same group (and -0.0 as 0.0, which it
// equals). Its slot is mix64 of this: Unit 9's lesson, applied from the start.
std::uint64_t HashAggregate::key_hash(const Row& row) const {
  std::uint64_t h = 0;
  for (std::size_t k = 0; k < group_cols_.size(); ++k) {
    const Field& f = row[group_cols_[k]];
    const std::uint64_t v = f.is_null                    ? 0x9E3779B97F4A7C15ULL
                            : f.type == ColType::kText   ? fnv1a(f.text)
                            : f.type == ColType::kDouble ? std::bit_cast<std::uint64_t>(f.f64 == 0 ? 0.0 : f.f64)
                                                         : static_cast<std::uint64_t>(f.i64);
    h = (k == 0) ? v : (h * 0x9E3779B97F4A7C15ULL) ^ v;
  }
  return h;
}

bool HashAggregate::same_key(const Row& row, std::uint32_t g) const {
  const Row& key = keys_[g];
  for (std::size_t k = 0; k < group_cols_.size(); ++k) {
    const Field& a = row[group_cols_[k]];
    const Field& b = key[k];
    if (a.is_null || b.is_null) {
      if (a.is_null != b.is_null) return false;
      continue;  // NULL and NULL: the same group
    }
    if (compare_values(a, b) != 0) return false;
  }
  return true;
}

// The group this row belongs to, created if it is the first of its key.
// Linear probing over a power-of-two table kept at most half full.
std::uint32_t HashAggregate::find_or_add(const Row& row) {
  const std::uint64_t h = key_hash(row);
  std::size_t i = mix64(h) & mask_;
  std::size_t probes = 1;
  for (;; i = (i + 1) & mask_, ++probes) {
    const std::uint32_t s = slots_[i];
    if (s == 0) break;  // an empty slot: this key is new
    const std::uint32_t g = s - 1;
    if (hashes_[g] == h && same_key(row, g)) {
      if (probes > longest_probe_) longest_probe_ = probes;
      return g;
    }
  }
  if (probes > longest_probe_) longest_probe_ = probes;
  const auto g = static_cast<std::uint32_t>(keys_.size());
  Row key;
  key.reserve(group_cols_.size());
  for (std::size_t c : group_cols_) key.push_back(row[c]);
  keys_.push_back(std::move(key));
  hashes_.push_back(h);
  states_.resize(states_.size() + aggs_.size());
  slots_[i] = g + 1;
  if (keys_.size() * 2 > slots_.size()) grow();
  return g;
}

void HashAggregate::grow() {
  slots_.assign(slots_.size() * 2, 0);
  mask_ = slots_.size() - 1;
  for (std::uint32_t g = 0; g < hashes_.size(); ++g) {
    std::size_t i = mix64(hashes_[g]) & mask_;
    while (slots_[i] != 0) i = (i + 1) & mask_;
    slots_[i] = g + 1;
  }
}

void HashAggregate::open() {
  if (batch_rows_) {  // Unit 12
    open_batches();
    return;
  }
  keys_.clear();
  hashes_.clear();
  states_.clear();
  slots_.assign(16, 0);
  mask_ = slots_.size() - 1;
  longest_probe_ = 0;
  next_ = 0;
  if (group_cols_.empty()) {  // no GROUP BY: one group, even if no row arrives
    keys_.emplace_back();
    hashes_.push_back(0);
    states_.resize(aggs_.size());
  }
  child_->open();
  Row r;
  while (child_->next(r)) {
    const std::uint32_t g = group_cols_.empty() ? 0 : find_or_add(r);
    AggState* st = states_.data() + static_cast<std::size_t>(g) * aggs_.size();
    for (std::size_t a = 0; a < aggs_.size(); ++a) update_(aggs_[a], st[a], r[aggs_[a].col]);
  }
  child_->close();
}

// ---- the batch path (Unit 12) ----

// The group of row i of a batch whose key hashes to h, created if new: the
// row path's find_or_add, reading the key from the batch's columns and the
// groups' keys from gkeys_. The same hash, the same probe sequence, the same
// table -- and so the same groups, in the same order.
std::uint32_t HashAggregate::find_or_add(const Batch& b, std::size_t i, std::uint64_t h) {
  std::size_t s = mix64(h) & mask_;
  std::size_t probes = 1;
  for (;; s = (s + 1) & mask_, ++probes) {
    const std::uint32_t slot = slots_[s];
    if (slot == 0) break;
    const std::uint32_t g = slot - 1;
    if (hashes_[g] != h) continue;
    const bool same = int_key_ ? b.cols[group_cols_[0]].nulls[i] == gkeys_[0].nulls[g] &&
                                     b.cols[group_cols_[0]].ints[i] == gkeys_[0].ints[g]
                               : same_key(b, i, g);
    if (same) {
      if (probes > longest_probe_) longest_probe_ = probes;
      return g;
    }
  }
  if (probes > longest_probe_) longest_probe_ = probes;
  const auto g = static_cast<std::uint32_t>(ngroups_++);
  for (std::size_t k = 0; k < group_cols_.size(); ++k) gkeys_[k].push_from(b.cols[group_cols_[k]], i);
  hashes_.push_back(h);
  for (std::size_t a = 0; a < aggs_.size(); ++a) {
    counts_[a].push_back(0);
    if (aggs_[a].func == AggFunc::kSum || aggs_[a].func == AggFunc::kAvg) sums_[a].push_back(0);
    if (aggs_[a].func == AggFunc::kMin || aggs_[a].func == AggFunc::kMax)
      best_[a].push(Field::Null(aggs_[a].type));
  }
  slots_[s] = g + 1;
  if (ngroups_ * 2 > slots_.size()) grow();
  return g;
}

bool HashAggregate::same_key(const Batch& b, std::size_t i, std::uint32_t g) const {
  for (std::size_t k = 0; k < group_cols_.size(); ++k) {
    const ColumnVector& a = b.cols[group_cols_[k]];
    const ColumnVector& key = gkeys_[k];
    if (a.nulls[i] || key.nulls[g]) {
      if (a.nulls[i] != key.nulls[g]) return false;
      continue;  // NULL and NULL: the same group
    }
    if (compare_at(a, i, key, g) != 0) return false;
  }
  return true;
}

// One aggregate folded over a whole batch, in a loop of its own: gids_[k] is
// the group of the batch's k-th live row. agg_update's rules, over columns.
void HashAggregate::fold(const AggSpec& a, std::size_t ai, const Batch& b) {
  const std::size_t n = b.count();
  const std::uint32_t* gid = gids_.data();
  std::int64_t* cnt = counts_[ai].data();
  if (a.star) {  // COUNT(*): every row counts, whatever it holds
    for (std::size_t k = 0; k < n; ++k) ++cnt[gid[k]];
    return;
  }
  const ColumnVector& v = b.cols[a.col];
  const std::uint8_t* null = v.nulls.data();
  switch (a.func) {
    case AggFunc::kCount:
      for_live(b, [&](std::size_t k, std::size_t i) { cnt[gid[k]] += null[i] ? 0 : 1; });
      break;
    case AggFunc::kSum:
    case AggFunc::kAvg: {
      std::int64_t* sum = sums_[ai].data();
      const std::int64_t* x = v.ints.data();
      bool overflow = false;
      for_live(b, [&](std::size_t k, std::size_t i) {
        if (null[i]) return;  // every aggregate but COUNT(*) skips NULL
        const std::int64_t val = x[i];
        std::int64_t& acc = sum[gid[k]];
        overflow |= (val > 0 && acc > INT64_MAX - val) || (val < 0 && acc < INT64_MIN - val);
        acc += overflow ? 0 : val;
        ++cnt[gid[k]];
      });
      if (overflow) throw ExecError{"integer overflow in " + std::string(agg_name(a.func))};
      break;
    }
    case AggFunc::kMin:
    case AggFunc::kMax: {
      ColumnVector& best = best_[ai];
      const int want = a.func == AggFunc::kMin ? -1 : 1;
      for_live(b, [&](std::size_t k, std::size_t i) {
        if (null[i]) return;
        const std::uint32_t g = gid[k];
        if (++cnt[g] == 1 || compare_at(v, i, best, g) == want) best.copy_from(g, v, i);
      });
      break;
    }
    case AggFunc::kNone:
      break;
  }
}

void HashAggregate::open_batches() {
  keys_.clear();
  states_.clear();
  hashes_.clear();
  slots_.assign(16, 0);
  mask_ = slots_.size() - 1;
  longest_probe_ = 0;
  next_ = 0;
  ngroups_ = 0;
  const OutSchema& in = child_->schema();
  gkeys_.assign(group_cols_.size(), ColumnVector{});
  for (std::size_t k = 0; k < group_cols_.size(); ++k) gkeys_[k].type = in[group_cols_[k]].column.type;
  counts_.assign(aggs_.size(), {});
  sums_.assign(aggs_.size(), {});
  best_.assign(aggs_.size(), ColumnVector{});
  for (std::size_t a = 0; a < aggs_.size(); ++a) best_[a].type = aggs_[a].type;
  if (group_cols_.empty()) {  // no GROUP BY: one group, even if no row arrives
    ngroups_ = 1;
    hashes_.push_back(0);
    for (std::size_t a = 0; a < aggs_.size(); ++a) {
      counts_[a].push_back(0);
      sums_[a].push_back(0);
      best_[a].push(Field::Null(aggs_[a].type));
    }
  }
  child_->open();
  Batch b;
  int_key_ = group_cols_.size() == 1 && in[group_cols_[0]].column.type != ColType::kText &&
             in[group_cols_[0]].column.type != ColType::kDouble;
  while (child_->next_batch(b)) {
    const std::size_t n = b.count();
    gids_.resize(n);
    if (group_cols_.empty()) {
      std::fill(gids_.begin(), gids_.end(), 0);
    } else {
      // key_hash() of every live row, a key column at a time: each column's
      // type is looked at once for the batch.
      std::uint64_t* h = batch_hashes_.data();
      if (batch_hashes_.size() < n) batch_hashes_.resize(n), h = batch_hashes_.data();
      for (std::size_t c = 0; c < group_cols_.size(); ++c) {
        const ColumnVector& col = b.cols[group_cols_[c]];
        const std::uint8_t* null = col.nulls.data();
        const auto fold_in = [&](auto value_of) {
          for_live(b, [&](std::size_t k, std::size_t i) {
            const std::uint64_t v = null[i] ? 0x9E3779B97F4A7C15ULL : value_of(i);
            h[k] = (c == 0) ? v : (h[k] * 0x9E3779B97F4A7C15ULL) ^ v;
          });
        };
        if (col.type == ColType::kText) {
          fold_in([&](std::size_t i) { return fnv1a(col.texts[i]); });
        } else if (col.type == ColType::kDouble) {
          fold_in([&](std::size_t i) {
            return std::bit_cast<std::uint64_t>(col.doubles[i] == 0 ? 0.0 : col.doubles[i]);
          });
        } else {
          const std::int64_t* x = col.ints.data();
          fold_in([&](std::size_t i) { return static_cast<std::uint64_t>(x[i]); });
        }
      }
      if (int_key_) {
        // The common case inlined: one integer key, found in the first slot
        // its hash points to. Anything else -- a new group, a collision, a
        // NULL -- goes to find_or_add.
        const ColumnVector& key = b.cols[group_cols_[0]];
        for_live(b, [&](std::size_t k, std::size_t i) {
          const std::uint32_t slot = slots_[mix64(h[k]) & mask_];
          if (slot != 0) {
            const std::uint32_t g = slot - 1;
            if (hashes_[g] == h[k] && key.nulls[i] == 0 && gkeys_[0].nulls[g] == 0 &&
                key.ints[i] == gkeys_[0].ints[g]) {
              gids_[k] = g;
              return;
            }
          }
          gids_[k] = find_or_add(b, i, h[k]);
        });
      } else {
        for_live(b, [&](std::size_t k, std::size_t i) { gids_[k] = find_or_add(b, i, h[k]); });
      }
    }
    for (std::size_t a = 0; a < aggs_.size(); ++a) fold(aggs_[a], a, b);
  }
  child_->close();
}

bool HashAggregate::produce_batch(Batch& out, std::size_t max) {
  if (next_ >= ngroups_) return false;
  const std::size_t n = std::min(cap(max), ngroups_ - next_);
  out.format(types(), n);
  const std::size_t nk = group_cols_.size();
  for (std::size_t j = 0; j < n; ++j) {
    const std::size_t g = next_ + j;
    for (std::size_t k = 0; k < nk; ++k) out.cols[k].copy_from(j, gkeys_[k], g);
    for (std::size_t a = 0; a < aggs_.size(); ++a) {
      ColumnVector& dst = out.cols[nk + a];
      const std::int64_t count = counts_[a][g];
      switch (aggs_[a].func) {
        case AggFunc::kCount:
          dst.nulls[j] = 0;
          dst.ints[j] = count;
          break;
        case AggFunc::kSum:
          dst.nulls[j] = count ? 0 : 1;
          dst.ints[j] = count ? sums_[a][g] : 0;
          break;
        case AggFunc::kAvg:
          dst.nulls[j] = count ? 0 : 1;
          dst.doubles[j] = count ? static_cast<double>(sums_[a][g]) / static_cast<double>(count) : 0.0;
          break;
        case AggFunc::kMin:
        case AggFunc::kMax:
          dst.copy_from(j, best_[a], g);  // NULL until a value arrived
          break;
        case AggFunc::kNone:
          dst.nulls[j] = 1;
          break;
      }
    }
  }
  out.size = n;
  next_ += n;
  return true;
}

bool HashAggregate::produce(Row& out) {
  if (next_ >= keys_.size()) return false;
  const std::size_t g = next_++;
  out = keys_[g];
  const AggState* st = states_.data() + g * aggs_.size();
  for (std::size_t a = 0; a < aggs_.size(); ++a) out.push_back(agg_result(aggs_[a], st[a]));
  return true;
}

std::size_t HashAggregate::bytes() const {
  std::size_t b = slots_.capacity() * sizeof(std::uint32_t) + hashes_.capacity() * sizeof(std::uint64_t);
  if (!batch_rows_)
    return b + keys_.capacity() * sizeof(Row) + keys_.size() * group_cols_.size() * sizeof(Field) +
           states_.capacity() * sizeof(AggState);
  for (const ColumnVector& k : gkeys_) b += k.bytes();
  for (std::size_t a = 0; a < aggs_.size(); ++a)
    b += (counts_[a].capacity() + sums_[a].capacity()) * sizeof(std::int64_t) + best_[a].bytes();
  return b;
}

std::string HashAggregate::detail() const {
  if (group_cols_.empty()) return "one group (no GROUP BY)";
  return std::to_string(groups()) + " groups in a table of " +
         std::to_string(slots_.size()) + " slots; longest probe " +
         std::to_string(longest_probe_);
}

// ------------------------------------------------- ORDER BY and LIMIT (Unit 10)

Sort::Sort(OpPtr child, std::vector<SortKey> keys, std::optional<std::size_t> limit,
           std::string text, bool prefixes)
    : child_(std::move(child)), keys_(std::move(keys)), limit_(limit),
      text_(std::move(text)), prefixes_(prefixes) {
  schema_ = child_->schema();
  inputs_ = {child_.get()};
}

// The first sort key as 64 bits that order the way the rows must: the value's
// order_key (an integer with its sign bit flipped, a double by the usual bit
// trick, text by its first 8 bytes), NULL as the largest code; all inverted
// for DESC. The map never reverses an order, only merges values: equal
// prefixes may still differ (two strings with 8 bytes in common, a value that
// lands on NULL's code), and before() settles those.
std::uint64_t Sort::prefix_of(const Row& r) const {
  if (!prefixes_) return 0;
  const Field& f = r[keys_[0].col];
  const std::uint64_t p = f.is_null ? ~0ULL : order_key(f);  // NULL: after every value
  return keys_[0].desc ? ~p : p;
}

// Is rows_[a] ordered before rows_[b]? Key by key; a NULL is greater than
// every value (last ascending, first descending); a full tie goes to the row
// that arrived first.
bool Sort::before(std::uint32_t a, std::uint32_t b) const {
  const Row& x = rows_[a];
  const Row& y = rows_[b];
  for (const SortKey& k : keys_) {
    const Field& p = x[k.col];
    const Field& q = y[k.col];
    if (p.is_null || q.is_null) {
      if (p.is_null && q.is_null) continue;
      return k.desc ? p.is_null : q.is_null;
    }
    const int c = compare_values(p, q);
    if (c != 0) return k.desc ? c > 0 : c < 0;
  }
  return seq_[a] < seq_[b];
}

void Sort::open() {
  if (batch_rows_) {  // Unit 12
    open_batches();
    return;
  }
  rows_.clear();
  seq_.clear();
  order_.clear();
  next_ = 0;
  read_ = 0;
  replaced_ = 0;
  const auto cmp = [this](const Entry& a, const Entry& b) { return entry_before(a, b); };
  child_->open();
  Row r;
  if (!limit_) {  // a full sort: hold every row, then order the entries
    while (child_->next(r)) {
      order_.push_back({prefix_of(r), static_cast<std::uint32_t>(rows_.size())});
      rows_.push_back(std::move(r));
      seq_.push_back(read_++);
    }
    std::sort(order_.begin(), order_.end(), cmp);
  } else {  // top n: a max-heap of the best n so far; its top is the worst kept
    const std::size_t n = *limit_;
    while (child_->next(r)) {
      const std::uint64_t s = read_++;
      if (n == 0) continue;
      if (order_.size() < n) {
        order_.push_back({prefix_of(r), static_cast<std::uint32_t>(rows_.size())});
        rows_.push_back(std::move(r));
        seq_.push_back(s);
        std::push_heap(order_.begin(), order_.end(), cmp);
        if (order_.size() == n) {  // full: one more row, the scratch slot rows_[n]
          rows_.emplace_back();
          seq_.push_back(0);
        }
        continue;
      }
      const Entry cand{prefix_of(r), static_cast<std::uint32_t>(n)};
      std::swap(rows_[n], r);  // r gets the scratch row's buffer to refill
      seq_[n] = s;
      if (cmp(cand, order_.front())) {  // better than the worst row kept
        std::pop_heap(order_.begin(), order_.end(), cmp);
        const std::uint32_t w = order_.back().row;
        std::swap(rows_[w], rows_[n]);
        seq_[w] = s;
        order_.back() = {cand.prefix, w};
        std::push_heap(order_.begin(), order_.end(), cmp);
        ++replaced_;
      }
    }
    std::sort(order_.begin(), order_.end(), cmp);
  }
  held_ = order_.size();
  child_->close();
}

bool Sort::produce(Row& out) {
  if (next_ >= order_.size()) return false;
  out = std::move(rows_[order_[next_++].row]);
  return true;
}

// ---- the batch path (Unit 12) ----
// The same sort over rows held as columns: row r of the held input is row r of
// every cols_[c]. The entries, their prefixes and the order are the row path's,
// so the output is too, ties and all.

std::uint64_t Sort::prefix_of(const std::vector<ColumnVector>& c, std::size_t i) const {
  if (!prefixes_) return 0;
  const ColumnVector& k = c[keys_[0].col];
  const std::uint64_t p = k.nulls[i] ? ~0ULL : order_key_at(k, i);  // NULL: after every value
  return keys_[0].desc ? ~p : p;
}

bool Sort::before_cols(std::uint32_t a, std::uint32_t b) const {
  for (const SortKey& k : keys_) {
    const ColumnVector& c = cols_[k.col];
    const bool pn = c.nulls[a] != 0, qn = c.nulls[b] != 0;
    if (pn || qn) {
      if (pn && qn) continue;
      return k.desc ? pn : qn;
    }
    const int r = compare_at(c, a, c, b);
    if (r != 0) return k.desc ? r > 0 : r < 0;
  }
  return seq_[a] < seq_[b];
}

void Sort::open_batches() {
  rows_.clear();
  seq_.clear();
  order_.clear();
  next_ = 0;
  read_ = 0;
  replaced_ = 0;
  cols_.assign(schema_.size(), ColumnVector{});
  for (std::size_t c = 0; c < cols_.size(); ++c) cols_[c].type = schema_[c].column.type;
  const auto cmp = [this](const Entry& a, const Entry& b) { return entry_before_cols(a, b); };
  const auto hold = [&](const Batch& b, std::size_t i) {  // append b's row i; its index
    for (std::size_t c = 0; c < cols_.size(); ++c) cols_[c].push_from(b.cols[c], i);
    return static_cast<std::uint32_t>(seq_.size());
  };
  child_->open();
  Batch b;
  if (!limit_) {  // a full sort: hold every row, then order the entries
    while (child_->next_batch(b))
      for (std::size_t k = 0; k < b.count(); ++k) {
        const std::uint32_t r = hold(b, b.row(k));
        seq_.push_back(read_++);
        order_.push_back({prefix_of(cols_, r), r});
      }
    std::sort(order_.begin(), order_.end(), cmp);
  } else {  // top n: the row path's bounded heap, its scratch row at index n
    const std::size_t n = *limit_;
    while (child_->next_batch(b))
      for (std::size_t k = 0; k < b.count(); ++k) {
        const std::size_t i = b.row(k);
        const std::uint64_t s = read_++;
        if (n == 0) continue;
        if (order_.size() < n) {
          const std::uint32_t r = hold(b, i);
          seq_.push_back(s);
          order_.push_back({prefix_of(cols_, r), r});
          std::push_heap(order_.begin(), order_.end(), cmp);
          if (order_.size() == n) {  // full: one more row, the scratch row
            hold(b, i);
            seq_.push_back(0);
          }
          continue;
        }
        // A row whose first key's prefix comes after the worst row kept cannot
        // enter, and is turned away before anything of it is copied.
        const std::uint64_t p = prefix_of(b.cols, i);
        if (p > order_.front().prefix) continue;
        for (std::size_t c = 0; c < cols_.size(); ++c) cols_[c].copy_from(n, b.cols[c], i);
        seq_[n] = s;
        const Entry cand{p, static_cast<std::uint32_t>(n)};
        if (cmp(cand, order_.front())) {  // better than the worst row kept
          std::pop_heap(order_.begin(), order_.end(), cmp);
          const std::uint32_t w = order_.back().row;
          for (std::size_t c = 0; c < cols_.size(); ++c) cols_[c].copy_from(w, cols_[c], n);
          seq_[w] = s;
          order_.back() = {cand.prefix, w};
          std::push_heap(order_.begin(), order_.end(), cmp);
          ++replaced_;
        }
      }
    std::sort(order_.begin(), order_.end(), cmp);
  }
  held_ = order_.size();
  child_->close();
}

bool Sort::produce_batch(Batch& out, std::size_t max) {
  if (next_ >= order_.size()) return false;
  const std::size_t n = std::min(cap(max), order_.size() - next_);
  out.format(types(), n);
  std::vector<std::uint32_t>& idx = seq_index_;
  idx.resize(n);
  for (std::size_t j = 0; j < n; ++j) idx[j] = order_[next_ + j].row;
  for (std::size_t c = 0; c < cols_.size(); ++c) gather(out.cols[c], cols_[c], idx.data(), n);
  out.size = n;
  next_ += n;
  return true;
}

std::string Sort::describe() const {
  return "Sort " + text_ + (limit_ ? " (top " + std::to_string(*limit_) + ")" : "");
}

std::string Sort::detail() const {
  std::string d = "rows held: " + std::to_string(held_) + " of " + std::to_string(read_) + " read";
  if (limit_) d += ", " + std::to_string(replaced_) + " replacements";
  return d;
}

Limit::Limit(OpPtr child, std::uint64_t n) : child_(std::move(child)), n_(n) {
  schema_ = child_->schema();
  inputs_ = {child_.get()};
}

}  // namespace labdb
