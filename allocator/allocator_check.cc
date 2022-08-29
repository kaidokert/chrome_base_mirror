// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/allocator_check.h"

#include "base/allocator/buildflags.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/allocator/partition_allocator/shim/winheap_stubs_win.h"
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include <malloc.h>
#endif

#if BUILDFLAG(IS_APPLE)
#include "base/allocator/partition_allocator/shim/allocator_interception_mac.h"
#endif

namespace base {
namespace allocator {

bool IsAllocatorInitialized() {
#if BUILDFLAG(IS_WIN) && BUILDFLAG(USE_ALLOCATOR_SHIM)
  // Set by allocator_shim_override_ucrt_symbols_win.h when the
  // shimmed _set_new_mode() is called.
  return g_is_win_shim_layer_initialized;
#elif BUILDFLAG(IS_APPLE) && !defined(MEMORY_TOOL_REPLACES_ALLOCATOR) && \
    !BUILDFLAG(USE_PARTITION_ALLOC_AS_MALLOC)
  // From allocator_interception_mac.mm.
  return base::allocator::g_replaced_default_zone;
#else
  return true;
#endif
}

}  // namespace allocator
}  // namespace base
