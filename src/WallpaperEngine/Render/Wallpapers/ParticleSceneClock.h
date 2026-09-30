#pragma once
#include <cstdint>
#include <optional>

namespace WallpaperEngine::Render::Wallpapers {
// Native 14017fa70:217–225 retains a double sum, publishes its float cast to
// the particle context, and clears both only when that float exceeds 432000.
inline float advanceParticleSceneClock (double& accumulator, float sceneStep) {
    accumulator += static_cast<double> (sceneStep);
    const float published = static_cast<float> (accumulator);
    if (published > 432000.0f) {
        accumulator = 0.0;
        return 0.0f;
    }
    return published;
}

// Windows 2.8.42 14017fa70:215-216: root+0x150 retains the previous
// outer scene duration, while root+0x14c receives this frame's duration.
// This clock belongs to the scene, never to an individual scaled node tick.
struct ParticleSceneFrameDurations {
    float current { 0.0f };
    // Native constructor 14017c6d0:68 seeds root+0x150 to float 1/30.
    float previous { 1.0f / 30.0f };
    std::optional<uint32_t> frame;
    void publish (float duration, uint32_t frameId) {
        if (frame && *frame == frameId) return;
        previous = current;
        current = duration;
        frame = frameId;
    }
};
}
