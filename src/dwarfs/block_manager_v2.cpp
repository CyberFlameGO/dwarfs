/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <sparsehash/dense_hash_map>

#include <folly/stats/Histogram.h>

#include "dwarfs/block_data.h"
#include "dwarfs/block_manager.h"
#include "dwarfs/compiler.h"
#include "dwarfs/cyclic_hash.h"
#include "dwarfs/entry.h"
#include "dwarfs/error.h"
#include "dwarfs/filesystem_writer.h"
#include "dwarfs/inode.h"
#include "dwarfs/logger.h"
#include "dwarfs/mmif.h"
#include "dwarfs/os_access.h"
#include "dwarfs/progress.h"
#include "dwarfs/util.h"

namespace dwarfs {

/**
 * Block Manager Strategy
 *
 * For each *block*, start new rolling hash. The the hashes are associcated
 * with the block, new hash-offset-pairs will only be added as the block
 * grows. We only need to store a hash-offset-pair every N bytes, with N
 * being configurable (typically half of the window size so we find all
 * matches of at least 1.5 times the window size, but could also be smaller).
 *
 * For each *file*, start new rolling hash. Hash values *expire* immediately,
 * no history of hash values is kept. Up to n blocks (typically only one)
 * will be scanned for matches. Old file data beyond the moving window will
 * be added to the current *block*, causing the rolling *block* hash to also
 * advance. Data needs to be added to the block only in increments at which
 * a new hash valus is computed.
 *
 * This strategy ensures that we're using a deterministic amount of memory
 * (proportional to block size and history block count).
 *
 * A single window size is sufficient. That window size should still be
 * configurable.
 */

struct bm_stats {
  bm_stats()
      : l2_collision_vec_size(1, 0, 128) {}

  size_t total_hashes{0};
  size_t l2_collisions{0};
  size_t total_matches{0};
  size_t good_matches{0};
  size_t bad_matches{0};
  folly::Histogram<size_t> l2_collision_vec_size;
};

template <typename KeyT, typename ValT, KeyT EmptyKey = KeyT{},
          size_t MaxCollInline = 2>
class fast_multimap {
 private:
  using collision_vector = folly::small_vector<ValT, MaxCollInline>;
  using blockhash_t = google::dense_hash_map<KeyT, ValT>;
  using collision_t = std::unordered_map<KeyT, collision_vector>;

 public:
  fast_multimap() { values_.set_empty_key(EmptyKey); }

  void insert(KeyT const& key, ValT const& val) {
    if (key == EmptyKey or !values_.insert(std::make_pair(key, val)).second) {
      collisions_[key].emplace_back(val);
    }
  }

  template <typename F>
  void for_each_value(KeyT const& key, F&& func) const {
    if (auto it = values_.find(key); it != values_.end()) {
      func(it->second);
      if (auto it2 = collisions_.find(key); it2 != collisions_.end()) {
        for (auto const& val : it2->second) {
          func(val);
        }
      }
    }
  }

  void clear() {
    values_.clear();
    collisions_.clear();
  }

  blockhash_t const& values() const { return values_; };
  collision_t const& collisions() const { return collisions_; };

 private:
  blockhash_t values_;
  collision_t collisions_;
};

class active_block {
 private:
  using offset_t = uint32_t;
  using hash_t = uint32_t;

 public:
  active_block(size_t num, size_t size, size_t window_size, size_t window_step)
      : num_(num)
      , capacity_(size)
      , window_size_(window_size)
      , window_step_mask_(window_step - 1)
      , data_{std::make_shared<block_data>()} {
    DWARFS_CHECK((window_step & window_step_mask_) == 0,
                 "window step size not a power of two");
    data_->vec().reserve(capacity_);
  }

  size_t num() const { return num_; }
  size_t size() const { return data_->size(); }

  bool full() const { return size() == capacity_; }
  std::shared_ptr<block_data> data() const { return data_; }

  void append(uint8_t const* p, size_t size);

  size_t next_hash_distance() const {
    return window_step_mask_ + 1 - (data_->vec().size() & window_step_mask_);
  }

  template <typename F>
  void for_each_offset(hash_t key, F&& func) const {
    offsets_.for_each_value(key, std::forward<F>(func));
  }

  void finalize(bm_stats& stats) {
    stats.total_hashes += offsets_.values().size();
    for (auto& c : offsets_.collisions()) {
      stats.total_hashes += c.second.size();
      stats.l2_collisions += c.second.size() - 1;
      stats.l2_collision_vec_size.addValue(c.second.size());
    }
  }

 private:
  static constexpr size_t num_inline_offsets = 4;

  size_t num_, capacity_, window_size_, window_step_mask_;
  rsync_hash hasher_;
  fast_multimap<hash_t, offset_t, num_inline_offsets> offsets_;
  std::shared_ptr<block_data> data_;
};

template <typename LoggerPolicy>
class block_manager_ : public block_manager::impl {
 public:
  block_manager_(logger& lgr, progress& prog, const block_manager::config& cfg,
                 std::shared_ptr<os_access> os, filesystem_writer& fsw)
      : log_{lgr}
      , prog_{prog}
      , cfg_{cfg}
      , os_{std::move(os)}
      , fsw_{fsw}
      , window_size_{cfg.blockhash_window_size > 0
                         ? static_cast<size_t>(1) << cfg.blockhash_window_size
                         : 0}
      , window_step_{window_size_ >> cfg.window_increment_shift}
      , block_size_{static_cast<size_t>(1) << cfg.block_size_bits} {}

  void add_inode(std::shared_ptr<inode> ino) override;
  void finish_blocks() override;

 private:
  struct chunk_state {
    size_t offset{0};
    size_t size{0};
  };

  void block_ready();
  void finish_chunk(inode& ino);
  void append_to_block(inode& ino, mmif& mm, size_t offset, size_t size);
  void add_data(inode& ino, mmif& mm, size_t offset, size_t size);
  void segment_and_add_data(inode& ino, mmif& mm, size_t size);

  log_proxy<LoggerPolicy> log_;
  progress& prog_;
  const block_manager::config& cfg_;
  std::shared_ptr<os_access> os_;
  filesystem_writer& fsw_;

  size_t const window_size_;
  size_t const window_step_;
  size_t const block_size_;
  size_t block_count_{0};

  chunk_state chunk_;

  bm_stats stats_;

  // Active blocks are blocks that can still be referenced from new chunks.
  // Up to N blocks (configurable) can be active and are kept in this queue.
  // All active blocks except for the last one are immutable and potentially
  // already being compressed.
  std::deque<active_block> blocks_;
};

class segment_match {
 public:
  segment_match(active_block const* blk, uint32_t off) noexcept
      : block_{blk}
      , offset_{off} {}

  void verify_and_extend(uint8_t const* pos, size_t len, uint8_t const* begin,
                         uint8_t const* end);

  bool operator<(segment_match const& rhs) const {
    return size_ < rhs.size_ ||
           (size_ == rhs.size_ &&
            (block_->num() < rhs.block_->num() ||
             (block_->num() == rhs.block_->num() && offset_ < rhs.offset_)));
  }

  uint8_t const* data() const { return data_; }
  uint32_t size() const { return size_; }
  uint32_t offset() const { return offset_; }
  size_t block_num() const { return block_->num(); }

 private:
  active_block const* block_;
  uint32_t offset_;
  uint32_t size_{0};
  uint8_t const* data_;
};

void active_block::append(uint8_t const* p, size_t size) {
  auto& v = data_->vec();
  auto offset = v.size();
  DWARFS_CHECK(offset + size <= capacity_,
               fmt::format("block capacity exceeded: {} + {} > {}", offset,
                           size, capacity_));
  v.resize(offset + size);
  ::memcpy(v.data() + offset, p, size);

  while (offset < v.size()) {
    if (offset < window_size_) {
      hasher_.update(v[offset++]);
    } else {
      hasher_.update(v[offset - window_size_], v[offset]);
      if ((++offset & window_step_mask_) == 0) {
        offsets_.insert(hasher_(), offset - window_size_);
      }
    }
  }
}

void segment_match::verify_and_extend(uint8_t const* pos, size_t len,
                                      uint8_t const* begin,
                                      uint8_t const* end) {
  auto const& v = block_->data()->vec();

  if (::memcmp(v.data() + offset_, pos, len) == 0) {
    // scan backward
    auto tmp = offset_;
    while (tmp > 0 && pos > begin && v[tmp - 1] == pos[-1]) {
      --tmp;
      --pos;
    }
    len += offset_ - tmp;
    offset_ = tmp;
    data_ = pos;

    // scan forward
    pos += len;
    tmp = offset_ + len;
    while (tmp < v.size() && pos < end && v[tmp] == *pos) {
      ++tmp;
      ++pos;
    }
    size_ = tmp - offset_;
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::add_inode(std::shared_ptr<inode> ino) {
  auto e = ino->any();

  if (size_t size = e->size(); size > 0) {
    auto mm = os_->map_file(e->path(), size);

    LOG_TRACE << "adding inode " << ino->num() << " [" << ino->any()->name()
              << "] - size: " << size;

    if (window_size_ == 0 or size < window_size_) {
      // no point dealing with hashing, just write it out
      add_data(*ino, *mm, 0, size);
      finish_chunk(*ino);
    } else {
      segment_and_add_data(*ino, *mm, size);
    }
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::finish_blocks() {
  if (!blocks_.empty()) {
    block_ready();
  }

  auto l1_collisions = stats_.l2_collision_vec_size.computeTotalCount();

  LOG_INFO << "segmentation matches: good=" << stats_.good_matches
           << ", bad=" << stats_.bad_matches
           << ", total=" << stats_.total_matches;
  LOG_INFO << "segmentation collisions: L1="
           << fmt::format("{:.3f}%",
                          100.0 * (l1_collisions + stats_.l2_collisions) /
                              stats_.total_hashes)
           << ", L2="
           << fmt::format("{:.3f}%",
                          100.0 * stats_.l2_collisions / stats_.total_hashes)
           << " [" << stats_.total_hashes << " hashes]";

  if (l1_collisions > 0) {
    auto pct = [&](double p) {
      return stats_.l2_collision_vec_size.getPercentileEstimate(p);
    };
    LOG_DEBUG << "collision vector size p50: " << pct(0.5)
              << ", p75: " << pct(0.75) << ", p90: " << pct(0.9)
              << ", p95: " << pct(0.95) << ", p99: " << pct(0.99);
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::block_ready() {
  auto& block = blocks_.back();
  block.finalize(stats_);
  fsw_.write_block(block.data());
  ++prog_.block_count;
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::append_to_block(inode& ino, mmif& mm,
                                                   size_t offset, size_t size) {
  if (DWARFS_UNLIKELY(blocks_.empty() or blocks_.back().full())) {
    if (blocks_.size() >= cfg_.max_active_blocks) {
      blocks_.pop_front();
    }

    blocks_.emplace_back(block_count_++, block_size_, window_size_,
                         window_step_);
  }

  auto& block = blocks_.back();

  block.append(mm.as<uint8_t>(offset), size);
  chunk_.size += size;

  prog_.filesystem_size += size;

  if (block.full()) {
    mm.release_until(offset + size);
    finish_chunk(ino);
    block_ready();
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::add_data(inode& ino, mmif& mm, size_t offset,
                                            size_t size) {
  while (size > 0) {
    size_t block_offset = 0;

    if (!blocks_.empty()) {
      block_offset = blocks_.back().size();
    }

    size_t chunk_size = std::min(size, block_size_ - block_offset);

    append_to_block(ino, mm, offset, chunk_size);

    offset += chunk_size;
    size -= chunk_size;
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::finish_chunk(inode& ino) {
  if (chunk_.size > 0) {
    auto& block = blocks_.back();
    ino.add_chunk(block.num(), chunk_.offset, chunk_.size);
    chunk_.offset = block.full() ? 0 : block.size();
    chunk_.size = 0;
    prog_.chunk_count++;
  }
}

template <typename LoggerPolicy>
void block_manager_<LoggerPolicy>::segment_and_add_data(inode& ino, mmif& mm,
                                                        size_t size) {
  rsync_hash hasher;
  size_t offset = 0;
  size_t written = 0;
  size_t lookback_size = window_size_ + window_step_;
  size_t next_hash_offset =
      lookback_size +
      (blocks_.empty() ? window_step_ : blocks_.back().next_hash_distance());
  auto p = mm.as<uint8_t>();

  DWARFS_CHECK(size >= window_size_, "unexpected call to segment_and_add_data");

  for (; offset < window_size_; ++offset) {
    hasher.update(p[offset]);
  }

  std::vector<segment_match> matches;

  while (offset < size) {
    for (auto const& block : blocks_) {
      block.for_each_offset(hasher(), [&](uint32_t offset) {
        matches.emplace_back(&block, offset);
      });
    }

    if (!matches.empty()) {
      LOG_TRACE << "found " << matches.size() << " matches (hash=" << hasher()
                << ", window size=" << window_size_ << ")";

      for (auto& m : matches) {
        LOG_TRACE << "  block " << m.block_num() << " @ " << m.offset();
        m.verify_and_extend(p + offset - window_size_, window_size_,
                            p + written, p + size);
      }

      stats_.total_matches += matches.size();
      stats_.bad_matches +=
          std::count_if(matches.begin(), matches.end(),
                        [](auto const& m) { return m.size() == 0; });

      auto best = std::max_element(matches.begin(), matches.end());
      auto match_len = best->size();

      if (match_len > 0) {
        ++stats_.good_matches;
        LOG_TRACE << "successful match of length " << match_len << " @ "
                  << best->offset();

        auto block_num = best->block_num();
        auto match_off = best->offset();
        auto num_to_write = best->data() - (p + written);

        // best->block can be invalidated by this call to add_data()!
        add_data(ino, mm, written, num_to_write);
        written += num_to_write;
        finish_chunk(ino);

        ino.add_chunk(block_num, match_off, match_len);
        written += match_len;

        prog_.saved_by_segmentation += match_len;

        offset = written;

        if (size - written < window_size_) {
          break;
        }

        hasher.clear();

        for (; offset < written + window_size_; ++offset) {
          hasher.update(p[offset]);
        }

        next_hash_offset =
            written + lookback_size + blocks_.back().next_hash_distance();
      }

      matches.clear();

      if (match_len > 0) {
        continue;
      }
    }

    // no matches found, see if we can append data
    // we need to keep at least lookback_size bytes unwritten

    if (offset == next_hash_offset) {
      auto num_to_write = offset - lookback_size - written;
      add_data(ino, mm, written, num_to_write);
      written += num_to_write;
      next_hash_offset += window_step_;
    }

    hasher.update(p[offset - window_size_], p[offset]);
    ++offset;
  }

  add_data(ino, mm, written, size - written);
  finish_chunk(ino);
}

block_manager::block_manager(logger& lgr, progress& prog, const config& cfg,
                             std::shared_ptr<os_access> os,
                             filesystem_writer& fsw)
    : impl_(make_unique_logging_object<impl, block_manager_, logger_policies>(
          lgr, prog, cfg, os, fsw)) {}

} // namespace dwarfs