#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

namespace {
// Expected values below were printed by the unchanged native
// initializer interpreter and patch applier, driven with
// seeded records. Every
// case uses MT19937 seed 20261003; the first birth is a sphere birth, so its
// initializer records start at word 5: lifetime 5, size 6, colour 7, alpha 8,
// velocity 9-11, rotation 12-14, angular velocity 15-17, size 18, rotation 19-21.
constexpr uint32_t oracleSeed = 20261003u;

std::mt19937 streamAt (uint32_t word) {
    std::mt19937 rng (oracleSeed);
    rng.discard (word);
    return rng;
}

float unitAt (uint32_t word) {
    auto rng = streamAt (word);
    return nativeRandomUnit (rng);
}

bool near (float actual, float expected, float relative = 1.0e-6f, float absolute = 1.0e-6f) {
    return std::abs (actual - expected) <= std::max (absolute, relative * std::abs (expected));
}
} // namespace

TEST_CASE ("Instance patches reproduce the original initializer and operator records",
           "[particle][initializer][instance-patch]") {
    // Records as rewritten (oracle REC lines).
    const auto lifetimeHalf = patchedInitializerRange (0.4f, 1.2f, 0.5f);         // case L05
    REQUIRE (lifetimeHalf.min == 0.200000003f);
    REQUIRE (lifetimeHalf.span == 0.400000036f);
    const auto lifetimeOne = patchedInitializerRange (0.4f, 1.2f, 1.0f);          // case M
    REQUIRE (lifetimeOne.min == 0.400000006f);
    REQUIRE (lifetimeOne.span == 0.800000072f);
    const glm::vec3 velocityMin (-10, -5, -2), velocityMax (10, 15, 3);
    const glm::vec3 patchedVelocityMin (-20, -10, -4), patchedVelocitySpan (40, 40, 10);  // case S2
    for (int axis = 0; axis < 3; ++axis) {
        const auto range = patchedInitializerRange (velocityMin[axis], velocityMax[axis], 2.0f);
        REQUIRE (range.min == patchedVelocityMin[axis]);
        REQUIRE (range.span == patchedVelocitySpan[axis]);
    }
    const auto angular = patchedInitializerRange (-3.0f, 4.0f, instanceSpeedGain (2.0f, 0u));
    REQUIRE (angular.min == -6.0f);
    REQUIRE (angular.span == 14.0f);
    // Movement gravity (op8) and angular movement force (op9) at speed 2.
    REQUIRE (-30.0f * instanceSpeedGain (2.0f, 0u) == -60.0f);
    REQUIRE (1.5f * instanceSpeedGain (2.0f, 0u) == 3.0f);
    // Preset flag 0x10 (case S2-flag10) omits every speed descriptor.
    REQUIRE (instanceSpeedGain (2.0f, 0x10u) == 1.0f);
    REQUIRE (instanceSpeedGain (0.0f, 0x11u) == 1.0f);
}

TEST_CASE ("Lifetime random follows the original exponent, lifetime patch and floor",
           "[particle][initializer][lifetime]") {
    const float u = unitAt (5);
    REQUIRE (near (lifetimeRandomSample (u, 0.4f, 1.2f, 1.0f, 1.0f, 0u), 0.625741065f));  // M
    REQUIRE (near (lifetimeRandomSample (u, 0.4f, 1.2f, 2.0f, 1.0f, 0u), 0.463698804f));  // E exponent 2
    REQUIRE (near (lifetimeRandomSample (u, 0.4f, 1.2f, 1.0f, 0.5f, 0u), 0.312870532f));  // L05
    // Instance lifetime 0 produces zero bounds; the interpreter floors to 0.001
    // so these particles expire on the next tick instead of living forever.
    REQUIRE (lifetimeRandomSample (u, 0.4f, 1.2f, 1.0f, 0.0f, 0u) == 0.00100000005f);   // L0
    REQUIRE (near (lifetimeRandomSample (u, 0.4f, 1.2f, 1.0f, 0.0f, 0x40u), 0.625741065f)); // L0 flag 0x40
    REQUIRE (lifetimeRandomSample (0.25f, 0.0f, 0.0f, 1.0f, 1.0f, 0u) == 0.001f);
    REQUIRE (std::isnan (lifetimeRandomSample (u, std::nanf (""), 1.0f, 1.0f, 1.0f, 0u)));
}

TEST_CASE ("Velocity random draws Z, Y, X with the record exponent and speed patch",
           "[particle][initializer][velocity]") {
    const glm::vec3 min (-10, -5, -2), max (10, 15, 3);
    auto linear = streamAt (9);
    auto shaped = streamAt (9);
    const glm::vec3 linearSample = velocityRandomSample (linear, min, max, 1.0f, 1.0f);
    const glm::vec3 shapedSample = velocityRandomSample (shaped, min, max, 2.0f, 1.0f);
    // Native velocity = emitter velocity + initializer sample, with identical
    // emitter draws in both cases; the difference isolates the initializer.
    const glm::vec3 oracleLinear (-4.8907299f, 5.27200413f, 0.349515557f);   // Mbirth-60hz
    const glm::vec3 oracleShaped (-5.67234993f, 0.644257545f, -0.857537866f); // E-exp-birth
    const glm::vec3 oracleDelta = oracleShaped - oracleLinear;
    for (int axis = 0; axis < 3; ++axis) {
        CAPTURE (axis);
        REQUIRE (near (shapedSample[axis] - linearSample[axis], oracleDelta[axis], 1.0e-5f, 1.0e-5f));
    }
    REQUIRE (linear () == shaped ());  // three words each

    // Native emitter speed and the velocity record scale together at speed 2.
    auto single = streamAt (9);
    auto doubled = streamAt (9);
    const glm::vec3 base = velocityRandomSample (single, min, max, 1.0f, instanceSpeedGain (1.0f, 0u));
    const glm::vec3 fast = velocityRandomSample (doubled, min, max, 1.0f, instanceSpeedGain (2.0f, 0u));
    const glm::vec3 oracleFast (-9.78145981f, 10.5440083f, 0.699031115f);       // S2-birth
    const glm::vec3 oracleResidual = oracleFast - 2.0f * oracleLinear;
    for (int axis = 0; axis < 3; ++axis) {
        CAPTURE (axis);
        REQUIRE (near (fast[axis] - 2.0f * base[axis], oracleResidual[axis], 0.0f, 1.0e-5f));
    }
}

TEST_CASE ("Rotation and angular velocity random honour exponent and instance speed",
           "[particle][initializer][angular]") {
    const glm::vec3 rotationMin (0, 0, -1), rotationMax (0, 0, 2);
    const glm::vec3 duplicateMin (0, 0, 0.25f), duplicateMax (0, 0, 0.75f);
    for (const auto& [exponent, expected] : {std::pair {1.0f, 2.36410284f}, std::pair {1.5f, 2.2769804f}}) {
        CAPTURE (exponent);
        auto rng = streamAt (12);
        float rotation = 0.0f;
        addAngularSample (rotation, angularRandomSample (rng, rotationMin, rotationMax, exponent, 1.0f).z);
        rng.discard (4);  // angular velocity (15-17) and the duplicate size record (18)
        addAngularSample (rotation, angularRandomSample (rng, duplicateMin, duplicateMax, 1.0f, 1.0f).z);
        REQUIRE (near (rotation, expected));
    }

    const glm::vec3 angularMin (0, 0, -3), angularMax (0, 0, 4);
    const auto angularAt = [&] (float exponent, float gain) {
        auto rng = streamAt (15);
        return angularRandomSample (rng, angularMin, angularMax, exponent, gain).z;
    };
    REQUIRE (near (angularAt (1.0f, instanceSpeedGain (1.0f, 0u)), -1.48457479f));    // S1-birth
    REQUIRE (near (angularAt (0.7f, instanceSpeedGain (1.0f, 0u)), -0.601699591f));   // E-exp-birth
    REQUIRE (near (angularAt (1.0f, instanceSpeedGain (2.0f, 0u)), -2.96914959f));    // S2-birth
    REQUIRE (near (angularAt (1.0f, instanceSpeedGain (2.0f, 0x10u)), -1.48457479f)); // flag 0x10
    // powf (u, 0) is one for every unit, including zero: exponent 0 selects max.
    REQUIRE (initializerLaneUnit (0.0f, 0.0f) == 1.0f);
    REQUIRE (initializerLaneUnit (0.375f, 1.0f) == 0.375f);
}

TEST_CASE ("Duplicate size, colour and alpha records compose like the original streams",
           "[particle][initializer][duplicate]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::AlphaRandomInitializer;
    using WallpaperEngine::Data::Model::ColorRandomInitializer;
    using WallpaperEngine::Data::Model::LifetimeRandomInitializer;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::RotationRandomInitializer;
    using WallpaperEngine::Data::Model::SizeRandomInitializer;
    using WallpaperEngine::Data::Model::VelocityRandomInitializer;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"duplicates","particle":{"initializer":[
        {"name":"sizerandom","min":5,"max":15,"exponent":0.5},
        {"name":"colorrandom"},
        {"name":"alpharandom"},
        {"name":"lifetimerandom","exponent":2},
        {"name":"velocityrandom","exponent":0.75},
        {"name":"rotationrandom"},
        {"name":"sizerandom","min":0.5,"max":2},
        {"name":"colorrandom"},
        {"name":"alpharandom"}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    const auto& records = particle->initializers;
    REQUIRE (records.size () == 9);
    REQUIRE_FALSE (followsSameInitializer (records, *records[0]->as<SizeRandomInitializer> ()));
    REQUIRE_FALSE (followsSameInitializer (records, *records[1]->as<ColorRandomInitializer> ()));
    REQUIRE_FALSE (followsSameInitializer (records, *records[2]->as<AlphaRandomInitializer> ()));
    REQUIRE (followsSameInitializer (records, *records[6]->as<SizeRandomInitializer> ()));
    REQUIRE (followsSameInitializer (records, *records[7]->as<ColorRandomInitializer> ()));
    REQUIRE (followsSameInitializer (records, *records[8]->as<AlphaRandomInitializer> ()));
    // Factory reads exponent for lifetime, velocity and rotation records.
    REQUIRE (records[3]->as<LifetimeRandomInitializer> ()->exponent->value->getFloat () == 2.0f);
    REQUIRE (records[4]->as<VelocityRandomInitializer> ()->exponent->value->getFloat () == 0.75f);
    REQUIRE (records[5]->as<RotationRandomInitializer> ()->exponent->value->getFloat () == 1.0f);

    // Oracle E-exp-birth: the ordinary first record yields s1/2 (native .5 * s1)
    // and the second multiplies that baseline: native .5 * s1 * s2.
    const float first = 5.0f + std::pow (unitAt (6), 0.5f) * 10.0f;
    const float second = 0.5f + unitAt (18) * 1.5f;
    const float ordinaryBaseline = first / 2.0f;
    REQUIRE (near (ordinaryBaseline * second, 12.0432024f));
}

TEST_CASE ("Instance count patches emitter rate and per-period limit together",
           "[particle][emitter][instance-patch]") {
    EmitterScheduleConfig authored;
    authored.rate = 37.0f;
    authored.maxPerPeriod = 7;
    EmitterScheduleConfig active = authored;
    applyInstanceCount (active, authored, 0.5f, 0u);
    REQUIRE (active.rate == 18.5f);            // oracle C05
    REQUIRE (active.maxPerPeriod == 3u);       // (int)(7 * .5f)
    applyInstanceCount (active, authored, 0.0f, 0x20u);
    REQUIRE (active.rate == 37.0f);            // oracle C0-flag20
    REQUIRE (active.maxPerPeriod == 7u);
    applyInstanceCount (active, authored, 0.0f, 0u);
    REQUIRE (active.rate == 0.0f);             // oracle C0
    REQUIRE (active.maxPerPeriod == 0u);
    // op1: (int)(int64)((float)authored * count).
    REQUIRE (instanceMaxPerPeriod (5u, 0.1f) == 0u);
    REQUIRE (instanceMaxPerPeriod (5u, 2.5f) == 12u);
    REQUIRE (instanceMaxPerPeriod (16777217u, 1.0f) == 16777216u);
    REQUIRE (instanceMaxPerPeriod (3u, -1.0f) == 0xfffffffdu);
    REQUIRE (instanceMaxPerPeriod (3u, std::numeric_limits<float>::quiet_NaN ()) == 0u);
    REQUIRE (instanceMaxPerPeriod (3u, std::numeric_limits<float>::infinity ()) == 0u);
}
