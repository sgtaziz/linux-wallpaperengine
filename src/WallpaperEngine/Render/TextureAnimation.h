#pragma once

#include "WallpaperEngine/Data/Assets/Texture.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace WallpaperEngine::Render {

// Native 2.8.42 particle atlas setup (1401d2340) blends sequence frames by
// default, but clears the blend combo for authored flag 2 or random-frame
// mode. The caller applies this only to a same-page atlas shader pass.
inline bool particleAtlasBlendEnabled (uint32_t flags, std::string_view animationMode) {
    return (flags & 2u) == 0 && animationMode != "randomframe";
}

struct FramePhase {
    size_t ordinal;
    double fraction;
};

// Native particle sequences complete the authored multiplier's number of
// cycles over one particle lifetime, independent of TEXS frame durations.
inline double particleSequenceCycles (double age, double lifetime, double multiplier) {
    const double lifetimePosition = lifetime > 0.0 ? age / lifetime : 1.0;
    // Native leaves an explicitly authored zero at frame zero. Missing/null
    // fields are given the separate default of one by ObjectParser.
    return lifetimePosition * (multiplier >= 0.0 ? multiplier : 1.0);
}

/** Returns the frame ordinal and progress within its authored duration. */
inline std::optional<FramePhase> framePhaseAtTime (
    std::span<const Data::Assets::FrameSharedPtr> frames, double time) {
    if (frames.empty ())
        return std::nullopt;
    double duration = 0.0;
    for (const auto& frame : frames) {
        if (!frame || !std::isfinite (frame->frametime) || frame->frametime <= 0.0f)
            return std::nullopt;
        duration += frame->frametime;
    }
    if (!std::isfinite (duration) || duration <= 0.0 || !std::isfinite (time))
        return std::nullopt;

    double position = std::fmod (time, duration);
    if (position < 0.0)
        position += duration;
    for (size_t ordinal = 0; ordinal < frames.size (); ++ordinal) {
        const double frameDuration = frames[ordinal]->frametime;
        if (position <= frameDuration)
            return FramePhase {ordinal, position / frameDuration};
        position -= frameDuration;
    }
    return FramePhase {frames.size () - 1, 1.0};
}

/** Selects a texture's authored frame using its own frame durations. */
inline const Data::Assets::Frame* frameAtTime (
    std::span<const Data::Assets::FrameSharedPtr> frames, double time) {
    const auto phase = framePhaseAtTime (frames, time);
    return phase ? frames[phase->ordinal].get () : nullptr;
}

} // namespace WallpaperEngine::Render
