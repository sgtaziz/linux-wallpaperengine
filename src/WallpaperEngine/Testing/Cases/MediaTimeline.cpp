#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Media/MediaSource.h"
#include "WallpaperEngine/Media/MediaTimelineUnits.h"
#include "WallpaperEngine/Scripting/MediaEventPayloads.h"

#include <chrono>

using namespace WallpaperEngine::Media;

namespace {
class CountedMediaSource : public MediaSource {
public:
    CountedMediaSource () : MediaSource (std::chrono::seconds (10)) { }
    int polls = 0;
private:
    void performUpdate () override { ++polls; }
};
class SyntheticMediaSource : public MediaSource {
public:
    SyntheticMediaSource () : MediaSource (std::chrono::seconds (1)) { }
    void emit () {
        m_mediaInfo.playbackState = Playing;
        m_mediaInfo.title = "Synthetic";
        m_mediaInfo.artist = "Test Artist";
        m_mediaInfo.album = "Test Album";
        m_mediaInfo.duration = mprisMicrosecondsToSeconds (90000000);
        m_mediaInfo.position = mprisMicrosecondsToSeconds (12345678);
        m_mediaInfo.url = "file:///tmp/cover.png";
        m_mediaInfo.thumbnailAvailable = true;
        fireMetadataListeners ();
    }
private:
    void performUpdate () override { }
};
}

TEST_CASE ("MPRIS timeline microseconds become SceneScript seconds", "[script][media]") {
    REQUIRE (mprisMicrosecondsToSeconds (12345678) == 12.345678);
    REQUIRE (mprisMicrosecondsToSeconds (-250000) == -0.25);
    REQUIRE (mprisMicrosecondsToSeconds (0) == 0);
}

TEST_CASE ("Media polling respects its interval", "[script][media]") {
    CountedMediaSource source;
    source.update ();
    source.update ();
    REQUIRE (source.polls == 1);
}

TEST_CASE ("Synthetic media source delivers typed callback payloads in seconds", "[script][media]") {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    JSValue global = JS_GetGlobalObject (context);
    const char* definitions = R"(
        globalThis.events = [];
        globalThis.mediaPropertiesChanged = e => events.push('p:' + e.title + ':' + e.artist);
        globalThis.mediaPlaybackChanged = e => events.push('b:' + e.state);
        globalThis.mediaTimelineChanged = e => events.push('t:' + e.position.toFixed(6) + ':' + e.duration);
        globalThis.mediaThumbnailChanged = e => events.push('h:' + e.hasThumbnail + ':' +
            e.primaryColor.x + ':' + e.textColor.y);
    )";
    JSValue setup = JS_Eval (context, definitions, std::char_traits<char>::length (definitions),
                             "<media-callback-test>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (setup));
    JS_FreeValue (context, setup);
    SyntheticMediaSource source;
    auto unsubscribe = source.addMetadataListener ([&] (const MediaSource::MediaInfo& media) {
        const WallpaperEngine::Scripting::MediaPalette palette {
            .primary = {0.25f, 0.5f, 0.75f}, .secondary = {0, 0, 0},
            .tertiary = {0, 0, 0}, .text = {1, 0.75f, 0},
            .highContrast = {1, 1, 1}};
        WallpaperEngine::Scripting::MediaEventPayloads payloads (
            context, media, palette, [context] (const glm::vec3& vector) {
                JSValue result = JS_NewObject (context);
                JS_SetPropertyStr (context, result, "x", JS_NewFloat64 (context, vector.x));
                JS_SetPropertyStr (context, result, "y", JS_NewFloat64 (context, vector.y));
                JS_SetPropertyStr (context, result, "z", JS_NewFloat64 (context, vector.z));
                return result;
            });
        const char* names[] = {"mediaPropertiesChanged", "mediaPlaybackChanged",
                               "mediaTimelineChanged", "mediaThumbnailChanged"};
        JSValue values[] = {payloads.properties (), payloads.playback (),
                                  payloads.timeline (), payloads.thumbnail ()};
        for (int i = 0; i < 4; ++i) {
            JSValue function = JS_GetPropertyStr (context, global, names[i]);
            JSValue result = JS_Call (context, function, global, 1, &values[i]);
            REQUIRE_FALSE (JS_IsException (result));
            JS_FreeValue (context, result);
            JS_FreeValue (context, function);
        }
    });
    source.emit ();
    JSValue encoded = JS_Eval (context, "events.join('|')", 16, "<media-events>", JS_EVAL_TYPE_GLOBAL);
    REQUIRE_FALSE (JS_IsException (encoded));
    const char* chars = JS_ToCString (context, encoded);
    REQUIRE (chars != nullptr);
    REQUIRE (std::string (chars) == "p:Synthetic:Test Artist|b:1|t:12.345678:90|h:true:0.25:0.75");
    JS_FreeCString (context, chars);
    JS_FreeValue (context, encoded);
    unsubscribe ();
    JS_FreeValue (context, global);
    JS_FreeContext (context);
    JS_FreeRuntime (runtime);
}
