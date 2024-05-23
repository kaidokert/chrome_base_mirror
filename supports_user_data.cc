// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/supports_user_data.h"

#include "base/feature_list.h"
#include "base/sequence_checker.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_map.h"

namespace base {

struct SupportsUserData::Impl {
  // Externally-defined data accessible by key.
  absl::flat_hash_map<const void*, std::unique_ptr<Data>> user_data_;
};

std::unique_ptr<SupportsUserData::Data> SupportsUserData::Data::Clone() {
  return nullptr;
}

SupportsUserData::SupportsUserData() : impl_(std::make_unique<Impl>()) {
  // Harmless to construct on a different execution sequence to subsequent
  // usage.
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

SupportsUserData::SupportsUserData(SupportsUserData&& rhs) {
  *this = std::move(rhs);
}

SupportsUserData& SupportsUserData::operator=(SupportsUserData&& rhs) {
  impl_ = std::move(rhs.impl_);
  in_destructor_ = rhs.in_destructor_;
  rhs.impl_ = std::make_unique<Impl>();
  return *this;
}

SupportsUserData::Data* SupportsUserData::GetUserData(const void* key) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Avoid null keys; they are too vulnerable to collision.
  DCHECK(key);
  auto found = impl_->user_data_.find(key);
  if (found != impl_->user_data_.end()) {
    return found->second.get();
  }
  return nullptr;
}

std::unique_ptr<SupportsUserData::Data> SupportsUserData::TakeUserData(
    const void* key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Null keys are too vulnerable to collision.
  CHECK(key);
  auto found = impl_->user_data_.find(key);
  if (found != impl_->user_data_.end()) {
    std::unique_ptr<SupportsUserData::Data> deowned;
    deowned.swap(found->second);
    impl_->user_data_.erase(key);
    return deowned;
  }
  return nullptr;
}

void SupportsUserData::SetUserData(const void* key,
                                   std::unique_ptr<Data> data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!in_destructor_) << "Calling SetUserData() when SupportsUserData is "
                            "being destroyed is not supported.";
  // Avoid null keys; they are too vulnerable to collision.
  DCHECK(key);
  if (data.get()) {
    impl_->user_data_[key] = std::move(data);
  } else {
    RemoveUserData(key);
  }
}

void SupportsUserData::RemoveUserData(const void* key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = impl_->user_data_.find(key);
  if (it != impl_->user_data_.end()) {
    // Remove the entry from the map before deleting `owned_data` to avoid
    // reentrancy issues when `owned_data` owns `this`. Otherwise:
    //
    // 1. `RemoveUserData()` calls `erase()`.
    // 2. `erase()` deletes `owned_data`.
    // 3. `owned_data` deletes `this`.
    //
    // At this point, `erase()` is still on the stack even though the
    // backing map (owned by `this`) has already been destroyed, and it
    // may simply crash, cause a use-after-free, or any other number of
    // interesting things.
    auto owned_data = std::move(it->second);
    impl_->user_data_.erase(it);
  }
}

void SupportsUserData::DetachFromSequence() {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

void SupportsUserData::CloneDataFrom(const SupportsUserData& other) {
  for (const auto& data_pair : other.impl_->user_data_) {
    auto cloned_data = data_pair.second->Clone();
    if (cloned_data) {
      SetUserData(data_pair.first, std::move(cloned_data));
    }
  }
}

SupportsUserData::~SupportsUserData() {
  if (!impl_->user_data_.empty()) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  }
  in_destructor_ = true;
  absl::flat_hash_map<const void*, std::unique_ptr<Data>> user_data;
  impl_->user_data_.swap(user_data);
  // Now this->impl_->user_data_ is empty, and any destructors called
  // transitively from the destruction of |local_user_data| will see it that
  // way instead of examining a being-destroyed object.
}

void SupportsUserData::ClearAllUserData() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  impl_->user_data_.clear();
}

}  // namespace base
