#pragma once

#include "StereoSpectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace WallpaperEngine::Audio::Drivers::Recorders {

// Native host 0x140111355 and 0x1401114c3: elapsed is bounded before the
// rate/fade product, then the resulting filter delta is bounded once more.
[[nodiscard]] inline float nativeSpectrumFilterDelta (float elapsedSeconds, float sceneRate,
                                                      float visibilityFade) {
    if (!std::isfinite (elapsedSeconds)) elapsedSeconds = 0.0001f;
    const float elapsed = std::clamp (elapsedSeconds, 0.0001f, 0.25f);
    float delta = (sceneRate * visibilityFade) * elapsed;
    if (!std::isfinite (delta)) delta = 0.0001f;
    return std::clamp (delta, 0.0001f, 0.25f);
}

// Recovered per-scene host filter from wallpaper64.exe v2.8.42
// FUN_140110630:759–1900. Kept separate from the global PulseAudio recorder
// CScene owns one history through SceneSpectrumState. Scene rate and faded
// visibility fade currently enters as a neutral value until its native runtime source
// has been recovered in the Linux scene model.
class NativeSpectrumSmoother {
public:
    [[nodiscard]] StereoSpectrum::Bands update (const StereoSpectrum::Bands& raw,
                                                float elapsedSeconds, float sceneRate,
                                                float visibilityFade) {
        const float delta = nativeSpectrumFilterDelta (elapsedSeconds, sceneRate, visibilityFade);

        std::array<float, 128> current {};
        std::copy (raw.audio64[0].begin (), raw.audio64[0].end (), current.begin ());
        std::copy (raw.audio64[1].begin (), raw.audio64[1].end (), current.begin () + 64);
        float globalPeak = 0.0f;
        for (float value : current) {
            if (std::isfinite (value)) globalPeak = std::max (globalPeak, value);
        }
        if (m_coefficients[0] <= 0.0001f && globalPeak >= 0.0001f) {
            m_coefficients.fill (1.0f);
        }
        const float coefficientStep = std::min (delta, 1.0f);
        for (size_t group = 0; group < m_coefficients.size (); ++group) {
            float peak = 0.0f;
            for (size_t i = group * 8; i < group * 8 + 8; ++i) {
                if (std::isfinite (current[i])) peak = std::max (peak, current[i]);
            }
            const float target = std::max (peak, globalPeak * 0.333f);
            const float difference = target - m_coefficients[group];
            if (std::abs (difference) <= 0.0001f) m_coefficients[group] = target;
            else if (difference > 0.0f)
                m_coefficients[group] += std::min (difference, coefficientStep);
            else
                m_coefficients[group] -= std::min (-difference, coefficientStep) * 0.5f;
        }

        // FUN_140110630 zeroes the scene's raw output and skips the entire
        // history-update loop on silence; histories resume on reactivation.
        if (globalPeak < 0.0001f) return {};
        const float intermediateStep = std::min (delta * 20.0f, 1.0f);
        const float publishedStep = std::min (delta * 40.0f, 1.0f);
#if defined(__SSE__)
        std::array<float, 16> reciprocals {};
        for (size_t group = 0; group < reciprocals.size (); ++group) {
            const float coefficient = std::max (m_coefficients[group], 0.001f);
            // v2.8.42 0x1401122d2 onward broadcasts each coefficient and
            // applies RCPPS once per group before the history vector loop.
            reciprocals[group] = _mm_cvtss_f32 (_mm_rcp_ps (_mm_set1_ps (coefficient)));
        }
#endif
        for (size_t i = 0; i < current.size (); ++i) {
            const float source = std::isfinite (current[i]) ? current[i] : 0.0f;
#if defined(__SSE__)
            const float normalized = source * reciprocals[i / 8];
#else
            // Preserve the portable normalization policy where the native
            // x86 reciprocal estimate is unavailable.
            const float coefficient = std::max (m_coefficients[i / 8], 0.001f);
            const float normalized = source / coefficient;
#endif
            m_intermediate[i] += (normalized - m_intermediate[i]) * intermediateStep;
            const float change = m_intermediate[i] - m_published[i];
            m_published[i] += std::clamp (change, -publishedStep, publishedStep);
        }
        std::array<float, 64> left {};
        std::array<float, 64> right {};
        std::copy_n (m_published.begin (), 64, left.begin ());
        std::copy_n (m_published.begin () + 64, 64, right.begin ());
        return StereoSpectrum::from64 (left, right);
    }

private:
    std::array<float, 16> m_coefficients {};
    std::array<float, 128> m_intermediate {};
    std::array<float, 128> m_published {};
};

} // namespace WallpaperEngine::Audio::Drivers::Recorders
