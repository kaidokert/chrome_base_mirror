// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/gfx/platform_canvas_win.h"

#include "base/gfx/bitmap_platform_device_win.h"
#include "base/logging.h"
#include "base/process_util.h"

#ifdef ARCH_CPU_64_BITS
#error This code does not work on x64. Please make sure all the base unit tests\
 pass before doing any real work.
#endif

namespace gfx {

// Crashes the process. This is called when a bitmap allocation fails, and this
// function tries to determine why it might have failed, and crash on different
// lines. This allows us to see in crash dumps the most likely reason for the
// failure. It takes the size of the bitmap we were trying to allocate as its
// arguments so we can check that as well.
void CrashForBitmapAllocationFailure(int w, int h) {
  // The maximum number of GDI objects per process is 10K. If we're very close
  // to that, it's probably the problem.
  const int kLotsOfGDIObjs = 9990;
  CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) < kLotsOfGDIObjs);

  // If the bitmap is ginormous, then we probably can't allocate it.
  // We use 64M pixels = 256MB @ 4 bytes per pixel.
  const int64 kGinormousBitmapPxl = 64000000;
  CHECK(static_cast<int64>(w) * static_cast<int64>(h) < kGinormousBitmapPxl);

  // If we're using a crazy amount of virtual address space, then maybe there
  // isn't enough for our bitmap.
  const int64 kLotsOfMem = 1500000000;  // 1.5GB.
  scoped_ptr<process_util::ProcessMetrics> process_metrics(
      process_util::ProcessMetrics::CreateProcessMetrics(GetCurrentProcess()));
  CHECK(process_metrics->GetPagefileUsage() < kLotsOfMem);

  // Everything else.
  CHECK(0);
}


PlatformCanvasWin::PlatformCanvasWin() : SkCanvas() {
}

PlatformCanvasWin::PlatformCanvasWin(int width, int height, bool is_opaque)
    : SkCanvas() {
  bool initialized = initialize(width, height, is_opaque, NULL);
  if (!initialized) 
    CrashForBitmapAllocationFailure(width, height);
}

PlatformCanvasWin::PlatformCanvasWin(int width,
                               int height,
                               bool is_opaque,
                               HANDLE shared_section)
    : SkCanvas() {
  bool initialized = initialize(width, height, is_opaque, shared_section);
  if (!initialized) 
    CrashForBitmapAllocationFailure(width, height);
}

PlatformCanvasWin::~PlatformCanvasWin() {
}

bool PlatformCanvasWin::initialize(int width,
                                   int height,
                                   bool is_opaque,
                                   HANDLE shared_section) {
  SkDevice* device =
      createPlatformDevice(width, height, is_opaque, shared_section);
  if (!device)
    return false;

  setDevice(device);
  device->unref(); // was created with refcount 1, and setDevice also refs
  return true;
}

HDC PlatformCanvasWin::beginPlatformPaint() {
  return getTopPlatformDevice().getBitmapDC();
}

void PlatformCanvasWin::endPlatformPaint() {
  // we don't clear the DC here since it will be likely to be used again
  // flushing will be done in onAccessBitmap
}

PlatformDeviceWin& PlatformCanvasWin::getTopPlatformDevice() const {
  // All of our devices should be our special PlatformDevice.
  SkCanvas::LayerIter iter(const_cast<PlatformCanvasWin*>(this), false);
  return *static_cast<PlatformDeviceWin*>(iter.device());
}

SkDevice* PlatformCanvasWin::createDevice(SkBitmap::Config config,
                                       int width,
                                       int height,
                                       bool is_opaque, bool isForLayer) {
  DCHECK(config == SkBitmap::kARGB_8888_Config);
  return createPlatformDevice(width, height, is_opaque, NULL);
}

SkDevice* PlatformCanvasWin::createPlatformDevice(int width,
                                               int height,
                                               bool is_opaque,
                                               HANDLE shared_section) {
  HDC screen_dc = GetDC(NULL);
  SkDevice* device = BitmapPlatformDeviceWin::create(screen_dc, width, height,
                                                  is_opaque, shared_section);
  ReleaseDC(NULL, screen_dc);
  return device;
}

SkDevice* PlatformCanvasWin::setBitmapDevice(const SkBitmap&) {
  NOTREACHED();
  return NULL;
}

}  // namespace gfx
