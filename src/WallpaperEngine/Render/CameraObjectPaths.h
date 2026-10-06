#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "WallpaperEngine/Data/Model/Object.h"

namespace WallpaperEngine::Render {

/** The pose, and the per-path zoom/fov, a camera object's active path produced this frame. */
struct CameraObjectPathFrame {
    glm::vec3 eye;
    glm::vec3 center;
    glm::vec3 up;
    float zoom = 1.0f;
    float fov = 50.0f;
};

/**
 * Runtime queue of one camera object (native state current
 * index, remaining-index vector, last pick). Items are
 * runtime copies of the parsed paths because each owns its own playback time.
 */
struct CameraObjectPathState {
    std::vector<Data::Model::SceneCameraObjectPath> paths;
    int current = -1;
    /** Native fills it with every valid item in file order at load. */
    std::vector<int> queue;
    bool hasLast = false;
    int last = 0;
    /** Current item's zoom/fov as written by the last tick. */
    float zoom = 1.0f;
    float fov = 50.0f;

    CameraObjectPathState () = default;
    explicit CameraObjectPathState (const std::vector<Data::Model::SceneCameraObjectPath>& source) :
        paths (source) {
        for (int index = 0; index < static_cast<int> (paths.size ()); ++index) queue.push_back (index);
    }

    [[nodiscard]] bool hasCurrent () const {
        return current >= 0 && current < static_cast<int> (paths.size ());
    }
};

/** Native: one mt19937 word, multiply-high, rejection below (2^32 - count) % count. */
inline uint32_t nativeCameraQueueDraw (std::mt19937& rng, uint32_t count) {
    const uint32_t threshold = static_cast<uint32_t> (-count) % count;
    for (;;) {
        const uint64_t product = static_cast<uint64_t> (rng ()) * count;
        if (static_cast<uint32_t> (product) >= threshold) return static_cast<uint32_t> (product >> 32);
    }
}

/**
 * Native selection, performed when no path is current or the current
 * single-mode path finished. Sequential walks the item list; random draws from
 * the remaining-index queue (refilled when empty) and skips the previous pick
 * unless it is the only candidate.
 */
inline void selectNextCameraObjectPath (CameraObjectPathState& state, bool sequential, std::mt19937& rng) {
    const int count = static_cast<int> (state.paths.size ());
    if (count <= 0) return;
    if (sequential) {
        // Every parsed item is enabled.
        state.current = (state.current + 1 + count) % count;
        return;
    }
    state.current = -1;
    if (state.queue.empty())
        for (int index = 0; index < count; ++index) state.queue.push_back (index);
    const size_t size = state.queue.size ();
    const size_t start = nativeCameraQueueDraw (rng, static_cast<uint32_t> (size));
    size_t position = start;
    size_t tried = 0;
    while (tried < size) {
        if (!state.hasLast || state.last != state.queue[position]) break;
        position = (position + 1) % size;
        ++tried;
    }
    // When every candidate equals the previous pick the original draw is used.
    if (tried >= size) position = start;
    const int picked = state.queue[position];
    state.queue.erase (state.queue.begin () + static_cast<std::ptrdiff_t> (position));
    state.current = picked;
    state.hasLast = true;
    state.last = picked;
}

/** Euler angles (radians, x/y/z) of the object rotation Rz*Ry*Rx whose local -Z looks from eye at center. */
inline glm::vec3 cameraObjectAnglesForPose (const glm::vec3& eye, const glm::vec3& center, const glm::vec3& up) {
    const glm::vec3 z = glm::normalize (eye - center);
    const glm::vec3 x = glm::normalize (glm::cross (up, z));
    const glm::vec3 y = glm::cross (z, x);
    const glm::mat3 rotation (x, y, z);
    return {
        std::atan2 (rotation[1][2], rotation[2][2]),
        std::atan2 (-rotation[0][2], std::sqrt (rotation[1][2] * rotation[1][2] + rotation[2][2] * rotation[2][2])),
        std::atan2 (rotation[0][1], rotation[0][0]),
    };
}

/**
 * One native object tick: pick a path when needed, advance its timeline by dt
 * and evaluate the pose. Channels without keys keep the supplied defaults.
 * Returns nothing when there are no paths (the native update returns early).
 */
inline std::optional<CameraObjectPathFrame> advanceCameraObjectPaths (
    CameraObjectPathState& state, bool sequential, std::mt19937& rng, float dt, bool orthogonal,
    const CameraObjectPathFrame& defaults
) {
    if (state.paths.empty ()) return std::nullopt;
    bool pick = true;
    if (state.hasCurrent ()) {
        auto& timeline = state.paths[state.current].timeline;
        if (!timeline.finished) pick = false;
        else {
            // Native clears bit 0x40000000 and rewinds before choosing the successor.
            timeline.finished = false;
            timeline.time = 0.0f;
        }
    }
    if (pick) selectNextCameraObjectPath (state, sequential, rng);
    if (!state.hasCurrent ()) return std::nullopt;
    auto& path = state.paths[state.current];
    path.timeline.advance (dt);
    CameraObjectPathFrame frame = defaults;
    frame.zoom = 1.0f;
    frame.fov = 50.0f;
    const auto sampled = [&path] (Data::Model::PropertyAnimation& track, size_t channel) {
        track.time = path.timeline.time;
        return track.sample (channel);
    };
    glm::vec3* const targets[3] = {&frame.center, &frame.eye, &frame.up};
    for (size_t vector = 0; vector < 3; ++vector)
        for (size_t channel = 0; channel < 3; ++channel)
            if (!path.vectors[vector].channels[channel].empty ())
                (*targets[vector])[static_cast<int> (channel)] = sampled (path.vectors[vector], channel);
    // Native writes only the track the scene projection uses (zoom for orthogonal, fov otherwise).
    if (orthogonal) {
        if (!path.zoom.channels[0].empty ()) frame.zoom = sampled (path.zoom, 0);
    } else if (!path.fov.channels[0].empty ()) frame.fov = sampled (path.fov, 0);
    state.zoom = frame.zoom;
    state.fov = frame.fov;
    return frame;
}

} // namespace WallpaperEngine::Render
