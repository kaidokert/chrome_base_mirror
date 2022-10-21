// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/system/sys_info.h"

#include <stddef.h>
#include <stdint.h>

#include "base/notreached.h"

namespace {

uint64_t AmountOfMemory(int pages_name) {
  return 0;
}

}  // namespace

namespace base {

/*
// static
int SysInfo::NumberOfProcessors() {
  return 1;
}
*/

// static
uint64_t SysInfo::AmountOfPhysicalMemoryImpl() {
  return AmountOfMemory(0);
}

// static
uint64_t SysInfo::AmountOfAvailablePhysicalMemoryImpl() {
  return AmountOfMemory(0);
}

// static
std::string SysInfo::CPUModelName() {
  char name[256] = "<unknown>";
  return name;
}

SysInfo::HardwareInfo SysInfo::GetHardwareInfoSync() {
  HardwareInfo info;
  info.manufacturer = "starboard_manufacturer";
  info.model = "starboard_model";
  return info;
}

}  // namespace base
