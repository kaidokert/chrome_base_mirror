// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_STRINGS_STRING_SLICE_H_
#define BASE_STRINGS_STRING_SLICE_H_

#include <stdint.h>

#include <bit>
#include <string_view>

namespace base::internal {

// Determines the minimum unsigned integer type needed to hold `kSize`.
template <size_t kSize>
struct IndexTypeForSize {
 private:
  static constexpr auto Helper() {
    constexpr int kMinBits = std::bit_width(kSize);
    if constexpr (kMinBits <= 8) {
      return uint8_t();
    } else if constexpr (kMinBits <= 16) {
      return uint16_t();
    } else if constexpr (kMinBits <= 32) {
      return uint32_t();
    } else {
      return size_t();
    }
  }

 public:
  using Type = decltype(Helper());
};

}  // namespace base::internal

namespace base::subtle {

// This is intended for use in tables generated by scripts and should not be
// directly used. Consider a global constant like this:
//
// constexpr std::string_view kNames[] = {
//   "Alice",
//   "Bob",
//   "Eve",
// };
//
// "Alice", "Bob", and "Eve" are also constants, but they are not stored inline
// in `kNames`; `kNames[0]` points to "Alice", `kNames[1]` points to "Bob", and
// `kNames[2]` points to "Eve". However, images can be loaded at arbitrary base
// addresses, so the actual pointer values are unknown at build time.
//
// To solve this, the tooling stores `kNames` in `.data.rel.ro` and records 3
// relocations in `.rela.dyn`. When the image is loaded, the linker applies the
// relocations to fix up the addresses before marking the section read-only.
//
// Unfortunately, this has both a binary size cost (relocation entries are
// relatively large unless RELR is in use) and a runtime cost (to apply the
// relocations).
//
// StringSlice avoids relocations by only storing an offset and a length and
// dynamically resolving to a std::string_view at runtime. Using `StringSlice`,
// the above example might look like this instead:
//
// constexpr char kData[] = "AliceBobEve";
// constexpr StringSlice<sizeof(kData), kData> kNames[] = {
//   {0, 5},
//   {5, 3},
//   {8, 3},
// };
//
// While this has a small runtime cost (typically a PC-relative load), modern
// CPUs are quite good at this sort of math.
template <size_t N, const char (&kData)[N]>
struct StringSlice {
  using IndexType = typename internal::IndexTypeForSize<N>::Type;

  IndexType offset;
  IndexType length;

  friend constexpr bool operator==(StringSlice lhs, StringSlice rhs) {
    return std::string_view(lhs) == std::string_view(rhs);
  }
  friend constexpr auto operator<=>(StringSlice lhs, StringSlice rhs) {
    return std::string_view(lhs) <=> std::string_view(rhs);
  }
  constexpr operator std::string_view() const {
    // Note 1: using as_string_view() or span() can cause issues with constexpr
    // evaluation limits.
    // Note 2: Subtract 1 from kData since this is intended for use with string
    // literals, and the terminating nul should not be included.
    return std::string_view(kData, sizeof(kData) - 1).substr(offset, length);
  }
};

}  // namespace base::subtle

#endif  // BASE_STRINGS_STRING_SLICE_H_
