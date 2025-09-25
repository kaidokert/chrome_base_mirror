// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_SAMPLING_HEAP_PROFILER_LOCK_FREE_ADDRESS_HASH_SET_H_
#define BASE_SAMPLING_HEAP_PROFILER_LOCK_FREE_ADDRESS_HASH_SET_H_

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "base/base_export.h"
#include "base/check_op.h"
#include "base/compiler_specific.h"
#include "base/feature_list.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/raw_ref.h"
#include "base/sampling_heap_profiler/lock_free_bloom_filter.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "base/types/optional_util.h"

namespace base {

// If enabled, LockFreeAddressHashSet will use the bloom filter.
BASE_EXPORT BASE_DECLARE_FEATURE(kUseLockFreeBloomFilter);

// A hash set container that provides lock-free version of |Contains| operation.
// It does not support concurrent write operations |Insert| and |Remove|.
// All write operations if performed from multiple threads must be properly
// guarded with a lock.
// |Contains| method can be executed concurrently with other |Insert|, |Remove|,
// or |Contains| even over the same key.
// However, please note the result of concurrent execution of |Contains|
// with |Insert| or |Remove| over the same key is racy.
//
// The hash set never rehashes, so the number of buckets stays the same
// for the lifetime of the set.
//
// Internally the hashset is implemented as a vector of N buckets
// (N has to be a power of 2). Each bucket holds a single-linked list of
// nodes each corresponding to a key.
// It is not possible to really delete nodes from the list as there might
// be concurrent reads being executed over the node. The |Remove| operation
// just marks the node as empty by placing nullptr into its key field.
// Consequent |Insert| operations may reuse empty nodes when possible.
//
// The structure of the hashset for N buckets is the following:
// 0: {*}--> {key1,*}--> {key2,*}--> NULL
// 1: {*}--> NULL
// 2: {*}--> {NULL,*}--> {key3,*}--> {key4,*}--> NULL
// ...
// N-1: {*}--> {keyM,*}--> NULL
class BASE_EXPORT LockFreeAddressHashSet {
 public:
  // Stats about the hash set's buckets, for metrics.
  struct BASE_EXPORT BucketStats {
    BucketStats(std::vector<size_t> lengths, double chi_squared);
    ~BucketStats();

    BucketStats(const BucketStats&);
    BucketStats& operator=(const BucketStats&);

    // Length of each bucket (ie. number of key slots that must be searched).
    std::vector<size_t> lengths;

    // Result of a chi-squared test that measures uniformity of bucket usage.
    double chi_squared = 0.0;
  };

  // Creates a hash set with `buckets_count` buckets. `lock` is a lock that
  // must be held by callers of |Insert|, |Remove| and |Copy|. |Contains| is
  // lock-free.
  LockFreeAddressHashSet(size_t buckets_count, Lock& lock);

  ~LockFreeAddressHashSet();

  enum class ContainsResult {
    // The key is in the hash set. If the kUseLockFreeBloomFilter feature is
    // enabled, it's also in the supplemental Bloom filter.
    kFound,
    // The key is not in the hash set. If the kUseLockFreeBloomFilter feature is
    // enabled, it's also not in the supplemental Bloom filter.
    kNotFound,
    // The key was matched in the supplemental Bloom filter, but not the hash
    // set. (A Bloom filter false positive.) This is only returned if the
    // kUseLockFreeBloomFilter feature is enabled, and is mainly useful for
    // tracking statistics of the Bloom filter usage.
    kNotFoundButMatchedInBloomFilter,
  };

  // Checks if the |key|, which must not be nullptr, is in the set. Checks the
  // bloom filter first if it's enabled. Can be executed concurrently with
  // |Insert|, |Remove|, and |Contains| operations.
  ALWAYS_INLINE ContainsResult Contains(void* key) const;

  // Removes the |key|, which must not be nullptr, from the set. The key must be
  // present in the set before the invocation. Concurrent execution of |Insert|,
  // |Remove|, or |Copy| is not supported.
  ALWAYS_INLINE void Remove(void* key);

  // Inserts the |key|, which must not be nullptr, into the set. The key must
  // not be present in the set before the invocation. Also adds the key to the
  // bloom filter if it's enabled. Concurrent execution of |Insert|, |Remove|,
  // or |Copy| is not supported.
  void Insert(void* key);

  // Copies contents of |other| set into the current set. The current set
  // must be empty before the call.
  // Concurrent execution of |Insert|, |Remove|, or |Copy| is not supported.
  void Copy(const LockFreeAddressHashSet& other);

  size_t buckets_count() const {
    // `buckets_` should never be resized.
    DCHECK_EQ(buckets_.size(), bucket_mask_ + 1);
    return buckets_.size();
  }

  size_t size() const {
    lock_->AssertAcquired();
    return size_;
  }

  // Returns the average bucket utilization.
  float load_factor() const {
    lock_->AssertAcquired();
    return 1.f * size() / buckets_.size();
  }

  // Returns stats about the buckets. Must not be called concurrently with
  // |Insert|, |Remove| or |Copy|.
  BucketStats GetBucketStats() const;

  LockFreeBloomFilter* bloom_filter() { return base::OptionalToPtr(filter_); }
  const LockFreeBloomFilter* bloom_filter() const {
    return base::OptionalToPtr(filter_);
  }

 private:
  friend class LockFreeAddressHashSetTest;

  struct Node {
    ALWAYS_INLINE Node(void* k, Node* next) : next(next) {
      key.store(k, std::memory_order_relaxed);
    }

    std::atomic<void*> key;

    // This field is not a raw_ptr<> to avoid out-of-line destructor.
    RAW_PTR_EXCLUSION Node* next;
  };

  // Returns the node containing `key` (which must not be null), or nullptr if
  // it's not in the hash set.
  ALWAYS_INLINE Node* FindNode(void* key);
  ALWAYS_INLINE const Node* FindNode(void* key) const;

  // Returns the hash of `key`.
  ALWAYS_INLINE static uint32_t Hash(void* key);

  raw_ref<Lock> lock_;

  std::vector<std::atomic<Node*>> buckets_;
  size_t size_ GUARDED_BY(lock_) = 0;
  const size_t bucket_mask_;

  // A bloom filter to speed up lookups of addresses not in the hash set.
  std::optional<LockFreeBloomFilter> filter_;
};

ALWAYS_INLINE LockFreeAddressHashSet::ContainsResult
LockFreeAddressHashSet::Contains(void* key) const {
  if (!filter_) {
    return FindNode(key) ? ContainsResult::kFound : ContainsResult::kNotFound;
  }
  if (filter_->MaybeContains(key)) {
    // The filter may have false positives, so need to check the hash set.
    return FindNode(key) ? ContainsResult::kFound
                         : ContainsResult::kNotFoundButMatchedInBloomFilter;
  }
  return ContainsResult::kNotFound;
}

ALWAYS_INLINE void LockFreeAddressHashSet::Remove(void* key) {
  lock_->AssertAcquired();
  Node* node = FindNode(key);
  DCHECK_NE(node, nullptr);
  // We can never delete the node, nor detach it from the current bucket
  // as there may always be another thread currently iterating over it.
  // Instead we just mark it as empty, so |Insert| can reuse it later.
  node->key.store(nullptr, std::memory_order_relaxed);
  --size_;
  // Keys can't be removed from the bloom filter. Old keys will be cleared out
  // when the hash set is resized.
}

ALWAYS_INLINE LockFreeAddressHashSet::Node* LockFreeAddressHashSet::FindNode(
    void* key) {
  DCHECK_NE(key, nullptr);
  const std::atomic<Node*>& bucket = buckets_[Hash(key) & bucket_mask_];
  // It's enough to use std::memory_order_consume ordering here, as the
  // node->next->...->next loads form a dependency chain.
  // However std::memory_order_consume is temporarily deprecated in C++17.
  // See https://isocpp.org/files/papers/p0636r0.html#removed
  // Make use of more strong std::memory_order_acquire for now.
  //
  // Update 2024-12-13: According to
  // https://en.cppreference.com/w/cpp/atomic/memory_order, C++20 changed the
  // semantics of a "consume operation" - see the definitions of
  // "Dependency-ordered before", "Simply happens-before" and "Strongly
  // happens-before" - but "Release-Consume ordering" still carries the note
  // that it's "temporarily discouraged" so it's unclear if it's now safe to use
  // here.
  for (Node* node = bucket.load(std::memory_order_acquire); node != nullptr;
       node = node->next) {
    if (node->key.load(std::memory_order_relaxed) == key) {
      return node;
    }
  }
  return nullptr;
}

ALWAYS_INLINE const LockFreeAddressHashSet::Node*
LockFreeAddressHashSet::FindNode(void* key) const {
  return const_cast<LockFreeAddressHashSet*>(this)->FindNode(key);
}

// static
ALWAYS_INLINE uint32_t LockFreeAddressHashSet::Hash(void* key) {
  // A simple fast hash function for addresses.
  constexpr uintptr_t random_bits = static_cast<uintptr_t>(0x4bfdb9df5a6f243b);
  uint64_t k = reinterpret_cast<uintptr_t>(key);
  return static_cast<uint32_t>((k * random_bits) >> 32);
}

}  // namespace base

#endif  // BASE_SAMPLING_HEAP_PROFILER_LOCK_FREE_ADDRESS_HASH_SET_H_
