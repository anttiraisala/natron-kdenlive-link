// cache.h - in-memory output cache of processed frames, LRU eviction, bounded
// by a byte budget. Keys are content hashes (see protocol.h), so an entry can
// never be stale: if any input, comp file or parameter changes, the key changes.
//
// Milestone 1 keeps only the memory tier. A lossless compressed disk tier
// (outside the project folder by default) is planned for a later milestone.
#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nkb/protocol.h"

namespace nkb {

struct FrameMeta {
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat pixel_format = PixelFormat::Unknown;
  AlphaMode alpha = AlphaMode::Straight;
  Colorspace colorspace = Colorspace::Unspecified;
  int64_t frame_number = 0;
};

struct Frame {
  FrameMeta meta;
  std::vector<uint8_t> data;
};

struct CacheStats {
  uint64_t entries = 0;
  uint64_t bytes = 0;
  uint64_t target_bytes = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t too_large = 0;  // frames bigger than the whole budget, never cached
};

class FrameCache {
 public:
  explicit FrameCache(uint64_t target_bytes) : target_(target_bytes) {}
  // Changes the byte budget (used once at startup; does not evict by itself).
  void set_target_bytes(uint64_t b) { std::lock_guard<std::mutex> lk(m_); target_ = b; }

  // Returns nullptr on miss. A hit marks the entry most recently used.
  std::shared_ptr<const Frame> get(const Key& k);
  // Inserts/replaces, then evicts least recently used entries until the total
  // is within the budget. Returns the number of evicted entries.
  size_t put(const Key& k, std::shared_ptr<const Frame> f);

  struct ClearResult { uint64_t entries; uint64_t bytes; };
  ClearResult clear();
  CacheStats stats() const;

 private:
  struct Entry {
    Key key;
    std::shared_ptr<const Frame> frame;
  };
  using List = std::list<Entry>;

  mutable std::mutex m_;
  uint64_t target_;
  uint64_t bytes_ = 0;
  List lru_;  // front = most recently used
  std::unordered_map<Key, List::iterator, KeyHash> index_;
  uint64_t hits_ = 0, misses_ = 0, evictions_ = 0, too_large_ = 0;
};

}  // namespace nkb
