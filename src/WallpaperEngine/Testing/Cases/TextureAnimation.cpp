#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/TextureAnimation.h"

#include <limits>
#include <utility>
#include <vector>

using namespace WallpaperEngine::Data::Assets;
using WallpaperEngine::Render::frameAtTime;
using WallpaperEngine::Render::framePhaseAtTime;
using WallpaperEngine::Render::particleSequenceCycles;
using WallpaperEngine::Render::particleAtlasBlendEnabled;

TEST_CASE ("Native atlas sequence blend respects particle mode and flag 2",
           "[particle][texture][animation]") {
    // The native renderer supplies this combo even when the material omits it
    // or authors the opposite value; the particle node decides the result.
    REQUIRE (particleAtlasBlendEnabled (0u, "sequence"));
    REQUIRE (particleAtlasBlendEnabled (0u, ""));
    REQUIRE (particleAtlasBlendEnabled (4u, "sequence"));
    REQUIRE_FALSE (particleAtlasBlendEnabled (2u, "sequence"));
    REQUIRE_FALSE (particleAtlasBlendEnabled (0u, "randomframe"));
    REQUIRE_FALSE (particleAtlasBlendEnabled (2u, "randomframe"));

    // An explicitly authored zero cycle still stays at tile zero: enabling
    // the native blend combo does not introduce progression by itself.
    const double frozen = particleSequenceCycles (3.0, 4.0, 0.0);
    REQUIRE (frozen == 0.0);
    std::vector<FrameSharedPtr> frames;
    for (unsigned i = 0; i < 4; ++i) {
        auto frame = std::make_shared<Frame> ();
        frame->frameNumber = i;
        frame->frametime = 0.25f;
        frames.push_back (std::move (frame));
    }
    const auto still = framePhaseAtTime (frames, frozen);
    REQUIRE (still->ordinal == 0);
    REQUIRE (still->fraction == 0.0);
    const auto beforeWrap = framePhaseAtTime (frames, 0.999);
    const auto afterWrap = framePhaseAtTime (frames, 1.0);
    REQUIRE (beforeWrap->ordinal == 3);
    REQUIRE (beforeWrap->fraction == Catch::Approx (0.996));
    REQUIRE (afterWrap->ordinal == 0);
    REQUIRE (afterWrap->fraction == 0.0);
}

TEST_CASE ("Particle atlas sequences complete authored cycles over particle lifetime",
           "[particle][texture][animation]") {
    // Native 3217188597 fixed-bird probes: multiplier 25 repeats after 1.2s
    // with lifetime 30, and after 0.6s with lifetime 15. Doubling all TEXS
    // frame durations does not change that period.
    REQUIRE (particleSequenceCycles (1.2, 30.0, 25.0) == Catch::Approx (1.0));
    REQUIRE (particleSequenceCycles (0.6, 15.0, 25.0) == Catch::Approx (1.0));
    REQUIRE (particleSequenceCycles (0.6, 30.0, 25.0) == Catch::Approx (0.5));
    REQUIRE (particleSequenceCycles (1.2, 30.0, 1.0) == Catch::Approx (0.04));
    // Native 2.8.42 fixed Fog1 probe: explicit zero holds the first atlas tile,
    // while missing/null fields are parsed as the ordinary multiplier of one.
    REQUIRE (particleSequenceCycles (1.2, 30.0, 0.0) == Catch::Approx (0.0));
    REQUIRE (particleSequenceCycles (9.0, 30.0, 0.0) == Catch::Approx (0.0));
    REQUIRE (particleSequenceCycles (0.0, 0.0, 25.0) == Catch::Approx (25.0));

    std::vector<FrameSharedPtr> oneSecond, twoSeconds;
    for (unsigned i = 0; i < 2; ++i) {
        auto one = std::make_shared<Frame> ();
        one->frameNumber = i;
        one->frametime = 0.5f;
        oneSecond.push_back (one);
        auto two = std::make_shared<Frame> ();
        two->frameNumber = i;
        two->frametime = 1.0f;
        twoSeconds.push_back (two);
    }
    const double cycles = particleSequenceCycles (0.72, 30.0, 25.0);
    const auto phaseOne = framePhaseAtTime (oneSecond, cycles * 1.0);
    const auto phaseTwo = framePhaseAtTime (twoSeconds, cycles * 2.0);
    REQUIRE (phaseOne->ordinal == 1);
    REQUIRE (phaseTwo->ordinal == phaseOne->ordinal);
    REQUIRE (phaseTwo->fraction == Catch::Approx (phaseOne->fraction));
}

TEST_CASE ("Texture animation uses its own unequal frame durations and page indices", "[render][texture][animation]") {
    std::vector<FrameSharedPtr> frames;
    for (const auto [page, seconds] : {std::pair {2u, 0.25f}, {0u, 0.5f}, {1u, 0.75f}}) {
        auto frame = std::make_shared<Frame> ();
        frame->frameNumber = page;
        frame->frametime = seconds;
        frames.push_back (std::move (frame));
    }
    REQUIRE (frameAtTime (frames, 0.0)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 0.25)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 0.251)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, 0.75)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, 0.751)->frameNumber == 1);
    REQUIRE (frameAtTime (frames, 1.5)->frameNumber == 2);
    REQUIRE (frameAtTime (frames, 1.751)->frameNumber == 0);
    REQUIRE (frameAtTime (frames, -0.1)->frameNumber == 1);
    REQUIRE (framePhaseAtTime (frames, 0.125)->ordinal == 0);
    REQUIRE (framePhaseAtTime (frames, 0.125)->fraction == Catch::Approx (0.5));
    REQUIRE (framePhaseAtTime (frames, 0.25)->ordinal == 0);
    REQUIRE (framePhaseAtTime (frames, 0.25)->fraction == Catch::Approx (1.0));
    REQUIRE (framePhaseAtTime (frames, 0.5)->ordinal == 1);
    REQUIRE (framePhaseAtTime (frames, 0.5)->fraction == Catch::Approx (0.5));
    REQUIRE (framePhaseAtTime (frames, 1.125)->ordinal == 2);
    REQUIRE (framePhaseAtTime (frames, 1.125)->fraction == Catch::Approx (0.5));

    // Pass input 0 can change independently of the renderable's primary image.
    // The selector has no primary-image duration argument to contaminate its clock.
    frames[1]->frametime = 0.1f;
    REQUIRE (frameAtTime (frames, 0.5)->frameNumber == 1);
}

TEST_CASE ("Invalid frame timing fails without undefined page selection", "[render][texture][animation]") {
    std::vector<FrameSharedPtr> frames;
    REQUIRE (frameAtTime (frames, 0.0) == nullptr);
    auto frame = std::make_shared<Frame> ();
    frames.push_back (frame);
    REQUIRE (frameAtTime (frames, 1.0) == nullptr);
    frame->frametime = 0.1f;
    REQUIRE (frameAtTime (frames, std::numeric_limits<double>::quiet_NaN ()) == nullptr);
}
