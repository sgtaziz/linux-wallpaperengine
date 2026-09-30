#pragma once

#include "WallpaperEngine/Render/Objects/ParticleCore.h"

namespace WallpaperEngine::Render::Objects::ParticleCore {

struct HsvRandomRange {
    float hueMin { 0.0f };
    float hueMax { 1.0f };
    int hueSteps { 6 };
    float saturationMin { 0.5f };
    float saturationMax { 1.0f };
    float valueMin { 0.5f };
    float valueMax { 1.0f };
};

struct ColorListRandomRange {
    std::vector<glm::vec3> colorsRgb { glm::vec3 (1.0f) };
    glm::vec3 noise { 0.0f };
};

inline uint32_t nativeCrtRandomWord (uint32_t& state) {
    // 2.8.42 1402c97a0: a thread-local MSVCRT stream, independent of the
    // particle MT19937. The native startup seeds it from performance time.
    state = state * 0x343fdu + 0x269ec3u;
    return (state >> 16) & 0x7fffu;
}

inline glm::vec3 nativeInitialHsvToRgb (glm::vec3 hsv) {
    // 1401b8c70 uses fmod, not floor wrapping. In particular a negative
    // HSV-random tint-adjusted hue falls outside every chromatic sector.
    const float chroma = hsv.z * hsv.y;
    const float sector = static_cast<float> (std::fmod (
        static_cast<double> (hsv.x * 6.0f), 6.0));
    const float secondary = static_cast<float> ((1.0 - std::abs (
        std::fmod (static_cast<double> (sector), 2.0) - 1.0)) * chroma);
    glm::vec3 rgb (0.0f);
    if (sector >= 0.0f && sector < 1.0f) rgb = {chroma, secondary, 0.0f};
    else if (sector >= 1.0f && sector < 2.0f) rgb = {secondary, chroma, 0.0f};
    else if (sector >= 2.0f && sector < 3.0f) rgb = {0.0f, chroma, secondary};
    else if (sector >= 3.0f && sector < 4.0f) rgb = {0.0f, secondary, chroma};
    else if (sector >= 4.0f && sector < 5.0f) rgb = {secondary, 0.0f, chroma};
    else if (sector >= 5.0f && sector < 6.0f) rgb = {chroma, 0.0f, secondary};
    return rgb + (hsv.z - chroma);
}

struct NativeHsvRandomRange {
    float hueMin;
    float hueStep;
    int hueMaxIndex;
    glm::vec2 saturation; // minimum, span
    glm::vec2 value;
};

inline NativeHsvRandomRange compileNativeHsvRandomRange (
    HsvRandomRange authored, glm::vec3 tint = glm::vec3 (1.0f), bool tintValid = false) {
    // 1401c5490:7869-7936 / instructions 1401c79d6-1401c7a57. The
    // authored step count is the inclusive maximum index, even for a
    // partial hue interval. Full rotations use count rather than count-1.
    const int steps = std::max (0, authored.hueSteps);
    float hueSpan = 0.0f;
    float denominator = 0.0f;
    if (steps > 1) {
        hueSpan = authored.hueMax - authored.hueMin;
        if (hueSpan == 0.0f) hueSpan = 1.0f;
        denominator = static_cast<float> (steps) - 1.0f;
        if (std::abs (std::fmod (hueSpan, 1.0f)) < 0.0027777778f)
            denominator += 1.0f;
    }
    NativeHsvRandomRange result { authored.hueMin,
        denominator == 0.0f ? 0.0f : hueSpan / denominator, steps,
        {authored.saturationMin, authored.saturationMax - authored.saturationMin},
        {authored.valueMin, authored.valueMax - authored.valueMin} };
    if (tintValid) {
        // Descriptor 13 in 1401d15a0 recenters the S/V interval on tint
        // HSV, then trims only the upper end. Hue remains unwrapped.
        const glm::vec3 hsv = particleRgbToHsv (tint);
        result.hueMin = (hsv.x - (hueSpan * 0.5f + authored.hueMin)) + authored.hueMin;
        const auto recenter = [] (glm::vec2 range, float center) {
            range.x = std::clamp ((center - (range.y * 0.5f + range.x)) + range.x,
                                  0.0f, 1.0f);
            if (range.x + range.y > 1.0f) range.y = 1.0f - range.x;
            return range;
        };
        result.saturation = recenter (result.saturation, hsv.y);
        result.value = recenter (result.value, hsv.z);
    }
    return result;
}

inline glm::vec3 nativeHsvRandomSample (
    std::mt19937& rng, uint32_t crtWord, HsvRandomRange range,
    glm::vec3 tint = glm::vec3 (1.0f), bool tintValid = false) {
    const auto packed = compileNativeHsvRandomRange (range, tint, tintValid);
    const float unit = static_cast<float> (crtWord & 0x7fffu) / 32767.0f;
    const float sampled = unit * (static_cast<float> (packed.hueMaxIndex) + 1.0f);
    // The consumer's CVTTSS2SI returns INT_MIN outside the signed domain;
    // its following lower clamp then selects zero. Avoid an undefined C++
    // float-to-int conversion for an extreme authored step count.
    const int sampledIndex = sampled < 2147483648.0f
        ? static_cast<int> (sampled) : std::numeric_limits<int>::min ();
    const int index = std::clamp (sampledIndex, 0, packed.hueMaxIndex);
    // Opcode 4 always consumes S then V from MT, including zero spans.
    const float saturation = nativeRandomUnit (rng) * packed.saturation.y + packed.saturation.x;
    const float value = nativeRandomUnit (rng) * packed.value.y + packed.value.x;
    return nativeInitialHsvToRgb ({static_cast<float> (index) * packed.hueStep + packed.hueMin,
                                  saturation, value});
}

inline glm::vec3 nativeColorListSample (
    std::mt19937& rng, const ColorListRandomRange& range,
    glm::vec3 tint = glm::vec3 (1.0f), bool tintValid = false) {
    // Empty/invalid colors compile to HSV(0,1,1) (1401c7dc7), while the
    // missing-field default is RGB white (1401ba740).
    const auto colorHsv = [&] (uint32_t index) {
        return range.colorsRgb.empty () ? glm::vec3 (0.0f, 1.0f, 1.0f)
            : particleRgbToHsv (range.colorsRgb[index]);
    };
    const uint32_t count = static_cast<uint32_t> (std::max<size_t> (1, range.colorsRgb.size ()));
    const glm::vec3 selected = colorHsv (nativeImageSampleIndex (rng, count));
    const glm::vec3 minimum = glm::max (selected - range.noise, glm::vec3 (0.0f));
    const glm::vec3 maximum = glm::min (selected + range.noise, glm::vec3 (1.0f));
    glm::vec3 hsv;
    // Opcode 5: bounded selection, then H/S/V. Even a single constant
    // color consumes four MT words (selection rejection can add words).
    for (int axis = 0; axis < 3; ++axis)
        hsv[axis] = nativeRandomUnit (rng) * (maximum[axis] - minimum[axis]) + minimum[axis];
    if (tintValid) hsv += particleRgbToHsv (tint) - colorHsv (0);
    hsv.x -= std::floor (hsv.x);
    hsv.y = std::clamp (hsv.y, 0.0f, 1.0f);
    hsv.z = std::clamp (hsv.z, 0.0f, 1.0f);
    return nativeInitialHsvToRgb (hsv);
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
