#include "nkb/cache.h"

namespace nkb {

std::shared_ptr<const Frame> FrameCache::get(const Key& k) {
  std::lock_guard<std::mutex> lk(m_);
  auto it = index_.find(k);
  if (it == index_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  lru_.splice(lru_.begin(), lru_, it->second);  // mark most recently used
  return it->second->frame;
}

size_t FrameCache::put(const Key& k, std::shared_ptr<const Frame> f) {
  std::lock_guard<std::mutex> lk(m_);
  const uint64_t sz = f->data.size();
  if (sz > target_) {  // would evict everything and still not fit
    ++too_large_;
    return 0;
  }
  auto it = index_.find(k);
  if (it != index_.end()) {  // replace
    bytes_ -= it->second->frame->data.size();
    lru_.erase(it->second);
    index_.erase(it);
  }
  lru_.push_front(Entry{k, std::move(f)});
  index_[k] = lru_.begin();
  bytes_ += sz;
  size_t evicted = 0;
  while (bytes_ > target_ && !lru_.empty()) {
    auto& victim = lru_.back();
    bytes_ -= victim.frame->data.size();
    index_.erase(victim.key);
    lru_.pop_back();
    ++evicted;
    ++evictions_;
  }
  return evicted;
}

FrameCache::ClearResult FrameCache::clear() {
  std::lock_guard<std::mutex> lk(m_);
  ClearResult r{lru_.size(), bytes_};
  lru_.clear();
  index_.clear();
  bytes_ = 0;
  return r;
}

CacheStats FrameCache::stats() const {
  std::lock_guard<std::mutex> lk(m_);
  CacheStats s;
  s.entries = lru_.size();
  s.bytes = bytes_;
  s.target_bytes = target_;
  s.hits = hits_;
  s.misses = misses_;
  s.evictions = evictions_;
  s.too_large = too_large_;
  return s;
}

}  // namespace nkb
