// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/lock_impl.h"
#include "base/logging.h"

// NOTE: Although windows critical sections support recursive locks, we do not
// allow this, and we will commonly fire a DCHECK() if a thread attempts to
// acquire the lock a second time (while already holding it).

LockImpl::LockImpl() {
#ifndef NDEBUG
  recursion_count_shadow_ = 0;
  recursion_used_ = false;
#endif  // NDEBUG
  // The second parameter is the spin count, for short-held locks it avoid the
  // contending thread from going to sleep which helps performance greatly.
  ::InitializeCriticalSectionAndSpinCount(&os_lock_, 2000);
}

LockImpl::~LockImpl() {
  ::DeleteCriticalSection(&os_lock_);
}

bool LockImpl::Try() {
  if (::TryEnterCriticalSection(&os_lock_) != FALSE) {
#ifndef NDEBUG
    recursion_count_shadow_++;
    if (2 == recursion_count_shadow_ && !recursion_used_) {
      recursion_used_ = true;
      DCHECK(false);  // Catch accidental redundant lock acquisition.
    }
#endif
    return true;
  }
  return false;
}

void LockImpl::Lock() {
  ::EnterCriticalSection(&os_lock_);
#ifndef NDEBUG
  // ONLY access data after locking.
  recursion_count_shadow_++;
  if (2 == recursion_count_shadow_ && !recursion_used_) {
    recursion_used_ = true;
    DCHECK(false);  // Catch accidental redundant lock acquisition.
  }
#endif  // NDEBUG
}

void LockImpl::Unlock() {
#ifndef NDEBUG
  --recursion_count_shadow_;  // ONLY access while lock is still held.
  DCHECK(0 <= recursion_count_shadow_);
#endif  // NDEBUG
  ::LeaveCriticalSection(&os_lock_);
}
