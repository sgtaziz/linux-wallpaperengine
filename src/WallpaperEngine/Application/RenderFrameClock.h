#pragma once

namespace WallpaperEngine::Application {

// The fullscreen resume branch has no render dispatch. Sampling both clock
// endpoints there prevents its next active frame from consuming paused time.
inline void resumeRenderFrameClock (float now, float& current, float& previous) {
    current = now;
    previous = now;
}

inline void sampleRenderFrameClock (float now, float& current, float& previous) {
    previous = current;
    current = now;
}

} // namespace WallpaperEngine::Application
