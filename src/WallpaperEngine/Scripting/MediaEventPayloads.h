#pragma once

#include "WallpaperEngine/Media/MediaSource.h"

#include <array>
#include <functional>
#include <glm/vec3.hpp>

extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Scripting {

struct MediaPalette {
    glm::vec3 primary;
    glm::vec3 secondary;
    glm::vec3 tertiary;
    glm::vec3 text;
    glm::vec3 highContrast;
};

// Uses the same immutable decoded artwork snapshot consumed by the album
// textures. This is a bounded Linux extraction policy, not a recovered
// Windows five-color quantizer.
MediaPalette paletteFromArtwork (const Media::MediaArtwork* artwork);

// Owns the four event payloads for the duration of a callback dispatch.
class MediaEventPayloads {
public:
    MediaEventPayloads (JSContext* context, const Media::MediaSource::MediaInfo& media,
                        const MediaPalette& palette,
                        const std::function<JSValue (const glm::vec3&)>& vectorValue);
    ~MediaEventPayloads ();
    MediaEventPayloads (const MediaEventPayloads&) = delete;
    MediaEventPayloads& operator= (const MediaEventPayloads&) = delete;

    JSValue properties () const { return m_events[0]; }
    JSValue playback () const { return m_events[1]; }
    JSValue timeline () const { return m_events[2]; }
    JSValue thumbnail () const { return m_events[3]; }

private:
    JSContext* m_context;
    std::array<JSValue, 4> m_events;
};

} // namespace WallpaperEngine::Scripting
