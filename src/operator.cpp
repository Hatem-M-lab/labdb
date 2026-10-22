#include "operator.hpp"

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

// -1, 0, or 1, comparing within one type: strings by bytes, all else by value.
inline int compare_fields(const Field& a, const Field& b, bool as_text) {
  if (as_text) {
    const int c = a.text.compare(b.text);
    return (c > 0) - (c < 0);
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

bool eval(const BoundComparison& c, const Row& row) {
  const Field& a = row[c.left];
  const Field& b = c.right_is_column ? row[c.right] : c.literal;
  if (a.is_null || b.is_null) return false;
  return holds(c.op, compare_fields(a, b, c.as_text));
}

bool eval_pair(const BoundComparison& c, const Row& l, const Row& r,
               std::size_t nleft) {
  const Field& a = c.left < nleft ? l[c.left] : r[c.left - nleft];
  const Field& b = !c.right_is_column ? c.literal
                   : c.right < nleft  ? l[c.right]
                                      : r[c.right - nleft];
  if (a.is_null || b.is_null) return false;
  return holds(c.op, compare_fields(a, b, c.as_text));
}

// -------------------------------------------------------------- plan text

namespace {

void plan_lines(const Operator& op, std::size_t depth, std::ostringstream& os) {
  std::string line(depth * 2, ' ');
  line += op.describe();
  const std::size_t width = 60;
  os << line << std::string(line.size() < width ? width - line.size() : 1, ' ')
     << "rows=" << op.rows_out() << "\n";
  const std::string d = op.detail();
  if (!d.empty()) os << std::string(depth * 2 + 4, ' ') << d << "\n";
  for (const Operator* c : op.children()) plan_lines(*c, depth + 1, os);
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

std::string plan_text(const Operator& root) {
  std::ostringstream os;
  plan_lines(root, 0, os);
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
                   JoinHashTable::MixFn mix)
    : left_(std::move(left)), right_(std::move(right)),
      residual_(std::move(residual)), text_(std::move(text)), mix_(mix),
      nleft_(left_->schema().size()), table_(mix) {
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
  // Build: file every right row under its key. A row whose key is NULL can
  // never match (NULL equals nothing, not even NULL), so it is not filed.
  table_ = JoinHashTable(mix_);
  right_->open();
  Row r;
  while (right_->next(r)) {
    if (null_key(r, rkeys_)) continue;
    const std::uint64_t h = key_hash(r, rkeys_);
    table_.add(h, std::move(r));
  }
  right_->close();
  table_.finish();
  // Probe: stream the left input past the table, one row at a time.
  left_->open();
  probing_ = false;
}

void HashJoin::close() { left_->close(); }  // the table stays, for detail()

bool HashJoin::produce(Row& out) {
  for (;;) {
    if (!probing_) {
      if (!left_->next(outer_)) return false;
      if (null_key(outer_, lkeys_)) continue;
      probe_hash_ = key_hash(outer_, lkeys_);
      std::tie(pos_, end_) = table_.bucket_range(probe_hash_);
      probing_ = true;
    }
    while (pos_ < end_) {
      const std::uint32_t id = table_.id_at(pos_++);
      if (table_.hash_of(id) != probe_hash_) continue;  // another key, same bucket
      const Row& in = table_.row(id);
      if (!keys_equal(outer_, in)) continue;            // another key, same hash
      bool match = true;
      for (const BoundComparison& c : residual_)
        if (!eval_pair(c, outer_, in, nleft_)) { match = false; break; }
      if (!match) continue;
      join_rows(outer_, in, out);
      return true;
    }
    probing_ = false;  // this left row's bucket is used up
  }
}

std::string HashJoin::describe() const { return "HashJoin ON " + text_; }

std::string HashJoin::detail() const {
  const JoinHashTable::Occupancy o = table_.occupancy();
  return "hash table (right input): " + std::to_string(o.rows) + " rows in " +
         std::to_string(o.buckets) + " buckets, " + std::to_string(o.used) +
         " used, longest chain " + std::to_string(o.longest);
}

}  // namespace labdb
