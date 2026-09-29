#pragma once

#include "NativeSpectrumSmoother.h"
#include <chrono>
#include <cstdint>

namespace WallpaperEngine::Audio::Drivers::Recorders {

// The native wproperties rate is a percentage with a 10% lower floor.
inline float sceneSpectrumRate (float authoredPercent) {
    return std::isfinite (authoredPercent) ? std::max (authoredPercent / 100.0f, 0.1f) : 1.0f;
}

// One instance belongs to each scene. Its published arrays keep their addresses
// for shader uniforms throughout that scene's lifetime.
class SceneSpectrumState {
public:
    [[nodiscard]] const StereoSpectrum::Bands& bands () const { return m_published; }

    bool advance (const StereoSpectrum::Bands& raw, uint32_t frameId,
                  std::chrono::steady_clock::time_point now,
                  float sceneRate, float visibilityFade) {
        if (m_frameInitialized && m_frameId == frameId) return false;
        const float elapsedSeconds = m_frameInitialized
            ? std::chrono::duration<float> (now - m_lastUpdate).count ()
            : 0.0001f;
        m_frameInitialized = true;
        m_frameId = frameId;
        m_lastUpdate = now;
        m_published = m_smoother.update (raw, elapsedSeconds, sceneRate, visibilityFade);
        return true;
    }

private:
    NativeSpectrumSmoother m_smoother;
    StereoSpectrum::Bands m_published {};
    uint32_t m_frameId = 0;
    std::chrono::steady_clock::time_point m_lastUpdate {};
    bool m_frameInitialized = false;
};

} // namespace WallpaperEngine::Audio::Drivers::Recorders
