#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Objects/ParticleChildPool.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vector_relational.hpp>
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

namespace {
struct BirthDraw {
    size_t descriptor;
    size_t slot;
    bool accepted;
    unsigned warmupWords;
};
struct BirthTick {
    unsigned wordsBefore;
    size_t born;
    std::vector<BirthDraw> draws;
};
struct BirthScenario {
    const char* name;
    uint32_t seed;
    std::vector<NativeBirthDescriptor> births;
    std::vector<BirthTick> ticks;
    std::vector<size_t> active;
    uint32_t mtNext;
};

// Original 2.8.42 with traced MT words (P1 oracle):
// the parent's emission words before the dispatch, each dispatch draw (slots
// relative to the tick's first birth), and the child warm-up words that
// follow an accepted draw.
std::vector<BirthScenario> nativeBirthOracle () {
    return {
        {"spawn-p1-burst5-cap8", 101u, {{0, 1.0f, 8}},
         {{30, 5, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {0, 3, true, 0}, {0, 4, true, 0}}}},
         {5}, 4200099656u},
        {"spawn-p1-burst5-cap5", 102u, {{0, 1.0f, 5}},
         {{30, 5, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {0, 3, true, 0}, {0, 4, true, 0}}}},
         {5}, 203961775u},
        {"spawn-p1-burst5-cap3", 103u, {{0, 1.0f, 3}},
         {{30, 3, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}}}},
         {3}, 1222169142u},
        {"spawn-p05-burst16-cap50", 104u, {{0, 0.5f, 50}},
         {{96, 16, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, false, 0}, {0, 3, false, 0}, {0, 4, true, 0}, {0, 5, false, 0}, {0, 6, true, 0}, {0, 7, true, 0}, {0, 8, true, 0}, {0, 9, false, 0}, {0, 10, true, 0}, {0, 11, true, 0}, {0, 12, true, 0}, {0, 13, true, 0}, {0, 14, true, 0}, {0, 15, false, 0}}}},
         {11}, 3064320526u},
        {"spawn-p05-burst16-cap4", 105u, {{0, 0.5f, 4}},
         {{96, 7, {{0, 0, false, 0}, {0, 1, false, 0}, {0, 2, true, 0}, {0, 3, true, 0}, {0, 4, false, 0}, {0, 5, true, 0}, {0, 6, true, 0}}}},
         {4}, 2537975672u},
        {"follow-p1+spawn-p05-burst6", 106u, {{0, 1.0f, 10}, {1, 0.5f, 10}},
         {{36, 6, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {0, 3, true, 0}, {0, 4, true, 0}, {0, 5, true, 0}, {1, 0, true, 0}, {1, 1, false, 0}, {1, 2, true, 0}, {1, 3, true, 0}, {1, 4, false, 0}, {1, 5, false, 0}}}},
         {6, 3}, 1681541872u},
        {"spawn-p05-cap3+follow-p05-cap3-burst12", 107u, {{0, 0.5f, 3}, {1, 0.5f, 3}},
         {{72, 12, {{0, 0, false, 0}, {0, 1, false, 0}, {0, 2, false, 0}, {0, 3, true, 0}, {0, 4, false, 0}, {0, 5, true, 0}, {0, 6, false, 0}, {0, 7, false, 0}, {0, 8, true, 0}, {1, 0, false, 0}, {1, 1, false, 0}, {1, 2, true, 0}, {1, 3, false, 0}, {1, 4, false, 0}, {1, 5, true, 0}, {1, 6, false, 0}, {1, 7, false, 0}, {1, 8, false, 0}, {1, 9, false, 0}, {1, 10, false, 0}, {1, 11, true, 0}}}},
         {3, 3}, 2514372692u},
        {"follow-p1-x2-burst4-refcount2", 108u, {{0, 1.0f, 10}, {1, 1.0f, 10}},
         {{24, 4, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {0, 3, true, 0}, {1, 0, true, 0}, {1, 1, true, 0}, {1, 2, true, 0}, {1, 3, true, 0}}}},
         {4, 4}, 4268876931u},
        {"cap0-spawn+follow-burst5", 109u, {{0, 1.0f, 0}, {1, 1.0f, 0}},
         {{30, 0, {}}},
         {0, 0}, 3948255239u},
        {"reuse-lifo-pool6-burst4", 110u, {{0, 1.0f, 10}},
         {{24, 4, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {0, 3, true, 0}}}},
         {4}, 1773099434u},
        {"spawn-p1-cap5-rate60-4ticks", 111u, {{0, 1.0f, 5}},
         {{12, 2, {{0, 0, true, 0}, {0, 1, true, 0}}},
          {12, 2, {{0, 0, true, 0}, {0, 1, true, 0}}},
          {12, 1, {{0, 0, true, 0}}},
          {12, 0, {}}},
         {5}, 1160954129u},
        {"follow-p05+spawn-p05-rate90-3ticks", 112u, {{0, 0.5f, 6}, {1, 0.5f, 6}},
         {{18, 3, {{0, 0, false, 0}, {0, 1, false, 0}, {0, 2, true, 0}, {1, 0, false, 0}, {1, 1, false, 0}, {1, 2, false, 0}}},
          {18, 3, {{0, 0, false, 0}, {0, 1, true, 0}, {0, 2, true, 0}, {1, 0, false, 0}, {1, 1, false, 0}, {1, 2, true, 0}}},
          {18, 3, {{0, 0, true, 0}, {0, 1, true, 0}, {0, 2, false, 0}, {1, 0, true, 0}, {1, 1, false, 0}, {1, 2, false, 0}}}},
         {5, 2}, 2936608506u},
        {"warm-spawn-p1-burst3+spawn-p05", 113u, {{0, 1.0f, 10}, {1, 0.5f, 10}},
         {{18, 3, {{0, 0, true, 36}, {0, 1, true, 36}, {0, 2, true, 36}, {1, 0, true, 0}, {1, 1, false, 0}, {1, 2, false, 0}}}},
         {3, 1}, 1426749626u},
        {"warm-follow-p05-burst6+warm-spawn-p1-cap2", 114u, {{0, 0.5f, 10}, {1, 1.0f, 2}},
         {{36, 6, {{0, 0, false, 0}, {0, 1, false, 0}, {0, 2, true, 36}, {0, 3, false, 0}, {0, 4, false, 0}, {0, 5, false, 0}, {1, 0, true, 12}, {1, 1, true, 12}}}},
         {1, 2}, 2167232341u},
        {"warm-child-with-eventdeath-burst2", 115u, {{0, 1.0f, 10}},
         {{12, 2, {{0, 0, true, 48}, {0, 1, true, 48}}}},
         {2}, 923402684u},
        {"warm-rope-child-with-eventdeath-burst2", 117u, {{0, 1.0f, 10}},
         {{12, 2, {{0, 0, true, 54}, {0, 1, true, 54}}}},
         {2}, 1849962911u},
        {"boundary-equal", 116u, {{0, 0.0417606235f, 10}},
         {{12, 2, {{0, 0, true, 0}, {0, 1, false, 0}}}},
         {1}, 2052968396u},
        {"boundary-below", 116u, {{0, 0.0417606197f, 10}},
         {{12, 2, {{0, 0, false, 0}, {0, 1, false, 0}}}},
         {0}, 2052968396u}
    };
}
} // namespace

TEST_CASE ("Birth events walk descriptors outermost with native draws and caps", "[particle][child][event]") {
    for (const auto& scenario : nativeBirthOracle ()) {
        INFO (scenario.name);
        std::mt19937 rng (scenario.seed);
        std::map<size_t, size_t> active;
        for (size_t tick = 0; tick < scenario.ticks.size (); ++tick) {
            INFO ("tick " << tick);
            const auto& expected = scenario.ticks[tick];
            rng.discard (expected.wordsBefore);
            std::vector<BirthDraw> draws;
            dispatchNativeBirthEvents (expected.born, scenario.births,
                [&active] (size_t descriptor) { return active[descriptor]; },
                [&] {
                    draws.push_back ({0, 0, false, 0});
                    return nativeRandomUnit (rng);
                },
                [&] (size_t born, size_t descriptor) {
                    ++active[descriptor];
                    draws.back () = {descriptor, born, true, 0};
                    // The spawned child's warm-up consumes its words now.
                    if (draws.size () <= expected.draws.size ())
                        rng.discard (expected.draws[draws.size () - 1].warmupWords);
                });
            REQUIRE (draws.size () == expected.draws.size ());
            for (size_t i = 0; i < draws.size (); ++i) {
                const auto& want = expected.draws[i];
                CHECK (draws[i].accepted == want.accepted);
                if (draws[i].accepted) {
                    CHECK (draws[i].descriptor == want.descriptor);
                    CHECK (draws[i].slot == want.slot);
                }
            }
        }
        for (size_t i = 0; i < scenario.active.size (); ++i)
            CHECK (active[i] == scenario.active[i]);
        CHECK (rng () == scenario.mtNext);
    }
}

TEST_CASE ("Warm-up records expired slots only for rope renderers", "[particle][child][event]") {
    // P1 oracle: a non-rope child warmed under an eventdeath descriptor makes
    // no death draws (48 warm-up words); the rope variant draws them (54).
    CHECK (nativeRecordsExpiredSlots (true, false));
    CHECK (nativeRecordsExpiredSlots (true, true));
    CHECK_FALSE (nativeRecordsExpiredSlots (false, false));
    CHECK (nativeRecordsExpiredSlots (false, true));
}

TEST_CASE ("Event child stacks compose the parent's current stack with the last anchor", "[particle][child][event]") {
    // Original 2.8.42 (P2 oracle). Every node anchors at
    // t0 with (S0, p0); live follow re-anchors each tick with (S_t, p_t);
    // spawn, death and a follow detached at t1 keep the t0 anchor.
    const glm::mat4 s0 (glm::vec4 (0, 2, 0, 0), glm::vec4 (-2, 0, 0, 0), glm::vec4 (0, 0, 1, 0), glm::vec4 (100, -50, 0, 1));
    const glm::mat4 stacks[] = {s0, glm::translate (glm::mat4 (1.0f), glm::vec3 (130, -20, 0)),
        glm::scale (glm::translate (glm::mat4 (1.0f), glm::vec3 (200, 10, 0)), glm::vec3 (0.5f, 0.5f, 1.0f))};
    const glm::vec3 particles[] = {{10, 20, 0}, {14, 26, 0}, {20, 30, 0}};
    const glm::mat4 descriptor = glm::scale (glm::translate (glm::mat4 (1.0f), glm::vec3 (3, 4, 0)), glm::vec3 (2, 2, 1));
    struct Expected { glm::vec2 column0, column1, translation; };
    struct Case {
        bool childWorld, parentWorld, live;
        Expected ticks[3];
    };
    const Case cases[] = {
        {false, false, false, {{{0, 4}, {-4, 0}, {52, -24}}, {{2, 0}, {0, 2}, {143, 4}}, {{1, 0}, {0, 1}, {206.5f, 22}}}},
        {false, false, true, {{{0, 4}, {-4, 0}, {52, -24}}, {{2, 0}, {0, 2}, {147, 10}}, {{1, 0}, {0, 1}, {211.5f, 27}}}},
        {false, true, false, {{{2, 0}, {0, 2}, {13, 24}}, {{0, -1}, {1, 0}, {167, 23.5f}}, {{0, -0.5f}, {0.5f, 0}, {218.5f, 31.75f}}}},
        {false, true, true, {{{2, 0}, {0, 2}, {13, 24}}, {{2, 0}, {0, 2}, {17, 30}}, {{2, 0}, {0, 2}, {23, 34}}}},
        {true, false, false, {{{0, 4}, {-4, 0}, {52, -24}}, {{0, 4}, {-4, 0}, {52, -24}}, {{0, 4}, {-4, 0}, {52, -24}}}},
        {true, false, true, {{{0, 4}, {-4, 0}, {52, -24}}, {{2, 0}, {0, 2}, {147, 10}}, {{1, 0}, {0, 1}, {211.5f, 27}}}},
        {true, true, false, {{{2, 0}, {0, 2}, {13, 24}}, {{2, 0}, {0, 2}, {13, 24}}, {{2, 0}, {0, 2}, {13, 24}}}},
        {true, true, true, {{{2, 0}, {0, 2}, {13, 24}}, {{2, 0}, {0, 2}, {17, 30}}, {{2, 0}, {0, 2}, {23, 34}}}},
    };
    const auto near = [] (glm::vec2 a, glm::vec2 b) { return glm::all (glm::lessThan (glm::abs (a - b), glm::vec2 (1e-4f))); };
    for (const auto& c : cases) {
        INFO ("child world " << c.childWorld << " parent world " << c.parentWorld << " live follow " << c.live);
        auto basis = nativeEventChildBasis (stacks[0], c.childWorld, c.parentWorld);
        REQUIRE (basis);
        glm::vec3 particle = particles[0];
        for (int tick = 0; tick < 3; ++tick) {
            INFO ("tick " << tick);
            if (c.live && tick > 0) {
                basis = nativeEventChildBasis (stacks[tick], c.childWorld, c.parentWorld);
                particle = particles[tick];
            }
            const auto top = nativeEventChildStack (stacks[tick], *basis, particle, descriptor, c.childWorld);
            CHECK (top[2] == glm::vec4 (0, 0, 1, 0));
            CHECK (near (glm::vec2 (top[0]), c.ticks[tick].column0));
            CHECK (near (glm::vec2 (top[1]), c.ticks[tick].column1));
            CHECK (near (glm::vec2 (top[3]), c.ticks[tick].translation));
        }
    }
}

TEST_CASE ("Child particle nodes take the owner's camera parallax shift once", "[particle][child][event][parallax]") {
    // shifts the view stack by the root scene object's parallax for
    // the whole draw; only changes the model stack (P2 oracle).
    const glm::vec3 parallax (12.5f, -7.0f, 0.0f);
    const glm::mat4 root = glm::rotate (glm::translate (glm::mat4 (1.0f), glm::vec3 (40, 30, 0)), 0.5f, glm::vec3 (0, 0, 1));
    const glm::mat4 descriptor = glm::translate (glm::mat4 (1.0f), glm::vec3 (3, 4, 0));
    const glm::vec3 particle (10, 20, 0);
    const glm::mat4 child = nativeEventChildStack (root, glm::mat4 (1.0f), particle, descriptor, false);
    const glm::mat4 rootDraw = nativeNodeDrawModel (parallax, root, false, false);
    const glm::mat4 childDraw = nativeNodeDrawModel (parallax, child, false, false);
    const glm::mat4 expected = rootDraw * glm::translate (glm::mat4 (1.0f), particle) * descriptor;
    for (int column = 0; column < 4; ++column)
        CHECK (glm::all (glm::lessThan (glm::abs (childDraw[column] - expected[column]), glm::vec4 (1e-4f))));
    CHECK (glm::all (glm::lessThan (glm::abs (glm::vec3 (childDraw[3] - child[3]) - parallax), glm::vec3 (1e-4f))));
    // A world-preset node draws from the scene base, still shifted once.
    CHECK (nativeNodeDrawModel (parallax, child, true, false) == glm::translate (glm::mat4 (1.0f), parallax));
    CHECK (nativeNodeDrawModel (parallax, child, true, true)
           == glm::translate (glm::mat4 (1.0f), parallax) * glm::scale (glm::mat4 (1.0f), glm::vec3 (1, -1, 1)));
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
