#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Objects/ParticleGeometry.h"

#include <stdexcept>
#include <string>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

TEST_CASE ("Inner particle warmup retains publication and never advances descendants", "[particle-stages]") {
    bool birthEvents = true;
    int cpuPosition = 24;
    int publishedPosition = 0;
    int childClock = 0;
    int spawned = 0;
    std::string order;
    const auto inner = [&] {
        cpuPosition += 24;
        if (birthEvents) ++spawned;
        order += 'I';
    };
    const auto publish = [&] { publishedPosition = cpuPosition; order += 'P'; };
    const auto children = [&] {
        // Child traversal must observe this node's just-published stream.
        REQUIRE (publishedPosition == cpuPosition);
        ++childClock;
        order += 'C';
    };
    advanceNodeStages (NodeAdvanceMode::Warmup, birthEvents, inner, publish, children);
    REQUIRE (cpuPosition == 48);
    REQUIRE (publishedPosition == 0);
    REQUIRE (childClock == 0);
    REQUIRE (spawned == 0);
    REQUIRE (birthEvents);
    REQUIRE (order == "I");
    advanceNodeStages (NodeAdvanceMode::Outer, birthEvents, inner, publish, children);
    REQUIRE (publishedPosition == 72);
    REQUIRE (childClock == 1);
    REQUIRE (spawned == 1);
    REQUIRE (order == "IIPC");
    REQUIRE_THROWS_AS (advanceNodeStages (NodeAdvanceMode::Warmup, birthEvents,
        [] { throw std::runtime_error ("failed inner operation"); }, publish, children), std::runtime_error);
    REQUIRE (birthEvents);
    birthEvents = false;
    advanceNodeStages (NodeAdvanceMode::Warmup, birthEvents, [] {}, publish, children);
    REQUIRE_FALSE (birthEvents);
}

TEST_CASE ("Published rope attributes survive script transforms while shader directions follow the draw matrix",
           "[particle-stages]") {
    GeometryPacket rope {1, true, std::vector<float> (8 * 37, 0.0f), {0, 1, 2},
                         {{{0, 0, 0}, {0, 0, 0}, {1, 0, 0}, {1, 0, 0}, 2, 4}}, 2};
    for (size_t vertex = 0; vertex < 8; ++vertex) {
        rope.vertices[vertex * 37] = 24.0f;
        rope.vertices[vertex * 37 + 26] = 0.75f;
    }
    const auto front = ropeDrawVertices (rope, {0, 0, 1}, {0, 0, -1}, false);
    const auto side = ropeDrawVertices (rope, {0, 0, 1}, {0, -1, 0}, false);
    REQUIRE (front[29] == Catch::Approx (2));
    REQUIRE (side[30] == Catch::Approx (-2));
    REQUIRE (front[31 + 1] == Catch::Approx (3)); // smoothstep halfway sized-right
    REQUIRE (side[31 + 2] == Catch::Approx (-3));
    REQUIRE (front[28] == side[28]);
    REQUIRE (rope.vertices[29] == 0); // drawing never alters the retained publication
    for (size_t vertex = 0; vertex < 8; ++vertex) {
        REQUIRE (front[vertex * 37] == 24);
        REQUIRE (side[vertex * 37] == 24);
        REQUIRE (front[vertex * 37 + 26] == 0.75f);
        REQUIRE (side[vertex * 37 + 26] == 0.75f);
    }
    // Mixed renderer packets own their attributes independently of the rope
    // stream and of later CPU birth-slot mutations.
    GeometryPacket sprite {0, false, {24}, {0, 1, 2}};
    std::vector<GeometryPacket> retained {sprite, rope};
    sprite.vertices[0] = 48;
    REQUIRE (retained[0].vertices[0] == 24);
    REQUIRE (retained[1].vertices[0] == 24);
    REQUIRE (ropeDrawVertices (retained[1], {0, 0, 1}, {0, 0, -1}, false) == front);
}

TEST_CASE ("Retirement and one-point rope retain separate published streams for late API warmup",
           "[particle-stages]") {
    GeometryPublication publication;
    REQUIRE (publication.beginStream (0, 2, false));
    publication.append ({0, false, {24}, {0, 1, 2}});
    REQUIRE (publication.beginStream (1, 2, true));
    publication.append ({1, true, {48}, {0, 1, 2}, {}});
    // Retirement's outer traversal sees zero CPU live particles. Native does
    // not map either stream; script warmup can make the retained node drawable
    // before a subsequent outer traversal replaces its geometry.
    REQUIRE_FALSE (publication.beginStream (0, 0, false));
    REQUIRE_FALSE (publication.beginStream (1, 0, true));
    REQUIRE (publication.streams ()[0][0].vertices[0] == 24);
    REQUIRE (publication.streams ()[1][0].vertices[0] == 48);
    // At one live particle only the sprite stream is replaced. Rope remains
    // gated from drawing now, but is still available to a later inner-only API.
    REQUIRE (publication.beginStream (0, 1, false));
    publication.append ({0, false, {72}, {0, 1, 2}});
    REQUIRE_FALSE (publication.beginStream (1, 1, true));
    REQUIRE (publication.streams ()[0][0].vertices[0] == 72);
    REQUIRE (publication.streams ()[1][0].vertices[0] == 48);
    REQUIRE (publication.beginStream (1, 2, true));
    REQUIRE (publication.streams ()[1].empty ());
    // A cold node has no publication even if API warmup produces live slots.
    GeometryPublication cold;
    REQUIRE_FALSE (cold.beginStream (0, 0, false));
    REQUIRE (cold.streams ().empty ());
}

TEST_CASE ("Node pause keeps aging clock and allows forced emissions independently of stop",
           "[particle-stages]") {
    REQUIRE (automaticEmissionAllowed (true, false, 0));
    REQUIRE_FALSE (automaticEmissionAllowed (true, true, 0));
    REQUIRE (automaticEmissionAllowed (true, true, 1));
    REQUIRE_FALSE (automaticEmissionAllowed (false, false, 0));
    REQUIRE (automaticEmissionAllowed (false, true, 1));
    const auto clock = tickClock (0.1f, 0.1f, 30);
    float age = 0.3f;
    dispatchTick (clock, [&] (float dt) { age += dt; }, [] (MovementTime) {});
    REQUIRE (age > 0.35f); // API pause is an emitter gate, not a frozen lifetime.
}

TEST_CASE ("Hidden root reset retains CP and publication while forced births remain independent of admission",
           "[particle-admission]") {
    GeometryPublication publication;
    REQUIRE (publication.beginStream (0, 1, false));
    publication.append ({0, false, {0}, {0, 1, 2}});
    uint32_t liveCount = 1;
    float cp = 24;
    bool enabled = false;
    uint32_t resets = 0;
    const auto reset = [&] { ++resets; liveCount = 0; };
    resetHiddenRoot (true, liveCount, reset);
    REQUIRE (resets == 0);
    resetHiddenRoot (false, liveCount, reset);
    REQUIRE (liveCount == 0);
    REQUIRE (resets == 1);
    REQUIRE (cp == 24);
    REQUIRE_FALSE (enabled);
    REQUIRE_FALSE (publication.beginStream (0, liveCount, false));
    REQUIRE (publication.streams ()[0][0].vertices[0] == 0);
    // Admission does not disable the forced API or overwrite the root's API
    // pause bit. A hidden API birth advances its retained CP, then is cleared
    // at the next outer admission check, leaving the old GPU stream intact.
    REQUIRE_FALSE (automaticEmissionAllowed (true, false, 0, false));
    REQUIRE (automaticEmissionAllowed (enabled, false, 1, false));
    const float hiddenBirth = cp;
    cp += 24;
    liveCount = 1;
    resetHiddenRoot (false, liveCount, reset);
    REQUIRE (hiddenBirth == 24);
    REQUIRE (liveCount == 0);
    REQUIRE (cp == 48);
    REQUIRE (resets == 2);
    resetHiddenRoot (false, liveCount, reset);
    REQUIRE (resets == 2); // Empty roots do not reset descendants/clocks.
    resetHiddenRoot (true, liveCount, reset);
    const float visibleBirth = cp;
    liveCount = 1;
    REQUIRE (visibleBirth == 48);
    REQUIRE (publication.streams ()[0][0].vertices[0] == 0);
    REQUIRE (publication.beginStream (0, liveCount, false));
    publication.append ({0, false, {visibleBirth}, {0, 1, 2}});
    REQUIRE (publication.streams ()[0][0].vertices[0] == 48);
}
