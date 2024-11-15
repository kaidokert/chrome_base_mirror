// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_CONTAINERS_ADAPTERS_H_
#define BASE_CONTAINERS_ADAPTERS_H_

#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/adapters_internal.h"

namespace base {

// Reversed returns a container adapter usable in a range-based "for" statement
// for iterating a reversible container in reverse order.
//
// Example:
//
//   std::vector<int> v = ...;
//   for (int i : base::Reversed(v)) {
//     // iterates through v from back to front
//   }
template <typename Range>
auto Reversed(Range&& range LIFETIME_BOUND) {
  return internal::ReversedAdapter<Range>(std::forward<Range>(range));
}

}  // namespace base

#endif  // BASE_CONTAINERS_ADAPTERS_H_
