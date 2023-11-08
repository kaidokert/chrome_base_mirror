// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_FUCHSIA_MEM_BUFFER_UTIL_H_
#define BASE_FUCHSIA_MEM_BUFFER_UTIL_H_

#include <fuchsia/mem/cpp/fidl.h>
#include <string>

#include "base/base_export.h"
#include "base/files/file.h"
#include "base/strings/string_piece.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {

// Returns the contents of `buffer` (which must be a valid UTF-8 string), or
// null in case of a conversion error.
BASE_EXPORT absl::optional<std::u16string> ReadUTF8FromVMOAsUTF16(
    const fuchsia::mem::Buffer& buffer);

// Creates a Fuchsia VMO from `data`. The size of the resulting virtual memory
// object will be set to the size of the string, and it will be given the name
// `name`.
BASE_EXPORT zx::vmo VmoFromString(StringPiece data, StringPiece name);

// Creates a Fuchsia memory buffer from `data`. The resulting virtual memory
// object will be given the name `name`.
// `fuchsia::mem::Buffer` is deprecated: for new interfaces, prefer using
// a VMO object directly (see `VmoFromString`).
BASE_EXPORT fuchsia::mem::Buffer MemBufferFromString(StringPiece data,
                                                     StringPiece name);

// Creates a Fuchsia memory buffer from the UTF-16 string `data`. The resulting
// virtual memory object will be given the name `name`.
BASE_EXPORT fuchsia::mem::Buffer MemBufferFromString16(StringPiece16 data,
                                                       StringPiece name);

// Returns the contents of `data`, or null if the read operation fails.
BASE_EXPORT absl::optional<std::string> StringFromVmo(const zx::vmo& vmo);

// Returns the contents of `buffer`, or null if the read operation fails.
// `fuchsia::mem::Buffer` is deprecated: for new interfaces, prefer using
// a VMO object directly (see `StringFromVmo`).
BASE_EXPORT absl::optional<std::string> StringFromMemBuffer(
    const fuchsia::mem::Buffer& buffer);

// Returns the contents of `data`, or null if the read operation fails.
BASE_EXPORT absl::optional<std::string> StringFromMemData(
    const fuchsia::mem::Data& data);

// Creates a memory-mapped, read-only Buffer with the contents of `file`. Will
// return an empty Buffer if the file could not be opened.
BASE_EXPORT fuchsia::mem::Buffer MemBufferFromFile(File file);

// Creates a non-resizeable, copy-on-write shared memory clone of `buffer`. The
// resulting virtual memory object will be given the name `name`.
BASE_EXPORT fuchsia::mem::Buffer CloneBuffer(const fuchsia::mem::Buffer& buffer,
                                             StringPiece name);

}  // namespace base

#endif  // BASE_FUCHSIA_MEM_BUFFER_UTIL_H_
