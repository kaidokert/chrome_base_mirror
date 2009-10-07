// Copyright (c) 2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/mac_util.h"

#import <Cocoa/Cocoa.h>

#include "base/file_path.h"
#include "base/logging.h"
#include "base/scoped_cftyperef.h"
#include "base/sys_string_conversions.h"

namespace mac_util {

std::string PathFromFSRef(const FSRef& ref) {
  scoped_cftyperef<CFURLRef> url(
      CFURLCreateFromFSRef(kCFAllocatorDefault, &ref));
  NSString *path_string = [(NSURL *)url.get() path];
  return [path_string fileSystemRepresentation];
}

bool FSRefFromPath(const std::string& path, FSRef* ref) {
  OSStatus status = FSPathMakeRef((const UInt8*)path.c_str(),
                                  ref, nil);
  return status == noErr;
}

// Adapted from http://developer.apple.com/carbon/tipsandtricks.html#AmIBundled
bool AmIBundled() {
  ProcessSerialNumber psn = {0, kCurrentProcess};

  FSRef fsref;
  if (GetProcessBundleLocation(&psn, &fsref) != noErr)
    return false;

  FSCatalogInfo info;
  if (FSGetCatalogInfo(&fsref, kFSCatInfoNodeFlags, &info,
                       NULL, NULL, NULL) != noErr) {
    return false;
  }

  return info.nodeFlags & kFSNodeIsDirectoryMask;
}

bool IsBackgroundOnlyProcess() {
  // This function really does want to examine NSBundle's idea of the main
  // bundle dictionary, and not the overriden MainAppBundle.  It needs to look
  // at the actual running .app's Info.plist to access its LSUIElement
  // property.
  NSDictionary* info_dictionary = [[NSBundle mainBundle] infoDictionary];
  return [[info_dictionary objectForKey:@"LSUIElement"] boolValue] != NO;
}

// No threading worries since NSBundle isn't thread safe.
static NSBundle* g_override_app_bundle = nil;

NSBundle* MainAppBundle() {
  if (g_override_app_bundle)
    return g_override_app_bundle;
  return [NSBundle mainBundle];
}

FilePath MainAppBundlePath() {
  NSBundle* bundle = MainAppBundle();
  return FilePath([[bundle bundlePath] fileSystemRepresentation]);
}

void SetOverrideAppBundle(NSBundle* bundle) {
  if (bundle != g_override_app_bundle) {
    [g_override_app_bundle release];
    g_override_app_bundle = [bundle retain];
  }
}

void SetOverrideAppBundlePath(const FilePath& file_path) {
  NSString* path = base::SysUTF8ToNSString(file_path.value());
  NSBundle* bundle = [NSBundle bundleWithPath:path];
  DCHECK(bundle) << "failed to load the bundle: " << file_path.value();

  SetOverrideAppBundle(bundle);
}

OSType CreatorCodeForCFBundleRef(CFBundleRef bundle) {
  OSType creator = kUnknownType;
  CFBundleGetPackageInfo(bundle, NULL, &creator);
  return creator;
}

OSType CreatorCodeForApplication() {
  CFBundleRef bundle = CFBundleGetMainBundle();
  if (!bundle)
    return kUnknownType;

  return CreatorCodeForCFBundleRef(bundle);
}

FilePath GetUserLibraryPath() {
  NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory,
                                                      NSUserDomainMask, YES);
  if ([dirs count] == 0)
    return FilePath();

  NSString* library_dir = [dirs objectAtIndex:0];
  const char* library_dir_path = [library_dir fileSystemRepresentation];
  if (!library_dir_path)
    return FilePath();

  return FilePath(library_dir_path);
}

CGColorSpaceRef GetSRGBColorSpace() {
  // Leaked.  That's OK, it's scoped to the lifetime of the application.
  static CGColorSpaceRef g_color_space_sRGB =
      CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  LOG_IF(ERROR, !g_color_space_sRGB) << "Couldn't get the sRGB color space";
  return g_color_space_sRGB;
}

CGColorSpaceRef GetSystemColorSpace() {
  // Leaked.  That's OK, it's scoped to the lifetime of the application.
  // Try to get the main display's color space.
  static CGColorSpaceRef g_system_color_space =
      CGDisplayCopyColorSpace(CGMainDisplayID());

  if (!g_system_color_space) {
    // Use a generic RGB color space.  This is better than nothing.
    g_system_color_space = CGColorSpaceCreateDeviceRGB();

    if (g_system_color_space) {
      LOG(WARNING) <<
          "Couldn't get the main display's color space, using generic";
    } else {
      LOG(ERROR) << "Couldn't get any color space";
    }
  }

  return g_system_color_space;
}

}  // namespace mac_util
