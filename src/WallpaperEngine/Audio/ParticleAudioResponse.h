#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace WallpaperEngine::Audio {

struct ParticleAudioSettings {
    int mode = 0;
    float lowerBound = 0.8f;
    float upperBound = 1.0f;
    float exponent = 2.0f;
    int firstBand = 0;
    int lastBand = 1;
};

// wallpaper64.exe v2.8.42 FUN_14022a8a0: modes 1/2/3 select the
// left/right/stereo-mean peak over inclusive bins of the 16-band spectrum.
inline float particleAudioResponse (std::span<const float, 16> left,
                                    std::span<const float, 16> right,
                                    const ParticleAudioSettings& settings) {
    const auto clampNativeBand = [] (int band) {
        return static_cast<int> (std::min (static_cast<unsigned> (band), 15u));
    };
    const int first = std::min (clampNativeBand (settings.firstBand), clampNativeBand (settings.lastBand));
    const int last = std::max (clampNativeBand (settings.firstBand), clampNativeBand (settings.lastBand));
    float peak = 0.0f;
    for (int bin = first; bin <= last; ++bin) {
        float value = 0.0f;
        switch (settings.mode) {
            case 1: value = left[bin]; break;
            case 2: value = right[bin]; break;
            case 3: value = left[bin] + right[bin]; break;
            default: break;
        }
        if (std::isfinite (value)) peak = std::max (peak, value);
    }
    if (settings.mode == 3) peak *= 0.5f;
    const float span = settings.upperBound - settings.lowerBound;
    if (!std::isfinite (span)) return 0.0f;
    const float normalized = span == 0.0f
        ? (peak >= settings.lowerBound ? 1.0f : 0.0f)
        : std::clamp ((peak - settings.lowerBound) / span, 0.0f, 1.0f);
    const float smooth = normalized * normalized * (3.0f - 2.0f * normalized);
    const float response = std::pow (smooth, settings.exponent);
    // Native COMISS treats a NaN or infinity pow result as above the upper
    // bound and returns one. Its unusual negative-exponent corner follows.
    if (!std::isfinite (response)) return 1.0f;
    return std::clamp (response, 0.0f, 1.0f);
}

} // namespace WallpaperEngine::Audio
