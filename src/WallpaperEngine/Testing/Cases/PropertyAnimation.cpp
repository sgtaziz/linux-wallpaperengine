#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Parsers/DynamicValueParser.h"

#include <cmath>
#include <limits>

using WallpaperEngine::Data::Model::PropertyAnimation;
using WallpaperEngine::Data::Parsers::DynamicValueParser;
using WallpaperEngine::Data::JSON::parseAuthoringJson;

TEST_CASE ("Authored property animation parses channels and samples curved handles", "[animation][property]") {
    auto value = DynamicValueParser::parse (parseAuthoringJson (R"({
      "value": "0 7", "animation": {
        "options": {"fps": 10, "length": 10, "mode": "single", "startpaused": true},
        "c0": [
          {"frame": 0, "value": 0, "front": {"enabled": true, "x": 0, "y": 0}},
          {"frame": 10, "value": 10, "back": {"enabled": true, "x": 0, "y": -10}}
        ],
        "c1": [{"frame": 0, "value": 7}, {"frame": 10, "value": 3}],
        "c2": [{"frame": 0, "value": 1}, {"frame": 10, "value": 9, "step": true}]
      }
    })", "animation-fixture.json"), {}, false);
    auto* animation = value->getAnimation ();
    REQUIRE (animation != nullptr);
    REQUIRE (animation->paused);
    REQUIRE (animation->mode == PropertyAnimation::Mode::Single);
    REQUIRE (animation->duration () == 1.0f);
    // Cubic y=10*t^3 with x=10*(3*t^2-2*t^3), solved at frame 2.
    REQUIRE (std::abs (animation->sampleFrame (0, 2) - 0.2368f) < 0.002f);
    REQUIRE (std::abs (animation->sampleFrame (0, 2) - 2.0f) > 1.0f);
    animation->setFrame (2.5f);
    const float frameBlend = (animation->sampleFrame (0, 2) + animation->sampleFrame (0, 3)) * 0.5f;
    REQUIRE (std::abs (animation->sample (0) - frameBlend) < 0.0001f);
    REQUIRE (std::abs (animation->sample (1) - 6.0f) < 0.002f);
    REQUIRE (animation->sampleFrame (2, 5) == 1.0f);
    animation->setFrame (10.0f);
    REQUIRE (std::abs (animation->sample (0) - 10.0f) < 0.002f);
}

TEST_CASE ("Single property animation obeys paused, play, finish, and stop state", "[animation][property]") {
    PropertyAnimation animation;
    animation.fps = 15.0f;
    animation.length = 15;
    animation.mode = PropertyAnimation::Mode::Single;
    animation.paused = true;
    animation.advance (0.25f);
    REQUIRE (animation.frame () == 0.0f);
    animation.play ();
    REQUIRE (animation.isPlaying ());
    animation.advance (0.5f);
    REQUIRE (std::abs (animation.frame () - 7.5f) < 0.0001f);
    animation.pause ();
    animation.advance (0.5f);
    REQUIRE (std::abs (animation.frame () - 7.5f) < 0.0001f);
    animation.play (); // resume from pause, not restart
    animation.advance (0.5f);
    REQUIRE (animation.finished);
    REQUIRE_FALSE (animation.isPlaying ());
    animation.play (); // native finished play restarts
    REQUIRE (animation.frame () == 0.0f);
    animation.setFrame (9.0f);
    REQUIRE (std::abs (animation.frame () - 9.0f) < 0.0001f);
    animation.stop ();
    REQUIRE (animation.isPlaying ());
    REQUIRE (animation.frame () == 0.0f);
    animation.channels[0] = {{0, 1.0f}, {15, 2.0f}};
    REQUIRE (animation.setFrame (1.0e30f));
    REQUIRE (std::isfinite (animation.sample (0)));
    animation.stop ();
    animation.rate = std::numeric_limits<float>::max ();
    animation.advance (std::numeric_limits<float>::max ());
    REQUIRE (animation.frame () == 0.0f);
}
