#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Objects/ParticleChildPool.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
#include <vector>

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

namespace {
// Expected values are the unchanged native death dispatch, executed
// from native (.cache/milestone-repair/
// ). Children never
// finish; active counts are taken after each tick. Boundary cases set the
// probability to the first draw, or to one ulp below it.
enum class Boundary { None, Equal, Below };
struct OracleTick {
    std::vector<uint32_t> expire;
    unsigned draws;
    std::vector<size_t> active;
};
struct OracleCase {
    const char* name;
    uint32_t seed;
    Boundary boundary;
    std::vector<NativeDeathDescriptor> deaths;
    std::vector<OracleTick> ticks;
    std::vector<std::vector<uint32_t>> slots;
    uint32_t mtNext;
};

std::vector<OracleCase> nativeDeathOracle () {
    return {
        {"single-cap3-p1-burst5", 11u, Boundary::None, {{0, 1.0f, 3}},
         {{{0, 1, 2, 3, 4}, 3, {3}}},
         {{0, 1, 2}}, 2854952027u},
        {"single-cap3-p1-staggered", 12u, Boundary::None, {{0, 1.0f, 3}},
         {{{0}, 1, {1}},
          {{1}, 1, {2}},
          {{2}, 1, {3}},
          {{3}, 0, {3}},
          {{4}, 0, {3}}},
         {{0, 1, 2}}, 3751169277u},
        {"single-cap50-p05-burst16", 13u, Boundary::None, {{0, 0.5f, 50}},
         {{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, 16, {3}}},
         {{2, 7, 10}}, 2755708342u},
        {"single-cap4-p05-burst16", 14u, Boundary::None, {{0, 0.5f, 4}},
         {{{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, 9, {4}}},
         {{5, 6, 7, 8}}, 864515722u},
        {"single-cap0-p1", 15u, Boundary::None, {{0, 1.0f, 0}},
         {{{0, 1, 2}, 0, {0}}},
         {{}}, 3645644232u},
        {"single-p0", 16u, Boundary::None, {{0, 0.0f, 10}},
         {{{0, 1, 2, 3}, 4, {0}}},
         {{}}, 2365244737u},
        {"default10-staggered12", 17u, Boundary::None, {{0, 1.0f, 10}},
         {{{0}, 1, {1}},
          {{1}, 1, {2}},
          {{2}, 1, {3}},
          {{3}, 1, {4}},
          {{4}, 1, {5}},
          {{5}, 1, {6}},
          {{6}, 1, {7}},
          {{7}, 1, {8}},
          {{8}, 1, {9}},
          {{9}, 1, {10}},
          {{10}, 0, {10}},
          {{11}, 0, {10}}},
         {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}}, 2818931012u},
        {"linuxdefault20-staggered12", 18u, Boundary::None, {{0, 1.0f, 20}},
         {{{0}, 1, {1}},
          {{1}, 1, {2}},
          {{2}, 1, {3}},
          {{3}, 1, {4}},
          {{4}, 1, {5}},
          {{5}, 1, {6}},
          {{6}, 1, {7}},
          {{7}, 1, {8}},
          {{8}, 1, {9}},
          {{9}, 1, {10}},
          {{10}, 1, {11}},
          {{11}, 1, {12}}},
         {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}, 2860884881u},
        {"two-cap1-p1-staggered6", 21u, Boundary::None, {{0, 1.0f, 1}, {1, 1.0f, 1}},
         {{{0}, 1, {1, 0}},
          {{1}, 2, {2, 1}},
          {{2}, 2, {3, 2}},
          {{3}, 2, {4, 3}},
          {{4}, 2, {5, 4}},
          {{5}, 2, {6, 5}}},
         {{0, 1, 2, 3, 4, 5}, {1, 2, 3, 4, 5}}, 673512177u},
        {"two-cap2-p1-burst3", 22u, Boundary::None, {{0, 1.0f, 2}, {1, 1.0f, 2}},
         {{{0, 1, 2}, 5, {3, 2}}},
         {{0, 1, 2}, {0, 2}}, 2737952102u},
        {"two-cap2-p1-burst3-then3", 23u, Boundary::None, {{0, 1.0f, 2}, {1, 1.0f, 2}},
         {{{0, 1, 2}, 5, {3, 2}},
          {{3, 4, 5}, 6, {6, 5}}},
         {{0, 1, 2, 3, 4, 5}, {0, 2, 3, 4, 5}}, 790253146u},
        {"three-cap10-p1-staggered12", 24u, Boundary::None, {{0, 1.0f, 10}, {1, 1.0f, 10}, {2, 1.0f, 10}},
         {{{0}, 3, {1, 1, 1}},
          {{1}, 3, {2, 2, 2}},
          {{2}, 3, {3, 3, 3}},
          {{3}, 3, {4, 4, 4}},
          {{4}, 3, {5, 5, 5}},
          {{5}, 3, {6, 6, 6}},
          {{6}, 3, {7, 7, 7}},
          {{7}, 3, {8, 8, 8}},
          {{8}, 3, {9, 9, 9}},
          {{9}, 1, {10, 9, 9}},
          {{10}, 2, {11, 10, 9}},
          {{11}, 0, {11, 10, 9}}},
         {{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 10}, {0, 1, 2, 3, 4, 5, 6, 7, 8}}, 1062118654u},
        {"two-cap3-p05-staggered10", 25u, Boundary::None, {{0, 0.5f, 3}, {1, 0.5f, 3}},
         {{{0}, 2, {0, 0}},
          {{1}, 2, {0, 1}},
          {{2}, 2, {1, 2}},
          {{3}, 2, {2, 3}},
          {{4}, 1, {3, 3}},
          {{5}, 0, {3, 3}},
          {{6}, 0, {3, 3}},
          {{7}, 0, {3, 3}},
          {{8}, 0, {3, 3}},
          {{9}, 0, {3, 3}}},
         {{2, 3, 4}, {1, 2, 3}}, 646899634u},
        {"boundary-equal", 31u, Boundary::Equal, {{0, 0.0f, 10}},
         {{{0}, 1, {1}}},
         {{0}}, 124517975u},
        {"boundary-below", 31u, Boundary::Below, {{0, 0.0f, 10}},
         {{{0}, 1, {0}}},
         {{}}, 124517975u},
        {"follow-invalidate-plus-death", 41u, Boundary::None, {{1, 1.0f, 10}},
         {{{3}, 1, {1}},
          {{5, 6}, 2, {3}}},
         {{3, 5, 6}}, 1378570210u},
    };
}
} // namespace

TEST_CASE ("Death events share the native per-tick budget across descriptors", "[particle][child][event]") {
    for (auto scenario : nativeDeathOracle ()) {
        INFO (scenario.name);
        std::mt19937 rng (scenario.seed);
        if (scenario.boundary != Boundary::None) {
            std::mt19937 probe (scenario.seed);
            float probability = nativeRandomUnit (probe);
            if (scenario.boundary == Boundary::Below) probability = std::nextafter (probability, 0.0f);
            scenario.deaths[0].probability = probability;
        }
        std::map<size_t, std::vector<uint32_t>> nodes;
        for (size_t tick = 0; tick < scenario.ticks.size (); ++tick) {
            INFO ("tick " << tick);
            const auto& expected = scenario.ticks[tick];
            unsigned draws = 0;
            dispatchNativeDeathEvents (expected.expire.size (), scenario.deaths,
                [&nodes] (size_t descriptor) { return nodes[descriptor].size (); },
                [&rng, &draws] { ++draws; return nativeRandomUnit (rng); },
                [&nodes, &expected] (size_t expired, size_t descriptor) {
                    nodes[descriptor].push_back (expected.expire[expired]);
                });
            CHECK (draws == expected.draws);
            for (size_t i = 0; i < scenario.deaths.size (); ++i)
                CHECK (nodes[scenario.deaths[i].descriptor].size () == expected.active[i]);
        }
        for (size_t i = 0; i < scenario.deaths.size (); ++i)
            CHECK (nodes[scenario.deaths[i].descriptor] == scenario.slots[i]);
        CHECK (rng () == scenario.mtNext);
    }
}

TEST_CASE ("Static and event child chains stop at native depth 10", "[particle][child][event]") {
    // W11 recursion fixtures: a 12-asset static chain, a static self-reference
    // and an eventspawn self-reference all build descendants at depths 1..10
    // under a depth-0 root. Self-references are bounded only by depth.
    const auto descendants = [] (uint32_t authoredLinks) {
        uint32_t depth = 0;
        while (depth < authoredLinks && nativeNodeOwnsChildren (depth)) ++depth;
        return depth;
    };
    CHECK (descendants (12) == 10);
    CHECK (descendants (UINT32_MAX) == 10);
    CHECK (descendants (2) == 2);
    CHECK (nativeNodeOwnsChildren (9));
    CHECK_FALSE (nativeNodeOwnsChildren (10));
}
