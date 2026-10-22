#include "operator.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
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

// ------------------------------------------------------------- one input

Filter::Filter(OpPtr child, std::vector<BoundComparison> preds, std::string text)
    : child_(std::move(child)), preds_(std::move(preds)), text_(std::move(text)) {
  schema_ = child_->schema();
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

Project::Project(OpPtr child, std::vector<std::size_t> cols, std::string text)
    : child_(std::move(child)), cols_(std::move(cols)), text_(std::move(text)) {
  for (std::size_t c : cols_) schema_.push_back(child_->schema()[c]);
}

bool Project::produce(Row& out) {
  if (!child_->next(in_)) return false;
  out.resize(cols_.size());
  for (std::size_t i = 0; i < cols_.size(); ++i) out[i] = in_[cols_[i]];
  return true;
}

// ------------------------------------------------------------ two inputs

NestedLoopJoin::NestedLoopJoin(OpPtr left, OpPtr right,
                               std::vector<BoundComparison> on, std::string text)
    : left_(std::move(left)), right_(std::move(right)), on_(std::move(on)),
      text_(std::move(text)), nleft_(left_->schema().size()) {
  schema_ = concat(left_->schema(), right_->schema());
}

void NestedLoopJoin::open() {
  inner_.clear();
  right_->open();  // read the right input once, and keep it
  Row r;
  while (right_->next(r)) inner_.push_back(std::move(r));
  right_->close();
  left_->open();
  have_outer_ = false;
}

void NestedLoopJoin::close() { left_->close(); }

bool NestedLoopJoin::produce(Row& out) {
  for (;;) {
    if (!have_outer_) {
      if (!left_->next(outer_)) return false;
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
  index_name_ = inner.name + "(" + inner_schema[inner_col].column.name + ")";
  text_key_ = inner_schema[inner_col].column.type == ColType::kText;
  schema_ = inner_first_ ? concat(inner_schema, outer_->schema())
                         : concat(outer_->schema(), inner_schema);
}

void IndexNestedLoopJoin::open() {
  cur_.emplace(cat_.open_index(inner_.name, inner_col_, rule_));
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
      if (!outer_->next(outer_row_)) return false;
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
  for (std::uint32_t g = 0; g < keys_.size(); ++g) {
    std::size_t i = mix64(hashes_[g]) & mask_;
    while (slots_[i] != 0) i = (i + 1) & mask_;
    slots_[i] = g + 1;
  }
}

void HashAggregate::open() {
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

bool HashAggregate::produce(Row& out) {
  if (next_ >= keys_.size()) return false;
  const std::size_t g = next_++;
  out = keys_[g];
  const AggState* st = states_.data() + g * aggs_.size();
  for (std::size_t a = 0; a < aggs_.size(); ++a) out.push_back(agg_result(aggs_[a], st[a]));
  return true;
}

std::size_t HashAggregate::bytes() const {
  return slots_.capacity() * sizeof(std::uint32_t) + keys_.capacity() * sizeof(Row) +
         keys_.size() * group_cols_.size() * sizeof(Field) +
         hashes_.capacity() * sizeof(std::uint64_t) + states_.capacity() * sizeof(AggState);
}

std::string HashAggregate::detail() const {
  if (group_cols_.empty()) return "one group (no GROUP BY)";
  return std::to_string(keys_.size()) + " groups in a table of " +
         std::to_string(slots_.size()) + " slots; longest probe " +
         std::to_string(longest_probe_);
}

// ------------------------------------------------- ORDER BY and LIMIT (Unit 10)

Sort::Sort(OpPtr child, std::vector<SortKey> keys, std::optional<std::size_t> limit,
           std::string text, bool prefixes)
    : child_(std::move(child)), keys_(std::move(keys)), limit_(limit),
      text_(std::move(text)), prefixes_(prefixes) {
  schema_ = child_->schema();
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
}

}  // namespace labdb
