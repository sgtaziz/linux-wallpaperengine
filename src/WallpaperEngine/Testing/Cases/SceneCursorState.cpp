#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Wallpapers/SceneCursorState.h"

#include <string>
#include <vector>

using WallpaperEngine::Render::Wallpapers::SceneCursorState;

TEST_CASE ("Scene cursor overlap delivers native top then bottom button events",
           "[scene][cursor][solid]") {
    SceneCursorState state;
    std::vector<std::string> events;
    auto frame = [&] (bool moved, bool down, bool block) {
        state.beginFrame (moved, down);
        for (const int layer : {2, 1}) {
            state.visit (layer, true, [&] (const char* event) {
                events.push_back (std::to_string (layer) + ":" + event);
            });
            if (block) break;
        }
        state.finishFrame ();
    };
    // Actual native overlap gate: both layers enter, then receive Down;
    // releasing emits each layer's Up immediately followed by its Click.
    frame (false, false, false);
    frame (false, true, false);
    frame (false, false, false);
    REQUIRE (events == std::vector<std::string> {
        "2:cursorEnter", "1:cursorEnter", "2:cursorDown", "1:cursorDown",
        "2:cursorUp", "2:cursorClick", "1:cursorUp", "1:cursorClick"});
}

TEST_CASE ("Scene cursor propagation blocker delivers native top layer events only",
           "[scene][cursor][solid]") {
    SceneCursorState state;
    std::vector<std::string> events;
    auto frame = [&] (bool moved, bool down, bool inside) {
        state.beginFrame (moved, down);
        for (const int layer : {2, 1}) {
            state.visit (layer, inside, [&] (const char* event) {
                events.push_back (std::to_string (layer) + ":" + event);
            });
            if (inside) break;
        }
        state.finishFrame ();
    };
    // Native disablepropagation control: the lower layer receives neither
    // hover nor button events while the upper hit blocks traversal.
    frame (false, false, true);
    frame (false, true, true);
    frame (false, false, true);
    frame (false, false, false);
    REQUIRE (events == std::vector<std::string> {
        "2:cursorEnter", "2:cursorDown", "2:cursorUp", "2:cursorClick", "2:cursorLeave"});
}

TEST_CASE ("Scene cursor solid and propagation skips retain native per-layer hover",
           "[scene][cursor][solid]") {
    SceneCursorState state;
    std::vector<std::string> events;
    auto frame = [&] (bool solid, bool block) {
        state.beginFrame (false, false);
        for (const int layer : {2, 1}) {
            if (layer == 2 && !solid) continue;
            state.visit (layer, true, [&] (const char* event) {
                events.push_back (std::to_string (layer) + ":" + event);
            });
            if (layer == 2 && block) break;
        }
        state.finishFrame ();
    };
    frame (true, false);
    events.clear ();
    // Actual stationary native controls retain hover across solid false/true
    // and across a preceding layer's propagation true/false transitions.
    frame (false, true);
    frame (true, true);
    frame (true, false);
    REQUIRE (events.empty ());
    // Destruction is distinct from temporarily skipping a candidate.
    state.forget (2);
    frame (true, false);
    REQUIRE (events == std::vector<std::string> {"2:cursorEnter"});
}

TEST_CASE ("Scene cursor overlap captures native drag and outside release per layer",
           "[scene][cursor][solid]") {
    SceneCursorState state;
    std::vector<std::string> events;
    auto frame = [&] (bool moved, bool down, bool inside) {
        state.beginFrame (moved, down);
        for (const int layer : {2, 1})
            state.visit (layer, inside, [&] (const char* event) {
                events.push_back (std::to_string (layer) + ":" + event);
            });
        state.finishFrame ();
    };
    frame (false, false, true);
    frame (false, true, true);
    events.clear ();
    // Native holds hover and sends Move to both pressed targets outside;
    // release then sends Up+Leave to each, without a Click.
    frame (true, true, false);
    REQUIRE (events == std::vector<std::string> {"2:cursorMove", "1:cursorMove"});
    events.clear ();
    frame (false, false, false);
    REQUIRE (events == std::vector<std::string> {
        "2:cursorUp", "2:cursorLeave", "1:cursorUp", "1:cursorLeave"});
    events.clear ();
    frame (true, false, false);
    REQUIRE (events.empty ());
}
