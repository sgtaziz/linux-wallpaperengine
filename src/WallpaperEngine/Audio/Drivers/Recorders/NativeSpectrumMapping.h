#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace WallpaperEngine::Audio::Drivers::Recorders {

struct NativeSpectrumInput {
    float r;
    float i;
};

// wallpaper64.exe v2.8.42, 1400d02b0.c:850–955. This is the complex
// transform input, not an ordinary real-audio FFT input. In particular,
// sample -1 intentionally yields a nonfinite imaginary component.
inline NativeSpectrumInput nativeSpectrumInput (float sample) {
    const float real = sample * 127.0f + 127.0f;
    return { real, 1.0f / real };
}

// 1400cf120.c:255–266. The installed native default duration factor is 30.
inline unsigned nativeSpectrumFftLength (unsigned sampleRate, float durationFactor = 30.0f) {
    const float rateFactor = std::max (static_cast<float> (sampleRate) / 44100.0f, 1.0f);
    return static_cast<unsigned> (rateFactor * 64.0f * durationFactor);
}

// 0x1400d1491–0x1400d14a0. The producer fills only this prefix of the
// allocated transform input. The remaining complex cells retain the silence
// value installed at 0x1400d1410–0x1400d143f. Preserve the three float32
// operations before truncation rather than simplifying to an integer ratio.
inline unsigned nativeSpectrumCaptureLength (unsigned fftLength) {
    const float fraction = 10.0f / 30.0f;
    const float omitted = fraction * static_cast<float> (fftLength);
    return static_cast<unsigned> (static_cast<float> (fftLength) - omitted);
}

// wallpaper64.exe v2.8.42, 0x1400d1c7a–0x1400d1ca9. The ratio-one
// endpoint maps to zero, although the native loop excludes that endpoint.
inline unsigned nativeSpectrumMappedBand (unsigned bin, unsigned binCount,
                                          float exponent = 0.25f) {
    if (binCount < 2 || bin == 0 || bin > binCount) return 0;
    const float ratio = static_cast<float> (bin - 1) / static_cast<float> (binCount - 1);
    return static_cast<unsigned> (std::pow (ratio, exponent) * 64.0f) % 64;
}

// The caller must preserve the previous output band across consecutive bins.
inline unsigned nativeSpectrumBand (unsigned previous, unsigned bin, unsigned binCount,
                                    float exponent = 0.25f) {
    return std::min (previous + 1, nativeSpectrumMappedBand (bin, binCount, exponent));
}

// wallpaper64.exe v2.8.42, 0x1400d1d3f–0x1400d1d5d: first multiply gain
// by 0.001f, divide the bin count by half the FFT length separately, then
// multiply the two rounded float32 values.
inline float nativeSpectrumScale (float gain, unsigned binCount, unsigned fftLength) {
    const float scaledGain = gain * 0.001f;
    const float binRatio = static_cast<float> (binCount)
        / (static_cast<float> (fftLength) * 0.5f);
    return scaledGain * binRatio;
}

// Isolated v2.8.42 producer arithmetic after its complex transform. The
// transform's unusual input preprocessing is not represented here.
template <typename ComplexValue>
[[nodiscard]] std::array<float, 64> nativeSpectrumReduce (
    std::span<const ComplexValue> transformed, unsigned binCount, unsigned fftLength,
    float exponent = 0.25f, float coefficient = 0.501f, float gain = 1.0f) {
    std::array<float, 64> bands {};
    if (binCount < 2 || transformed.size () < binCount || fftLength == 0) return bands;
    unsigned band = 0;
    constexpr float pi = 3.141592741f;
    for (unsigned bin = 1; bin < binCount; ++bin) {
        const float real = transformed[bin].r;
        const float imaginary = transformed[bin].i;
        float power = real * real + imaginary * imaginary;
        if (!std::isfinite (power)) power = 0.0f;
        band = nativeSpectrumBand (band, bin, binCount, exponent);
        const float ratio = static_cast<float> (bin - 1) / static_cast<float> (binCount - 1);
        const float weighted = power * (coefficient - std::cos (pi * ratio) * (1.0f - coefficient));
        if (weighted >= 0.0f) bands[band] = std::max (bands[band], std::sqrt (weighted));
        // The default coefficient keeps this branch nonnegative. Native's
        // separate negative-weight sqrt path remains to be typed.
    }
    const float scale = nativeSpectrumScale (gain, binCount, fftLength);
    for (float& value : bands) value *= scale;
    return bands;
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders
