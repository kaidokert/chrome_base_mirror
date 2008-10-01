// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/gfx/platform_device_mac.h"

#include "base/logging.h"
#include "base/gfx/skia_utils_mac.h"
#include "SkMatrix.h"
#include "SkPath.h"
#include "SkUtils.h"

namespace gfx {

namespace {

// Constrains position and size to fit within available_size.
bool constrain(int available_size, int* position, int *size) {
  if (*position < 0) {
    *size += *position;
    *position = 0;
  }
  if (*size > 0 && *position < available_size) {
    int overflow = (*position + *size) - available_size;
    if (overflow > 0) {
      *size -= overflow;
    }
    return true;
  }
  return false;
}

// Sets the opacity of the specified value to 0xFF.
void makeOpaqueAlphaAdjuster(uint32_t* pixel) {
  *pixel |= 0xFF000000;
}

} // namespace

PlatformDeviceMac::PlatformDeviceMac(const SkBitmap& bitmap)
    : SkDevice(bitmap) {
}

void PlatformDeviceMac::makeOpaque(int x, int y, int width, int height) {
  processPixels(x, y, width, height, makeOpaqueAlphaAdjuster);
}

// Set up the CGContextRef for peaceful coexistence with Skia
void PlatformDeviceMac::InitializeCGContext(CGContextRef context) {
  // CG defaults to the same settings as Skia
}

// static
void PlatformDeviceMac::LoadPathToCGContext(CGContextRef context,
                                            const SkPath& path) {
  // instead of a persistent attribute of the context, CG specifies the fill
  // type per call, so we just have to load up the geometry.
  CGContextBeginPath(context);

  SkPoint points[4] = { {0, 0}, {0, 0}, {0, 0}, {0, 0} };
  SkPath::Iter iter(path, false);
  for (SkPath::Verb verb = iter.next(points); verb != SkPath::kDone_Verb;
       verb = iter.next(points)) {
    switch (verb) {
      case SkPath::kMove_Verb: {  // iter.next returns 1 point
        CGContextMoveToPoint(context, points[0].fX, points[0].fY);
        break;
      }
      case SkPath::kLine_Verb: {  // iter.next returns 2 points
        CGContextAddLineToPoint(context, points[1].fX, points[1].fY);
        break;
      }
      case SkPath::kQuad_Verb: {  // iter.next returns 3 points
        CGContextAddQuadCurveToPoint(context, points[1].fX, points[1].fY,
                                     points[2].fX, points[2].fY);
        break;
      }
      case SkPath::kCubic_Verb: {  // iter.next returns 4 points
        CGContextAddCurveToPoint(context, points[1].fX, points[1].fY,
                                 points[2].fX, points[2].fY,
                                 points[3].fX, points[3].fY);
        break;
      }
      case SkPath::kClose_Verb: {  // iter.next returns 1 point (the last point)
        break;
      }
      case SkPath::kDone_Verb:  // iter.next returns 0 points
      default: {
        NOTREACHED();
        break;
      }
    }
  }
  CGContextClosePath(context);
}

// static
void PlatformDeviceMac::LoadTransformToCGContext(CGContextRef context,
                                                 const SkMatrix& matrix) {
  // CoreGraphics can concatenate transforms, but not reset the current one.
  // So in order to get the required behavior here, we need to first make
  // the current transformation matrix identity and only then load the new one.
  
  // Reset matrix to identity.
  CGAffineTransform orig_cg_matrix = CGContextGetCTM(context);
  CGAffineTransform orig_cg_matrix_inv = CGAffineTransformInvert(orig_cg_matrix);
  CGContextConcatCTM(context, orig_cg_matrix_inv);
  
  // assert that we have indeed returned to the identity Matrix.
  DCHECK(CGAffineTransformIsIdentity(CGContextGetCTM(context)));
  
  // Convert xform to CG-land.
  // Our coordinate system is flipped to match WebKit's so we need to modify
  // the xform to match that.
  SkMatrix transformed_matrix = matrix;
  SkScalar sy = matrix.getScaleY() * (SkScalar)-1;
  transformed_matrix.setScaleY(sy);
  size_t height = CGBitmapContextGetHeight(context);
  SkScalar ty = -matrix.getTranslateY(); // y axis is flipped.
  transformed_matrix.setTranslateY(ty + (SkScalar)height);
  
  CGAffineTransform cg_matrix = SkMatrixToCGAffineTransform(transformed_matrix);
  
  // Load final transform into context.
  CGContextConcatCTM(context, cg_matrix);
}

// static
void PlatformDeviceMac::LoadClippingRegionToCGContext(
         CGContextRef context,
         const SkRegion& region,
         const SkMatrix& transformation) {
  if (region.isEmpty()) {
    // region can be empty, in which case everything will be clipped.
    SkRect rect;
    rect.setEmpty();
    CGContextClipToRect(context, SkRectToCGRect(rect));
  } else if (region.isRect()) {
    // Do the transformation.
    SkRect rect;
    rect.set(region.getBounds());
    transformation.mapRect(&rect);
    SkIRect irect;
    rect.round(&irect);
    CGContextClipToRect(context, SkIRectToCGRect(irect));
  } else {
    // It is complex.
    SkPath path;
    region.getBoundaryPath(&path);
    // Clip. Note that windows clipping regions are not affected by the
    // transform so apply it manually.
    path.transform(transformation);
    // TODO(playmobil): Implement.
    NOTREACHED();
    // LoadPathToDC(context, path);
    // hrgn = PathToRegion(context);
  }
}
  
}  // namespace gfx

