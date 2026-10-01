#pragma once

#include "WallpaperEngine/Data/Assets/Texture.h"

#include <cmath>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

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

struct TextureAnimationCursor {
    uint32_t frame = 0;
    float subframe = 0.0f;
};

// Both native 14015fdd0 (private controls) and 14015f0d0 (shared texture
// sampling) perform this single transition and discard excess progress.
inline void advanceTextureAnimationCursor (
    std::span<const Data::Assets::FrameSharedPtr> frames, float step, TextureAnimationCursor& cursor) {
    if (frames.empty () || !(step > 0.0f || step < 0.0f)) return;
    const int32_t frame = std::bit_cast<int32_t> (cursor.frame);
    const size_t current = frame >= 0 && static_cast<size_t> (frame) < frames.size ()
        ? static_cast<size_t> (frame) : 0;
    if (!frames[current]) return;
    cursor.subframe += step;
    if (step > 0.0f && cursor.subframe >= frames[current]->frametime) {
        ++cursor.frame;
        cursor.subframe -= frames[current]->frametime;
        if (std::bit_cast<int32_t> (cursor.frame) < 0 || cursor.frame >= frames.size ()) cursor.frame = 0;
        if (frames[cursor.frame]) cursor.subframe = std::min (cursor.subframe, frames[cursor.frame]->frametime);
    } else if (step < 0.0f && cursor.subframe <= 0.0f) {
        --cursor.frame;
        if (std::bit_cast<int32_t> (cursor.frame) < 0) cursor.frame = static_cast<uint32_t> (frames.size () - 1);
        // Native raw reverse seeks can point outside the frame array. Retain
        // the script value without dereferencing an invalid authored frame.
        if (cursor.frame < frames.size () && frames[cursor.frame])
            cursor.subframe = std::max (0.0f, cursor.subframe + frames[cursor.frame]->frametime);
    }
}

// One shared clock belongs to a cached CTexture, not each image/pass. Native
// 14015f0d0 advances only when sampled, once per engine frame. Pure getters
// observe the preceding render's cursor; a private override bypasses sampling.
class SharedTextureAnimation {
public:
    explicit SharedTextureAnimation (std::vector<Data::Assets::FrameSharedPtr> frames)
        : m_frames (std::move (frames)) { }
    [[nodiscard]] const TextureAnimationCursor& cursor () const { return m_cursor; }
    [[nodiscard]] uint32_t getFrame () const { return m_cursor.frame; }
    void sample (float delta, uint32_t frameCounter) {
        if (m_sampledFrame == frameCounter) return;
        m_sampledFrame = frameCounter;
        advanceTextureAnimationCursor (m_frames, delta, m_cursor);
    }

private:
    std::vector<Data::Assets::FrameSharedPtr> m_frames;
    TextureAnimationCursor m_cursor;
    std::optional<uint32_t> m_sampledFrame;
};

// Image controls initially join the texture's shared timeline. Native 2.8.42
// 14020e670 creates this state lazily; 1401fa330–500 detach only when required.
// Frame metadata remains immutable and can be shared by many image instances.
class ImageTextureAnimation {
public:
    explicit ImageTextureAnimation (std::vector<Data::Assets::FrameSharedPtr> frames,
                                    std::shared_ptr<SharedTextureAnimation> shared = {})
        : m_frames (std::move (frames)), m_shared (shared ? std::move (shared)
            : std::make_shared<SharedTextureAnimation> (m_frames)) {
        for (const auto& frame : m_frames)
            if (frame) m_duration += frame->frametime;
    }

    [[nodiscard]] uint32_t frameCount () const { return static_cast<uint32_t> (m_frames.size ()); }
    [[nodiscard]] float duration () const { return m_duration; }
    [[nodiscard]] float rate () const { return m_rate; }
    [[nodiscard]] uint32_t getFrame () const { return m_override ? m_cursor.frame : m_shared->getFrame (); }
    [[nodiscard]] bool isPlaying () const { return !m_override || m_playing; }
    [[nodiscard]] bool hasOverride () const { return m_override; }

    [[nodiscard]] const std::shared_ptr<SharedTextureAnimation>& sharedPlayback () const { return m_shared; }

    void setRate (float rate) {
        m_rate = rate;
        if (rate != 1.0f) detach ();
    }
    void play () { m_playing = true; }
    void pause () { detach (); m_playing = false; }
    void stop () { m_override = true; m_playing = false; m_cursor = {}; }
    void join () { m_override = false; }
    void setFrame (uint32_t frame) {
        m_cursor = {frame, 0.0f};
        if (!m_override) {
            m_override = true;
            m_playing = true;
        }
    }

    // Native 14015fdd0 advances at most one frame per scene tick, including
    // large rates/deltas. Excess progress is clamped to the next duration.
    // Zero delta retains even an out-of-range seek; rendering resolves it.
    void advance (float delta) {
        if (m_override && m_playing) advanceTextureAnimationCursor (m_frames, delta * m_rate, m_cursor);
    }

private:
    void detach () {
        if (!m_override) {
            m_cursor = m_shared->cursor ();
            m_override = true;
        }
    }

    std::vector<Data::Assets::FrameSharedPtr> m_frames;
    std::shared_ptr<SharedTextureAnimation> m_shared;
    float m_duration = 0.0f;
    float m_rate = 1.0f;
    TextureAnimationCursor m_cursor;
    bool m_override = false;
    bool m_playing = true;
};

} // namespace WallpaperEngine::Render
