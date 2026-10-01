#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Objects/ParticleChildPool.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <memory>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

TEST_CASE ("Event child pools retain ownership and select each descriptor in reverse completion order",
           "[particle-child-pool]") {
    struct State {
        int generatedControlPoint;
        int& destroyed;
        ~State () { ++destroyed; }
    };
    int destroyed = 0;
    {
        RetainedChildPool<std::unique_ptr<State>> pool;
        auto first = std::unique_ptr<State> (new State { 24, destroyed });
        const auto* firstIdentity = first.get ();
        pool.retire (2, std::move (first));
        pool.retire (0, std::unique_ptr<State> (new State { 72, destroyed }));
        pool.retire (2, std::unique_ptr<State> (new State { 48, destroyed }));
        REQUIRE (destroyed == 0);
        REQUIRE_FALSE (pool.take (1));
        auto latest = pool.take (2);
        REQUIRE ((*latest)->generatedControlPoint == 48);
        latest.reset ();
        REQUIRE (destroyed == 1);
        auto retained = pool.take (2);
        REQUIRE (retained->get () == firstIdentity);
        REQUIRE ((*retained)->generatedControlPoint == 24);
        (*retained)->generatedControlPoint += 24;
        pool.retire (2, std::move (*retained));
        retained.reset ();
        REQUIRE (destroyed == 1);
        REQUIRE_FALSE (pool.take (99));
        auto isolated = pool.take (0);
        REQUIRE ((*isolated)->generatedControlPoint == 72);
    }
    REQUIRE (destroyed == 3);
}

TEST_CASE ("Inactive child patch traversal retains generated values and allocation high water",
           "[particle-child-pool]") {
    struct State {
        NativeSlotAllocation slots;
        float phase = 0.75f;
        float signedStep = -0.25f;
        float generatedControlPoint = 48.0f;
    };
    RetainedChildPool<std::unique_ptr<State>> pool;
    auto child = std::make_unique<State> ();
    REQUIRE (child->slots.allocate (4) == 0);
    REQUIRE (child->slots.allocate (4) == 1);
    child->slots.release (0);
    child->slots.release (1);
    pool.retire (0, std::move (child));
    pool.visit ([] (auto& inactive) {
        // A root instance patch reaches inactive records before reactivation.
        inactive->signedStep = -0.5f;
    });
    child = std::move (*pool.take (0));
    resetSequencePhase (child->phase, child->signedStep);
    REQUIRE (child->phase == 0.0f);
    REQUIRE (child->signedStep == -0.5f);
    REQUIRE (child->generatedControlPoint == 48.0f);
    REQUIRE (child->slots.highWater () == 2);
    REQUIRE (child->slots.allocate (4) == 0);
    REQUIRE (child->slots.highWater () == 2);
}
