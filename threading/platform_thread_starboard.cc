// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/threading/platform_thread.h"

#include "base/notreached.h"

namespace base {

void PlatformThread::SetName(const std::string& name) {
    NOTREACHED();
}

void InitThreading() {}
void TerminateOnThread() {}

size_t GetDefaultThreadStackSize(const pthread_attr_t& attributes) {
    NOTREACHED();
    return 0;
}

}