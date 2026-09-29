#pragma once

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

} // namespace WallpaperEngine::Render::Wallpapers
