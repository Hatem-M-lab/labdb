#pragma once
// JoinHashTable -- the build side of a hash join. Unit 9.
//
// A hash join reads one input completely (the *build* side), files every row
// under its join key, and then streams the other input past it (the *probe*
// side), looking each probe row's key up instead of comparing it with every
// build row. This is the table it files them in.
//
// Layout: all build rows in one vector, plus a bucket index in "compressed
// row" form -- bucket b's row ids are order_[offset_[b] .. offset_[b+1]).
// Rows are added first and grouped by bucket once, in finish(), with a
// counting pass and a placement pass. A probe hashes its key, reads two
// offsets, and walks one short, contiguous run of ids.
//
// The bucket for a key is mix(hash) & (buckets - 1): a power-of-two bucket
// count, so a mask replaces a division. A mask keeps only the LOW bits of
// its input, so the input must have its bits well mixed first -- every input
// bit should influence every low output bit. That is what mix64 is for, and
// what Challenge 9.5's trap leaves out. The table counts how its rows spread
// across buckets (occupancy()) so a bad spread can be seen, not guessed.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "record.hpp"

namespace labdb {

// MurmurHash3's 64-bit finalizer (Austin Appleby, public domain): a bijection
// on 64-bit words in which every input bit affects every output bit.
inline std::uint64_t mix64(std::uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

// 64-bit FNV-1a over a string's bytes: the raw hash of a TEXT key.
inline std::uint64_t fnv1a(const std::string& s) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

class JoinHashTable {
 public:
  using MixFn = std::uint64_t (*)(std::uint64_t);

  explicit JoinHashTable(MixFn mix = mix64) : mix_(mix) {}

  // Build phase: file a row under its key's raw hash.
  void add(std::uint64_t key_hash, Row row) {
    hashes_.push_back(key_hash);
    rows_.push_back(std::move(row));
  }
  // Unit 12: file only the hash -- for a build side whose rows are kept
  // elsewhere, as columns, where row id i is the i-th hash filed.
  void add_key(std::uint64_t key_hash) { hashes_.push_back(key_hash); }

  // End of the build: choose the bucket count (the smallest power of two
  // that is at least the row count, so the load factor is at most 1) and
  // group the row ids by bucket. Probes may begin after this.
  void finish() {
    std::size_t nb = 1;
    while (nb < hashes_.size()) nb <<= 1;
    mask_ = nb - 1;
    offset_.assign(nb + 1, 0);
    for (std::uint64_t h : hashes_) ++offset_[bucket_of(h) + 1];
    for (std::size_t b = 0; b < nb; ++b) offset_[b + 1] += offset_[b];
    order_.resize(hashes_.size());
    std::vector<std::uint32_t> fill(offset_.begin(), offset_.end() - 1);
    for (std::uint32_t id = 0; id < hashes_.size(); ++id)
      order_[fill[bucket_of(hashes_[id])]++] = id;
  }

  // The run of positions holding the rows in `key_hash`'s bucket.
  std::pair<std::uint32_t, std::uint32_t> bucket_range(std::uint64_t key_hash) const {
    const std::size_t b = bucket_of(key_hash);
    return {offset_[b], offset_[b + 1]};
  }
  std::uint32_t id_at(std::uint32_t pos) const { return order_[pos]; }
  std::uint64_t hash_of(std::uint32_t id) const { return hashes_[id]; }
  const Row& row(std::uint32_t id) const { return rows_[id]; }
  std::size_t size() const { return hashes_.size(); }

  // How the rows spread across buckets. `chain_hist[k]` counts the buckets
  // holding a number of rows in the k-th class: 0, 1, 2, 3-4, 5-8, 9-16, ...
  struct Occupancy {
    std::size_t rows = 0, buckets = 0, used = 0, longest = 0;
    std::vector<std::size_t> chain_hist;
  };
  Occupancy occupancy() const {
    Occupancy o;
    o.rows = hashes_.size();
    o.buckets = offset_.empty() ? 0 : offset_.size() - 1;
    for (std::size_t b = 0; b < o.buckets; ++b) {
      const std::size_t len = offset_[b + 1] - offset_[b];
      if (len) ++o.used;
      if (len > o.longest) o.longest = len;
      std::size_t cls = 0;  // 0 -> 0, 1 -> 1, 2 -> 2, 3..4 -> 3, 5..8 -> 4, ...
      if (len > 0) {
        cls = 1;
        for (std::size_t cap = 1; cap < len; cap <<= 1) ++cls;
      }
      if (o.chain_hist.size() <= cls) o.chain_hist.resize(cls + 1, 0);
      ++o.chain_hist[cls];
    }
    return o;
  }

 private:
  std::size_t bucket_of(std::uint64_t h) const { return mix_(h) & mask_; }

  MixFn mix_;
  std::vector<Row> rows_;
  std::vector<std::uint64_t> hashes_;
  std::vector<std::uint32_t> offset_;  // buckets + 1
  std::vector<std::uint32_t> order_;   // row ids, grouped by bucket
  std::size_t mask_ = 0;
};

}  // namespace labdb
