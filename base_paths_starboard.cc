// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Defines base::PathProviderPosix, default path provider on POSIX OSes that
// don't have their own base_paths_OS.cc implementation (i.e. all but Mac and
// Android).

#include "base/base_paths.h"

#include <limits.h>
#include <stddef.h>

#include <memory>
#include <ostream>
#include <string>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/nix/xdg_util.h"
#include "base/notreached.h"
#include "base/path_service.h"
#include "base/process/process_metrics.h"
#include "build/build_config.h"

namespace base {

const char kProcSelfExe[] = "/proc/self/exe";

bool PathProviderPosix(int key, FilePath* result) {
  switch (key) {
    case FILE_EXE:
    case FILE_MODULE: {  // TODO(evanm): is this correct?
      FilePath bin_dir;
      if (!ReadSymbolicLink(FilePath("/proc/self/exe"), &bin_dir)) {
        NOTREACHED() << "Unable to resolve " << kProcSelfExe << ".";
        return false;
      }
      *result = bin_dir;
      return true;
    }
    case DIR_SRC_TEST_DATA_ROOT: {
      // Allow passing this in the environment, for more flexibility in build
      // tree configurations (sub-project builds, gyp --output_dir, etc.)
      std::unique_ptr<Environment> env(Environment::Create());
      std::string cr_source_root;
      FilePath path;
      if (env->GetVar("CR_SOURCE_ROOT", &cr_source_root)) {
        path = FilePath(cr_source_root);
        if (PathExists(path)) {
          *result = path;
          return true;
        }
        DLOG(WARNING) << "CR_SOURCE_ROOT is set, but it appears to not "
                      << "point to a directory.";
      }
      // On POSIX, unit tests execute two levels deep from the source root.
      // For example:  out/{Debug|Release}/net_unittest
      if (PathService::Get(DIR_EXE, &path)) {
        *result = path.DirName().DirName();
        return true;
      }

      DLOG(ERROR) << "Couldn't find your source root.  "
                  << "Try running from your chromium/src directory.";
      return false;
    }
    case DIR_USER_DESKTOP:
      return false;
    case DIR_CACHE: {
      return false;
    }
  }
  return false;
}

}  // namespace base
