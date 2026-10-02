#pragma once

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include <optional>

namespace WallpaperEngine::Render {

struct CameraPathCursor {
    size_t path = 0;
    size_t sample = 0;
    float time = 0;
};

inline float nativeCameraPathHermite (float left, float right, float t) {
    const float squared = t * t;
    const float cubed = squared * t;
    const float rightTangent = cubed - squared;
    const float rightWeight = squared * 3.0f - (cubed + cubed);
    const float leftWeight = ((cubed + cubed) - squared * 3.0f) + 1.0f;
    const float leftTangent = (cubed - (squared + squared)) + t;
    const float halfDelta = (right - left) * 0.5f;
    // Original 1891a0 uses these self-subtractions and scalar addition order.
    return (((left - left) * 0.5f + halfDelta) * leftTangent + leftWeight * left)
        + ((right - right) * 0.5f + halfDelta) * rightTangent + rightWeight * right;
}

inline std::optional<Data::Model::CameraPathSample> advanceNativeCameraPath (
    const std::vector<Data::Model::CameraPath>& paths, CameraPathCursor& cursor, float dt
) {
    // The native root's first empty segment admits the stored root pose.
    if (paths.empty () || paths.front ().samples.empty ()) return std::nullopt;
    if (cursor.path >= paths.size ()) cursor = {};
    const auto& path = paths[cursor.path];
    // Malformed later segments must not read beyond their storage.
    if (path.samples.empty () || cursor.sample >= path.samples.size ()) return std::nullopt;
    const auto& left = path.samples[cursor.sample];
    const size_t next = cursor.sample + 1;
    float boundary;
    auto result = left;
    if (cursor.time < left.timestamp) {
        boundary = left.timestamp + (next < path.samples.size () ? path.samples[next].timestamp : 0);
    } else if (next >= path.samples.size ()) {
        // Direct PE 1899d4: this is a subtraction, not the absolute duration.
        boundary = path.duration - left.timestamp;
    } else {
        const auto& right = path.samples[next];
        boundary = right.timestamp;
        const float t = (cursor.time - left.timestamp) / (right.timestamp - left.timestamp);
        for (int axis = 0; axis < 3; ++axis) {
            result.eye[axis] = nativeCameraPathHermite (left.eye[axis], right.eye[axis], t);
            result.center[axis] = nativeCameraPathHermite (left.center[axis], right.center[axis], t);
            result.up[axis] = nativeCameraPathHermite (left.up[axis], right.up[axis], t);
        }
        result.zoom = nativeCameraPathHermite (left.zoom, right.zoom, t);
    }
    // Sample BEFORE dt; advance at most one entry, strictly beyond boundary.
    cursor.time += dt;
    if (boundary < cursor.time) {
        if (next >= path.samples.size () || path.duration <= path.samples[next].timestamp) {
            cursor.path = (cursor.path + 1) % paths.size ();
            cursor.sample = 0;
            cursor.time = 0;
        } else cursor.sample = next;
    }
    return result;
}

} // namespace WallpaperEngine::Render
