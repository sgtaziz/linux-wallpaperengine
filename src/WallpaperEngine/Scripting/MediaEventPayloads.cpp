#include "MediaEventPayloads.h"
#include "WallpaperEngine/Media/MediaArtwork.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace WallpaperEngine::Scripting;

MediaPalette WallpaperEngine::Scripting::paletteFromArtwork (const Media::MediaArtwork* artwork) {
    MediaPalette result {};
    if (!artwork || artwork->width <= 0 || artwork->height <= 0
        || static_cast<std::uint64_t> (artwork->width) * artwork->height * 4
            != artwork->rgba.size ()) return result;
    struct Bin { std::uint64_t weight = 0, red = 0, green = 0, blue = 0; };
    std::array<Bin, 4096> bins {};
    for (std::size_t i = 0; i < artwork->rgba.size (); i += 4) {
        const unsigned alpha = artwork->rgba[i + 3];
        if (alpha < 16) continue;
        const unsigned red = artwork->rgba[i], green = artwork->rgba[i + 1], blue = artwork->rgba[i + 2];
        const unsigned index = ((red >> 4) << 8) | ((green >> 4) << 4) | (blue >> 4);
        auto& bin = bins[index];
        bin.weight += alpha;
        bin.red += red * alpha;
        bin.green += green * alpha;
        bin.blue += blue * alpha;
    }
    std::vector<unsigned> ranked;
    ranked.reserve (bins.size ());
    for (unsigned i = 0; i < bins.size (); ++i)
        if (bins[i].weight) ranked.push_back (i);
    if (ranked.empty ()) return result;
    std::sort (ranked.begin (), ranked.end (), [&bins] (unsigned a, unsigned b) {
        if (bins[a].weight != bins[b].weight) return bins[a].weight > bins[b].weight;
        return a < b;
    });
    auto color = [&bins] (unsigned index) {
        const auto& bin = bins[index];
        return glm::vec3 (static_cast<float> (bin.red) / bin.weight / 255.0f,
                          static_cast<float> (bin.green) / bin.weight / 255.0f,
                          static_cast<float> (bin.blue) / bin.weight / 255.0f);
    };
    result.primary = color (ranked[0]);
    result.secondary = color (ranked[std::min<std::size_t> (1, ranked.size () - 1)]);
    result.tertiary = color (ranked[std::min<std::size_t> (2, ranked.size () - 1)]);
    const auto linear = [] (float value) {
        return value <= 0.04045f ? value / 12.92f : std::pow ((value + 0.055f) / 1.055f, 2.4f);
    };
    const float luminance = 0.2126f * linear (result.primary.r)
        + 0.7152f * linear (result.primary.g) + 0.0722f * linear (result.primary.b);
    result.text = luminance > 0.179f ? glm::vec3 (0.0f) : glm::vec3 (1.0f);
    result.highContrast = result.text;
    return result;
}

MediaEventPayloads::MediaEventPayloads (
    JSContext* context, const Media::MediaSource::MediaInfo& media,
    const MediaPalette& palette, const std::function<JSValue (const glm::vec3&)>& vectorValue) :
    m_context (context), m_events {JS_NewObject (context), JS_NewObject (context),
                                   JS_NewObject (context), JS_NewObject (context)} {
    JS_SetPropertyStr (context, m_events[0], "title", JS_NewString (context, media.title.c_str ()));
    JS_SetPropertyStr (context, m_events[0], "artist", JS_NewString (context, media.artist.c_str ()));
    JS_SetPropertyStr (context, m_events[0], "albumTitle", JS_NewString (context, media.album.c_str ()));
    JS_SetPropertyStr (context, m_events[1], "state", JS_NewInt32 (context, media.playbackState));
    JS_SetPropertyStr (context, m_events[2], "position", JS_NewFloat64 (context, media.position));
    JS_SetPropertyStr (context, m_events[2], "duration", JS_NewFloat64 (context, media.duration));
    JS_SetPropertyStr (context, m_events[3], "hasThumbnail", JS_NewBool (context, media.thumbnailAvailable));
    JS_SetPropertyStr (context, m_events[3], "primaryColor", vectorValue (palette.primary));
    JS_SetPropertyStr (context, m_events[3], "secondaryColor", vectorValue (palette.secondary));
    JS_SetPropertyStr (context, m_events[3], "tertiaryColor", vectorValue (palette.tertiary));
    JS_SetPropertyStr (context, m_events[3], "textColor", vectorValue (palette.text));
    JS_SetPropertyStr (context, m_events[3], "highContrastColor", vectorValue (palette.highContrast));
}

MediaEventPayloads::~MediaEventPayloads () {
    for (JSValue event : m_events) JS_FreeValue (m_context, event);
}
