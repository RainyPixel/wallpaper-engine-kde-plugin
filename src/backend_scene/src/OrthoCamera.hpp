#pragma once

#include "Core/Literals.hpp"
#include "Type.hpp"

#include <cmath>
#include <limits>

namespace wallpaper
{

// Scene general.zoom. Non-positive values (and NaN) act as 1.
inline float NormalizedCameraZoom(float zoom) { return zoom > 0.0f ? zoom : 1.0f; }

// Orthographic axis after zoom.
// zoom normalizes to 1, and that case keeps the axis so fill-mode fractions survive.
// Any other zoom is round(axis / zoom), clamped to [1, i32 max]. lround is undefined
// when the result does not fit in long, so out-of-range inputs never reach it.
inline double ZoomedExtent(double axis, float zoom) {
    const float used = NormalizedCameraZoom(zoom);
    if (used == 1.0f) return axis < 1.0 ? 1.0 : axis;

    const double raw = axis / static_cast<double>(used);
    if (! std::isfinite(raw) || raw < 0.5) return 1.0;
    const double limit = static_cast<double>(std::numeric_limits<i32>::max()) - 0.5;
    if (raw >= limit) return static_cast<double>(std::numeric_limits<i32>::max());
    return static_cast<double>(std::lround(raw));
}

// Parsed global ortho camera. The center is the unscaled ortho midpoint: zoom
// crops evenly around it and is not applied to the camera position.
struct OrthoCameraExtent {
    i32   width { 1 };
    i32   height { 1 };
    float center_x { 0.0f };
    float center_y { 0.0f };
};

inline OrthoCameraExtent OrthoCameraForZoom(i32 ortho_w, i32 ortho_h, float zoom) {
    return OrthoCameraExtent {
        static_cast<i32>(ZoomedExtent(static_cast<double>(ortho_w), zoom)),
        static_cast<i32>(ZoomedExtent(static_cast<double>(ortho_h), zoom)),
        static_cast<float>(ortho_w) / 2.0f,
        static_cast<float>(ortho_h) / 2.0f,
    };
}

// Fill mode rebuilds the global camera from the unscaled scene ortho and the
// view size. Zoom is applied to the orthographic result only. perspective_height
// is the pre-zoom height the perspective FOV is computed from.
struct FilledCameraExtents {
    double ortho_width { 1.0 };
    double ortho_height { 1.0 };
    double perspective_aspect { 1.0 };
    double perspective_height { 1.0 };
};

inline FilledCameraExtents CameraExtentsForFill(double scene_w, double scene_h, double view_w,
                                                double view_h, FillMode fill, float zoom) {
    const double fbo_aspect   = view_w / view_h;
    const double scene_aspect = scene_w / scene_h;
    double       cam_w        = scene_w;
    double       cam_h        = scene_h;
    double       per_aspect   = scene_aspect;
    switch (fill) {
    case FillMode::STRETCH:
        cam_w      = scene_w;
        cam_h      = scene_h;
        per_aspect = scene_aspect;
        break;
    case FillMode::ASPECTFIT:
        if (fbo_aspect < scene_aspect) {
            cam_w = scene_w;
            cam_h = scene_w / fbo_aspect;
        } else {
            cam_w = scene_h * fbo_aspect;
            cam_h = scene_h;
        }
        per_aspect = fbo_aspect;
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fbo_aspect > scene_aspect) {
            cam_w = scene_w;
            cam_h = scene_w / fbo_aspect;
        } else {
            cam_w = scene_h * fbo_aspect;
            cam_h = scene_h;
        }
        per_aspect = fbo_aspect;
        break;
    }
    return FilledCameraExtents {
        ZoomedExtent(cam_w, zoom),
        ZoomedExtent(cam_h, zoom),
        per_aspect,
        cam_h,
    };
}

} // namespace wallpaper
