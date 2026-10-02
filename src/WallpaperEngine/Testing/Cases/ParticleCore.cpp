#include "WallpaperEngine/Render/Objects/ParticleCore.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/ParticleRemapOperators.h"
#include "WallpaperEngine/Render/Objects/ParticleSlotStreams.h"
#include "WallpaperEngine/Render/Objects/ParticleNativeSlotScope.h"
#include "WallpaperEngine/Render/Shaders/ParticleRopeShader.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"
#include "WallpaperEngine/Render/Utils/NativeParticleGradientNoise.h"
#include "WallpaperEngine/Render/Wallpapers/ParticleSceneClock.h"
#include "WallpaperEngine/Maths.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Scripting/ParticleScriptBindings.h"

#include <array>
#include <cfenv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

TEST_CASE ("Emitter batch captures CP before ordered birth writes and next invocation sees accumulated CP",
           "[particle][birth-batch]") {
    using namespace WallpaperEngine::Render::Objects;
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto parsed = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{
        "initializer":[{"name":"remapinitialvalue","input":"maxlifetime",
            "inputrangemin":0,"inputrangemax":20,"output":"controlpoint",
            "outputcontrolpoint0":0,"outputcomponent":"x","outputrangemin":"24 0 0",
            "outputrangemax":"24 0 0","operation":"add","flags":1}]}})"), project);
    const auto& model = *parsed->as<Particle> ();
    const auto writer = createVectorRemapOperator (*model.initializers[0]
        ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> (),
        true, false, false, false);
    std::vector<ControlPointData> cps (1);
    cps[0].flags = model.controlPoints[0].flags;
    cps[0].basis = glm::mat3 (2, 0, 0, 0, 3, 0, 0, 0, 4);
    const glm::vec3 emitterOrigin (3, 5, 7);
    const auto batch = captureEmitterBirthTransform (emitterOrigin, &cps[0].position, &cps[0].basis);
    std::vector<ParticleInstance> particle (1);
    particle[0].alive = true;
    particle[0].lifetime = particle[0].initial.lifetime = 8;
    std::array<glm::vec3, 2> births;
    for (size_t i = 0; i < births.size (); ++i) {
        births[i] = batch.origin;
        writer (particle, 1, cps, 0, MovementTime {});
        REQUIRE (cps[0].position.x == 24 * (i + 1));
    }
    REQUIRE (births[0] == emitterOrigin);
    REQUIRE (births[1] == emitterOrigin);
    // Capturing again observes both CP writes. Capturing on every birth would
    // put the second at 24, the independently observed old Linux behavior.
    const auto next = captureEmitterBirthTransform (emitterOrigin, &cps[0].position, &cps[0].basis);
    REQUIRE (next.origin == emitterOrigin + glm::vec3 (48, 0, 0));
    const glm::vec3 displacement (1, 2, 3);
    cps[0].basis = glm::mat3 (1.0f);
    REQUIRE (batch.basis * displacement == glm::vec3 (2, 6, 12));
    REQUIRE (next.basis * displacement == glm::vec3 (2, 6, 12));
    const auto uncoupled = captureEmitterBirthTransform (emitterOrigin, nullptr, nullptr);
    REQUIRE (uncoupled.origin == emitterOrigin);
    REQUIRE (uncoupled.basis * displacement == displacement);
}

TEST_CASE ("Native physical streams retain holes and padded tail across restore and stop",
           "[particle][sparse][slots]") {
    using WallpaperEngine::Render::Objects::ParticleInstance;
    ParticleInstance zero;
    zero.color = zero.initial.color = glm::vec3 (0.0f);
    zero.alpha = zero.initial.alpha = zero.size = zero.initial.size = 0.0f;
    zero.lifetime = zero.initial.lifetime = 0.0f;
    NativeSlotStreams<ParticleInstance> slots (6, zero);
    REQUIRE (slots.streams ().size () == 8);
    REQUIRE (slots.capacity () == 6);
    slots.beginEmissionPass ();
    for (uint32_t index = 0; index < 6; ++index) {
        REQUIRE (slots.birth ([index] (ParticleInstance& p, uint32_t physical) {
            REQUIRE (physical == index);
            p.lifetime = index == 4 ? 0.25f : 4.0f;
            p.alive = true;
            p.position.x = 100.0f + static_cast<float> (physical);
            p.velocity.x = 8.0f;
            p.size = 5.0f;
            p.initial.size = 2.0f;
        }) == index);
    }
    REQUIRE (slots.nativeCount () == 6);
    std::vector<uint32_t> expired;
    slots.ageAndExpire (0.25f, [&] (const auto&, uint32_t index) { expired.push_back (index); });
    REQUIRE (expired.empty ()); // Native expires only strict age > lifetime.
    slots.ageAndExpire (0.125f, [&] (const auto&, uint32_t index) { expired.push_back (index); });
    REQUIRE (expired == std::vector<uint32_t> {4});
    REQUIRE (slots.highWater () == 6);
    REQUIRE (slots.nativeCount () == 5);
    REQUIRE (slots.streams ()[4].position.x == 104.0f);
    REQUIRE (slots.streams ()[7].age == 0.375f);
    // Unmasked operators execute dead and padded lanes; a later writer reads
    // physical lane four, while the dense view's fifth record is slot five.
    for (uint32_t index = 0; index < slots.paddedHighWater (); ++index) {
        auto& p = slots.streams ()[index];
        p.position += p.velocity * 0.5f;
        p.size += 3.0f;
    }
    slots.restore ([] (ParticleInstance& p) { p.size = p.initial.size; });
    REQUIRE (slots.streams ()[4].position.x == 108.0f);
    REQUIRE (slots.streams ()[4].size == 2.0f);
    REQUIRE (slots.streams ()[7].size == 3.0f);
    std::vector<uint32_t> drawn;
    slots.visitDrawSlots ([&] (const auto&, uint32_t index) { drawn.push_back (index); });
    REQUIRE (drawn == std::vector<uint32_t> {0, 1, 2, 3, 5});
    slots.beginEmissionPass ();
    REQUIRE (slots.birth ([] (ParticleInstance& p, uint32_t index) {
        REQUIRE (index == 4);
        REQUIRE (p.position.x == 108.0f); // Reuse occurs before birth overwrite.
        p.position.x = 204.0f;
        p.lifetime = 2.0f;
        p.age = 0.0f;
        p.alive = true;
    }) == 4);
    REQUIRE (slots.highWater () == 6);
    REQUIRE (slots.nativeCount () == 6);
    REQUIRE_FALSE (slots.birth ([] (auto&, uint32_t) {}));
    slots.stop ();
    REQUIRE (slots.highWater () == 0);
    REQUIRE (slots.nativeCount () == 0);
    REQUIRE (slots.streams ()[4].position.x == 204.0f);
    REQUIRE (slots.streams ()[7].size == 3.0f);
    REQUIRE (slots.streams ()[7].age == 0.375f);
    for (const auto& p : slots.streams ()) REQUIRE (p.lifetime == 0.0f);
    REQUIRE (slots.birth ([] (ParticleInstance& p, uint32_t index) {
        REQUIRE (index == 0);
        REQUIRE (p.position.x == 104.0f);
        p.lifetime = 1.0f;
    }) == 0);
}

TEST_CASE ("Native birth cursor and count are independent of zero and nonfinite markers",
           "[particle][sparse][slots]") {
    using WallpaperEngine::Render::Objects::ParticleInstance;
    ParticleInstance zero;
    zero.lifetime = 0.0f;
    NativeSlotStreams<ParticleInstance> slots (8, zero);
    slots.beginEmissionPass ();
    for (uint32_t index = 0; index < 3; ++index) {
        REQUIRE (slots.birth ([&] (ParticleInstance& p, uint32_t physical) {
            REQUIRE (physical == index);
            REQUIRE (slots.nativeCount () == index);
            REQUIRE (slots.highWater () == index);
            p.lifetime = index == 0 ? 0.0f : index == 1 ? -1.0f
                : std::numeric_limits<float>::quiet_NaN ();
        }) == index);
    }
    REQUIRE (slots.nativeCount () == 3);
    REQUIRE (slots.highWater () == 3);
    std::vector<uint32_t> drawn;
    slots.visitDrawSlots ([&] (const auto&, uint32_t index) { drawn.push_back (index); });
    REQUIRE (drawn == std::vector<uint32_t> {1, 2});
    std::vector<uint32_t> expired;
    slots.ageAndExpire (0.125f, [&] (const auto&, uint32_t index) { expired.push_back (index); });
    REQUIRE (expired == std::vector<uint32_t> {1});
    REQUIRE (slots.nativeCount () == 2); // The invisible zero birth still counts.
    REQUIRE (std::isnan (slots.streams ()[2].lifetime));
    REQUIRE (slots.streams ()[3].age == 0.125f);
    slots.beginEmissionPass ();
    REQUIRE (slots.birth ([] (ParticleInstance& p, uint32_t index) {
        REQUIRE (index == 0);
        REQUIRE (p.age == 0.125f);
        p.lifetime = 0.0f;
        p.age = 0.0f;
    }) == 0);
    // The cursor is shared by subsequent emitter records in this pass.
    REQUIRE (slots.birth ([] (ParticleInstance& p, uint32_t index) {
        REQUIRE (index == 1);
        p.lifetime = 1.0f;
    }) == 1);
    REQUIRE (slots.birth ([] (ParticleInstance& p, uint32_t index) {
        REQUIRE (index == 3); // NaN marker at two is occupied.
        p.lifetime = 1.0f;
    }) == 3);
    REQUIRE (slots.nativeCount () == 5);
    REQUIRE (slots.highWater () == 4);
}

TEST_CASE ("Parsed sparse control-point output uses the production capability boundary",
           "[particle][sparse][parser]") {
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto authored = JSON::parse (R"({"id":1,"parent":70,"particle":{
        "maxcount":8,"emitter":[{"name":"sphererandom","rate":0},{"name":"boxrandom","rate":0}],
        "initializer":[{"name":"lifetimerandom","min":-2,"max":0},
            {"name":"sizerandom","min":4,"max":4},
            {"name":"mapsequencebetweencontrolpoints","count":6},
            {"name":"remapinitialvalue","input":"position","inputcomponent":"x",
             "output":"maxlifetime","operation":"remap","flags":1,
             "inputrangemin":36,"inputrangemax":60,"outputrangemin":1,"outputrangemax":20}],
        "operator":[{"name":"movement"},{"name":"capvelocity","maxspeed":10},
            {"name":"remapvalue","input":"position","output":"controlpoint","outputcontrolpoint0":2,
             "operation":"remap","flags":0},
            {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,"output":"color",
             "operation":"remap","flags":0}],"renderer":[{"name":"sprite"}]}})");
    const auto check = [&] (const JSON& data) {
        return ObjectParser::parse (data, project);
    };
    const auto valid = check (authored);
    const auto& model = *valid->as<Particle> ();
    REQUIRE (needsNativeSlotStreams (model));
    REQUIRE (model.emitters[0].name == "sphererandom");
    REQUIRE (model.emitters[1].name == "boxrandom");
    REQUIRE_FALSE (nativeSlotScopeError (model)); // Scene transform parent is valid.
    REQUIRE (nativeSlotScopeError (model, true));
    REQUIRE (nativeSlotScopeError (model, false, true));
    const auto owned = std::find_if (model.controlPoints.begin (), model.controlPoints.end (),
        [] (const auto& cp) { return cp.id == 2; });
    REQUIRE (owned != model.controlPoints.end ());
    REQUIRE ((owned->flags & 0x10000u) != 0);

    for (const char* transform : {"sine", "square", "saw", "triangle", "simplexnoise", "fbmnoise"}) {
        auto data = authored;
        data["particle"]["operator"][2]["transformfunction"] = transform;
        const auto parsed = check (data);
        REQUIRE (nativeSlotScopeError (*parsed->as<Particle> ()));
    }
    for (const char* renderer : {"spritetrail", "rope", "ropetrail", "unknown"}) {
        auto data = authored;
        data["particle"]["renderer"][0]["name"] = renderer;
        const auto parsed = check (data);
        REQUIRE (nativeSlotScopeError (*parsed->as<Particle> ()));
    }
    for (const char* name : {"turbulence", "angularmovement", "alphafade", "boids"}) {
        auto data = authored;
        data["particle"]["operator"].push_back ({{"name", name}});
        const auto parsed = check (data);
        REQUIRE (nativeSlotScopeError (*parsed->as<Particle> ()));
    }
    auto event = authored;
    event["particle"]["initializer"].push_back ({{"name", "inheritinitialvaluefromevent"}});
    const auto parsedEvent = check (event);
    REQUIRE (nativeSlotScopeError (*parsedEvent->as<Particle> ()));
    auto unproven = authored;
    unproven["particle"]["initializer"][3]["outputrangemin"] = 0;
    const auto parsedUnproven = check (unproven);
    REQUIRE (nativeSlotScopeError (*parsedUnproven->as<Particle> ()));
    auto& positive = *valid->as<Particle> ()->initializers[3]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ();
    positive.outputMin = std::numeric_limits<float>::quiet_NaN ();
    REQUIRE (nativeSlotScopeError (model));
}

TEST_CASE ("Canonical ordinary birth remaps diagnose nonfinite state without changing birth division",
           "[particle][sparse][birth]") {
    using namespace WallpaperEngine::Render::Objects;
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto parsed = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{
        "initializer":[{"name":"remapinitialvalue","input":"position","inputcomponent":"x",
            "output":"maxlifetime","operation":"remap","flags":1,
            "inputrangemin":0,"inputrangemax":10,"outputrangemin":1,"outputrangemax":20}],
        "operator":[{"name":"remapvalue","input":"position","output":"controlpoint",
            "outputcontrolpoint0":2,"operation":"remap","flags":0}]}})"), project);
    const auto& model = *parsed->as<Particle> ();
    const auto& birth = *model.initializers[0]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ();
    const auto canonical = createScalarRemapOperator (birth, true, false, false, false, true);
    const auto compact = createScalarRemapOperator (birth, true, false, false, false);
    std::vector<ParticleInstance> particles (1);
    std::vector<ControlPointData> cps (3);
    particles[0].alive = true;
    particles[0].position.x = 7.0f;
    particles[0].lifetime = 1.0f;
    canonical (particles, 1, cps, 0.0f, {0.0f, 0.0f});
    const float birthDivide = 7.0f / 10.0f;
    REQUIRE (particles[0].lifetime == 1.0f + birthDivide * 19.0f);
    if (nativeRuntimeArithmeticAvailable)
        REQUIRE (particles[0].lifetime != 1.0f + (7.0f * nativeRuntimeReciprocal (10.0f)) * 19.0f);

    particles[0].lifetime = 1.0f;
    particles[0].position.x = std::numeric_limits<float>::quiet_NaN ();
    REQUIRE_THROWS_WITH (canonical (particles, 1, cps, 0.0f, {0.0f, 0.0f}),
        "Runtime control-point output nonfinite ordinary birth remap is unsupported");
    REQUIRE (particles[0].lifetime == 1.0f);
    REQUIRE_NOTHROW (compact (particles, 1, cps, 0.0f, {0.0f, 0.0f}));
    REQUIRE (particles[0].lifetime == 1.0f); // Accepted compact finite wrapper remains unchanged.

    const float huge = std::numeric_limits<float>::max ();
    ScalarRemapRange overflowing {-huge, huge, 1.0f, 20.0f, 1};
    REQUIRE_THROWS_AS (remapScalarValue (1.0f, huge, 0.0f, RemapOperation::Set,
        std::nullopt, overflowing, false, false, false, true), std::invalid_argument);
    ScalarRemapRange overflowingSquare {0.0f, 1.0f, 1.0f, 20.0f, 0,
        RemapTransform::Square, huge};
    REQUIRE_THROWS_AS (remapScalarValue (1.0f, 2.0f, 0.0f, RemapOperation::Set,
        std::nullopt, overflowingSquare, false, false, false, true), std::invalid_argument);
    const float nan = std::numeric_limits<float>::quiet_NaN ();
    REQUIRE (std::isnan (remapScalarValue (1.0f, nan, 0.0f, RemapOperation::Set,
        std::nullopt, {}, true, false, false, true))); // Existing exact birth CP path retains NaN.
    REQUIRE (std::isnan (remapScalarValue (1.0f, nan, 0.0f, RemapOperation::Set,
        std::nullopt, {}, false, true, false, true))); // Existing exact noise route remains unchecked.

    auto& vectorBirth = *model.operators[0]->as<VectorRemapValueOperator> ();
    vectorBirth.inputComponent = VectorRemapValueOperator::InputComponent::Min;
    const auto vectorCanonical = createVectorRemapOperator (vectorBirth, true, false, false, false, true);
    particles[0].position = glm::vec3 (1.0f, nan, 3.0f);
    REQUIRE_THROWS_AS (vectorCanonical (particles, 1, cps, 0.0f, {0.0f, 0.0f}), std::invalid_argument);
    // A nonfinite denominator must be diagnosed before the compact geometry
    // helper turns it into zero and silently admits the birth.
    auto& betweenBirth = *model.initializers[0]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> ();
    betweenBirth.input = ScalarRemapValueOperator::Input::PositionBetweenTwoControlPoints;
    betweenBirth.inputControlPoint0 = 0;
    betweenBirth.inputControlPoint1 = 1;
    const auto betweenCanonical = createScalarRemapOperator (betweenBirth, true, false, false, false, true);
    particles[0].position = glm::vec3 (1.0f);
    cps[1].position = glm::vec3 (huge);
    REQUIRE_THROWS_AS (betweenCanonical (particles, 1, cps, 0.0f, {0.0f, 0.0f}), std::invalid_argument);
}

TEST_CASE ("Runtime control-point writers read physical lane zero and ordered feedback",
           "[particle][sparse][remap]") {
    if (!nativeRuntimeArithmeticAvailable) return;
    using namespace WallpaperEngine::Render::Objects;
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto parsed = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{"operator":[
        {"name":"remapvalue","input":"position","output":"controlpoint","outputcontrolpoint0":2,
         "operation":"remap","flags":0},
        {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,"output":"size",
         "operation":"remap","flags":0},
        {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,"output":"controlpoint",
         "outputcontrolpoint0":2,"operation":"add","flags":0}]}})"), project);
    const auto& model = *parsed->as<Particle> ();
    const auto writer = createVectorRemapOperator (*model.operators[0]->as<VectorRemapValueOperator> (),
        false, false, false, false, true);
    const auto reader = createScalarRemapOperator (*model.operators[1]->as<ScalarRemapValueOperator> (),
        false, false, false, false, true);
    const auto feedback = createVectorRemapOperator (*model.operators[2]->as<VectorRemapValueOperator> (),
        false, false, false, false, true);
    std::vector<ParticleInstance> particles (8);
    for (uint32_t index = 0; index < 8; ++index) {
        auto& p = particles[index];
        p.position = glm::vec3 (10.0f * index, 0.0f, 0.0f);
        p.alive = index < 6 && index != 4;
        p.lifetime = p.alive ? 20.0f : 0.0f;
        p.age = 3.0f;
    }
    std::vector<ControlPointData> cps (8);
    cps[2].basis = glm::mat3 (2.0f);
    cps[2].offset = glm::vec3 (7.0f);
    cps[2].previousPosition = glm::vec3 (9.0f);
    writer (particles, 8, cps, 0.0f, {0.0f, 0.0f});
    // Independent native factory RCPPS(1) and physical lane4 source40. A
    // compact index4 would be the live slot5 source50; a tail lane is70.
    const float reciprocal = nativeRuntimeReciprocal (1.0f);
    REQUIRE (cps[2].position.x == 40.0f * reciprocal);
    REQUIRE (cps[2].basis == glm::mat3 (2.0f));
    REQUIRE (cps[2].offset == glm::vec3 (7.0f));
    REQUIRE (cps[2].previousPosition == glm::vec3 (9.0f));
    reader (particles, 8, cps, 0.0f, {0.0f, 0.0f});
    REQUIRE (particles[4].size == 40.0f * reciprocal * reciprocal);
    REQUIRE (particles[7].size == particles[4].size);
    cps[2].position = glm::vec3 (5.0f, 0.0f, 0.0f);
    feedback (particles, 8, cps, 0.0f, {0.0f, 0.0f});
    const float first = 5.0f + 5.0f * reciprocal;
    REQUIRE (cps[2].position.x == first + first * reciprocal);
    // A five-slot pool still executes block4 even though three lanes are padding.
    writer (particles, 5, cps, 0.0f, {0.0f, 0.0f});
    REQUIRE (cps[2].position.x == 40.0f * reciprocal);
}

TEST_CASE ("Sparse runtime IEEE fractions clamps reductions and ranges retain native semantics",
           "[particle][sparse][remap][ieee]") {
    if (!nativeRuntimeArithmeticAvailable) return;
    const float nan = std::bit_cast<float> (0x7fc12345u);
    REQUIRE (std::isinf (nativeRuntimeLifetimeFraction (1.0f, 0.0f)));
    REQUIRE (std::isnan (nativeRuntimeLifetimeFraction (0.0f, 0.0f)));
    REQUIRE (nativeRuntimeClamp01 (nan) == 1.0f);
    REQUIRE (std::signbit (nativeRuntimeClamp01 (-0.0f)));
    REQUIRE (reduceNativeRuntimeRemapVector (nan, 2.0f, 1.0f, RemapVectorComponent::Max) == 2.0f);
    REQUIRE (reduceNativeRuntimeRemapVector (2.0f, nan, 1.0f, RemapVectorComponent::Max) == 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (reduceNativeRuntimeRemapVector (
        2.0f, 1.0f, nan, RemapVectorComponent::Max)) == 0x7fc12345u);
    const BlendEnvelope envelope {0.0f, 0.25f, 0.75f, 1.0f};
    REQUIRE (blendWeight (nativeRuntimeLifetimeFraction (1.0f, 0.0f), envelope, true) == 0.0f);
    REQUIRE (blendWeight (nativeRuntimeLifetimeFraction (0.0f, 0.0f), envelope, true) == 1.0f);
    REQUIRE (nativeRuntimePositionBetweenControlPoints ({1, 2, 3}, {}, {}) == 0.0f);
    REQUIRE (std::isnan (nativeRuntimeCapVelocityFactor (0, 0, 0, 0, 0, std::nullopt)));
    REQUIRE (nativeRuntimeCapVelocityFactor (0, 0, 0, 1, 0, std::nullopt) == 1.0f);
    REQUIRE (nativeRuntimeCapVelocityFactor (3, 4, 0, 2,
        nativeRuntimeLifetimeFraction (1, 0), envelope) == 1.0f);
    REQUIRE (nativeRuntimeCapVelocityFactor (3, 4, 0, 2, 0, std::nullopt)
        == Catch::Approx (0.4f).epsilon (0.001f));
    ScalarRemapRange range {0, 3, 0, 1, 0};
    REQUIRE (remapScalarValue (0.0f, 1.0f, nan, RemapOperation::Set,
        std::nullopt, range, false, false, true) == nativeRuntimeReciprocal (3.0f));
    REQUIRE (remapScalarValue (4.0f, 1.0f, nan, RemapOperation::Set,
        std::nullopt, range) == 4.0f); // Accepted compact finite guard remains.
}

TEST_CASE ("Blended runtime control-point output preserves native dead-lane envelope cases",
           "[particle][sparse][remap][blend]") {
    if (!nativeRuntimeArithmeticAvailable) return;
    using namespace WallpaperEngine::Render::Objects;
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"particle":{"operator":[
        {"name":"remapvalue","input":"lifetimefraction","output":"controlpoint","outputcontrolpoint0":2,
         "outputcomponent":"x","operation":"remap","flags":1,"outputrangemin":3,"outputrangemax":7,
         "blendinend":0.25,"blendoutstart":0.75}]}})");
    std::vector<ParticleInstance> particles (8);
    particles[0].age = 0.0f;
    particles[0].lifetime = 1.0f;
    particles[4].alive = false;
    particles[4].lifetime = 0.0f;
    std::vector<ControlPointData> cps (8);
    for (const char* operation : {"remap", "multiply", "add", "subtract"}) {
        data["particle"]["operator"][0]["operation"] = operation;
        const auto parsed = ObjectParser::parse (data, project);
        const auto& model = *parsed->as<Particle> ()->operators[0]->as<VectorRemapValueOperator> ();
        const auto writer = createVectorRemapOperator (model, false, false, false, false, true);
        cps[2].position = glm::vec3 (9.0f, 20.0f, 30.0f);
        particles[4].age = 1.0f;
        writer (particles, 8, cps, 0.0f, {0.0f, 0.0f});
        REQUIRE (cps[2].position == glm::vec3 (9.0f, 20.0f, 30.0f));
        particles[4].age = 0.0f;
        writer (particles, 8, cps, 0.0f, {0.0f, 0.0f});
        const float expected = std::string (operation) == "remap" ? 7.0f
            : std::string (operation) == "multiply" ? 63.0f
            : std::string (operation) == "add" ? 16.0f : 2.0f;
        REQUIRE (cps[2].position == glm::vec3 (expected, 20.0f, 30.0f));
    }
    data["particle"]["operator"][0]["operation"] = "remap";
    data["particle"]["operator"][0]["outputrangemin"] = 7.0f;
    data["particle"]["operator"][0]["outputrangemax"] = 7.0f;
    data["particle"]["operator"][0]["blendinstart"] = 0.1f;
    data["particle"]["operator"][0]["blendinend"] = 0.4f;
    data["particle"]["operator"][0]["blendoutstart"] = 0.6f;
    data["particle"]["operator"][0]["blendoutend"] = 0.9f;
    const auto parsed = ObjectParser::parse (data, project);
    const auto writer = createVectorRemapOperator (*parsed->as<Particle> ()->operators[0]
        ->as<VectorRemapValueOperator> (), false, false, false, false, true);
    particles[0].age = 0.25f;
    cps[2].position.x = 9.0f;
    writer (particles, 1, cps, 0.0f, {0.0f, 0.0f});
#if defined(__SSE__)
    // Independent instruction oracle pinned to native 1401c2a40/14022a530:
    // pack both adjusted durations with RCPPS, then OUT*IN and CP lerp. This
    // avoids fixing an approximation to one processor's reciprocal table.
    const auto rcp = _mm_rcp_ps (_mm_setr_ps (0.4f - 0.1f, 0.9f - 0.6f, 1.0f, 1.0f));
    alignas (16) float inverse[4];
    _mm_store_ps (inverse, rcp);
    const float fraction = 0.25f * inverse[2];
    const auto ramps = _mm_mul_ps (_mm_setr_ps (fraction - 0.1f, 0.9f - fraction, 0, 0), rcp);
    const auto clamped = _mm_max_ps (_mm_setzero_ps (), _mm_min_ps (ramps, _mm_set1_ps (1.0f)));
    alignas (16) float weights[4];
    _mm_store_ps (weights, clamped);
    const float expected = 9.0f + (7.0f - 9.0f) * (weights[1] * weights[0]);
    REQUIRE (std::bit_cast<uint32_t> (cps[2].position.x) == std::bit_cast<uint32_t> (expected));
    REQUIRE (cps[2].position.x != 8.0f); // Division-based envelope counterfactual.
#endif
    particles[0].lifetime = 0.0f;
    particles[0].age = 0.0f;
    cps[2].position.x = 9.0f;
    writer (particles, 1, cps, 0.0f, {0.0f, 0.0f});
    REQUIRE (cps[2].position.x == 7.0f);
    particles[0].age = 1.0f;
    cps[2].position.x = 9.0f;
    writer (particles, 1, cps, 0.0f, {0.0f, 0.0f});
    REQUIRE (cps[2].position.x == 9.0f);
}

TEST_CASE ("Native control-point attraction uses full-radius falloff and default overshoot cap",
           "[particle][controlpoint][attract]") {
    const glm::vec3 center (10.0f, 20.0f, 0.0f);
    const glm::vec3 particle (40.0f, 20.0f, 0.0f);
    // At distance 30 the old half-threshold implementation applied no force.
    // Native opcode 0x0a instead uses 1 - 30/50 across the full radius.
    REQUIRE (controlPointAttractVelocityDelta (particle, center, 100.0f, 50.0f,
                                                {0.1f, 0.1f}, 2u).x == Catch::Approx (-4.0f));
    REQUIRE (controlPointAttractVelocityDelta (particle, center, -100.0f, 50.0f,
                                                {0.1f, 0.1f}, 2u).x == Catch::Approx (4.0f));
    REQUIRE (controlPointAttractVelocityDelta (particle, center, 100.0f, 30.0f,
                                                {0.1f, 0.1f}, 2u) == glm::vec3 (0.0f));
    REQUIRE (controlPointAttractVelocityDelta (center, center, 100.0f, 50.0f,
                                                {0.1f, 0.1f}, 2u) == glm::vec3 (0.0f));

    const glm::vec3 near (11.0f, 20.0f, 0.0f);
    REQUIRE (controlPointAttractVelocityDelta (near, center, 100.0f, 50.0f,
                                                {0.1f, 0.1f}, 2u).x == Catch::Approx (-1.0f));
    REQUIRE (controlPointAttractVelocityDelta (near, center, 100.0f, 50.0f,
                                                {0.1f, 0.1f}, 0u).x == Catch::Approx (-9.8f));
    REQUIRE (controlPointAttractVelocityDelta (near, center, -100.0f, 50.0f,
                                                {0.1f, 0.1f}, 2u).x == Catch::Approx (9.8f));
    // Native opcode 0x0a consumes the damping clock q, not integration h.
    // This differs when a slow frame produces h != q. The helper's typed
    // signature also requires the CParticle operator to forward both clocks.
    REQUIRE (controlPointAttractVelocityDelta (particle, center, 100.0f, 50.0f,
                                                {0.2f, 0.05f}, 2u).x == Catch::Approx (-2.0f));
}

TEST_CASE ("Shared particle RGB gain follows native birth gates", "[particle][instance][color]") {
    const glm::vec3 tint (0.25f, 0.5f, 1.0f);
    const glm::vec3 preset (0.75f, 0.75f, 0.75f);
    const auto gain = [&] (glm::vec3 rootTint, glm::vec3 rootPreset,
                           glm::vec3 nodePreset, float brightness,
                           uint32_t rootFlags, uint32_t nodeFlags,
                           bool tintCompiled, bool isRoot, bool hdr) {
        return particleInstanceBirthRgbGain (rootTint, rootPreset, nodePreset,
            brightness, rootFlags, nodeFlags, tintCompiled, isRoot, hdr);
    };
    // 1401d15a0 uses the HDR root brightness and the compiled color flag
    // as independent RGB factors. It leaves alpha and material overbright to
    // their own streams.
    REQUIRE (gain (tint, preset, preset, 2.0f, 0, 0, true, false, true)
             == glm::vec3 (0.5f, 1.0f, 2.0f));
    REQUIRE (gain (tint, preset, preset, 2.0f, 0, 0, true, false, false)
             == tint);
    REQUIRE (gain (tint, preset, preset, 2.0f, 0, 0, false, false, true)
             == glm::vec3 (2.0f));
    REQUIRE (gain (tint, preset, preset, 2.0f, 0, 0x200000u, true, false, true)
             == glm::vec3 (0.5f, 1.0f, 2.0f));
    REQUIRE (gain (tint, preset, preset, 2.0f, 0, 8u, false, false, true)
             == glm::vec3 (1.0f));
    REQUIRE (gain (tint, preset, preset, 2.0f, 8u, 0, false, false, true)
             == glm::vec3 (2.0f));
    REQUIRE (gain (glm::vec3 (-1.0f), preset, preset, 2.0f,
                  0, 0, false, false, true) == glm::vec3 (2.0f));
    REQUIRE (gain (tint, tint, preset, 2.0f, 0, 0, false, false, true)
             == glm::vec3 (2.0f));
    REQUIRE (gain (tint, preset, tint, 2.0f, 0, 0, false, false, true)
             == glm::vec3 (2.0f));
    REQUIRE (gain (tint, preset, tint, 2.0f, 0, 0, true, true, true)
             == glm::vec3 (0.5f, 1.0f, 2.0f));
}

TEST_CASE ("Compiled particle color endpoints receive a separate HSV tint",
           "[particle][instance][color]") {
    const glm::vec3 tint (0.25f, 0.5f, 1.0f);
    const glm::vec3 white (1.0f);
    const glm::vec3 shifted = particleInstanceShiftColorEndpoint (
        white, tint, white, true);
    REQUIRE (shifted.r == Catch::Approx (tint.r).margin (1e-6f));
    REQUIRE (shifted.g == Catch::Approx (tint.g).margin (1e-6f));
    REQUIRE (shifted.b == Catch::Approx (tint.b).margin (1e-6f));
    const glm::vec3 birthGain = particleInstanceBirthRgbGain (
        tint, white, white, 2.0f, 0, 0, true, false, true);
    const glm::vec3 nativeChild = birthGain * shifted;
    REQUIRE (nativeChild.r == Catch::Approx (0.125f).margin (1e-6f));
    REQUIRE (nativeChild.g == Catch::Approx (0.5f).margin (1e-6f));
    REQUIRE (nativeChild.b == Catch::Approx (2.0f).margin (1e-6f));
    REQUIRE (particleInstanceShiftColorEndpoint (white, tint, white, false) == white);
    REQUIRE (particleInstanceShiftColorEndpoint (
        glm::vec3 (1.0f, 0.75f, 0.0f), tint,
        glm::vec3 (1.0f, 0.75f, 0.0f), true).b
        == Catch::Approx (1.0f).margin (1e-6f));
}

TEST_CASE ("Particle preset color summary comes from color components, not authored top level",
           "[particle][instance][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"tinted","instanceoverride":{
        "colorn":"0.25 0.5 1","brightness":2},"particle":{
        "colorn":"0.25 0.5 1","hascolor":false,"flags":8,
        "initializer":[{"name":"colorrandom","min":"255 255 255",
                        "max":"255 255 255"}],
        "operator":[{"name":"colorchange","startvalue":"1 0.75 0",
                     "endvalue":"1 0 0"}]}})");
    auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->presetColorN == glm::vec3 (1.0f, 0.75f, 0.0f));
    REQUIRE (particle->presetHasColor);
    REQUIRE (particle->presetTintCompiled); // Missing scene version defaults to 0.
    REQUIRE (particle->flags == 8u);
    REQUIRE (particle->instanceOverride.colorn->value->getVec3 ()
             == glm::vec3 (0.25f, 0.5f, 1.0f));
    REQUIRE (particle->instanceOverride.brightness->value->getFloat () == 2.0f);
    // Native 14024d760 leaves an omitted instance tint at (-1,-1,-1),
    // allowing Torch's authored orange colorchange to run unchanged.
    data["instanceoverride"].erase ("colorn");
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    const glm::vec3 absentTint = particle->instanceOverride.colorn->value->getVec3 ();
    REQUIRE (absentTint == glm::vec3 (-1.0f));
    REQUIRE_FALSE (particleInstanceTintValid (absentTint, particle->presetColorN,
        particle->presetColorN, 0, particle->flags, true));
    REQUIRE (particleInstanceShiftColorEndpoint (glm::vec3 (1.0f, 0.75f, 0.0f),
        absentTint, particle->presetColorN, false) == glm::vec3 (1.0f, 0.75f, 0.0f));
    data["instanceoverride"]["colorn"] = "1 1 1";
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->instanceOverride.colorn->value->getVec3 () == glm::vec3 (1.0f));
    REQUIRE (particleInstanceTintValid (glm::vec3 (1.0f), particle->presetColorN,
        particle->presetColorN, 0, 0, true));
    // Native 14022af30 normalizes the legacy authored 0..255 `color` field
    // into `colorn`, overriding a simultaneous `colorn` entry.
    data["instanceoverride"]["color"] = "64 128 255";
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    const glm::vec3 aliasedTint = particle->instanceOverride.colorn->value->getVec3 ();
    REQUIRE (aliasedTint.r == Catch::Approx (64.0f / 255.0f));
    REQUIRE (aliasedTint.g == Catch::Approx (128.0f / 255.0f));
    REQUIRE (aliasedTint.b == Catch::Approx (1.0f));
    data["instanceoverride"].erase ("color");
    data["particle"].erase ("operator");
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->presetColorN == glm::vec3 (1.0f));
    REQUIRE (particle->presetHasColor);
    project.sceneVersion = 5;
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE_FALSE (particle->presetTintCompiled);
    data["particle"]["flags"] = 0x200000u;
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->presetTintCompiled);
    data["particle"]["flags"] = 0;
    data["particle"].erase ("initializer");
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->presetColorN == glm::vec3 (1.0f));
    REQUIRE_FALSE (particle->presetHasColor);
    REQUIRE (particle->presetTintCompiled);
}

TEST_CASE ("Root script tint changes affect later births without rewriting earlier RGB",
           "[particle][instance][color]") {
    using WallpaperEngine::Data::Model::DynamicValue;
    DynamicValue rootTint (glm::vec3 (0.25f, 0.5f, 1.0f));
    DynamicValue rootBrightness (2.0f);
    const glm::vec3 preset (0.0f);
    const auto birth = [&] {
        return particleInstanceBirthRgbGain (rootTint.getVec3 (), preset, preset,
            rootBrightness.getFloat (), 0, 0, true, false, true);
    };
    const glm::vec3 firstBirth = birth ();
    rootTint.update (glm::vec3 (0.5f, 0.25f, 0.75f), DynamicValue::Script);
    const glm::vec3 secondBirth = birth ();
    const glm::vec3 repeatedBirth = birth ();
    rootTint.update (glm::vec3 (0.25f, 0.5f, 1.0f), DynamicValue::Script);
    const glm::vec3 fourthBirth = birth ();
    REQUIRE (firstBirth == glm::vec3 (0.5f, 1.0f, 2.0f));
    REQUIRE (secondBirth == glm::vec3 (1.0f, 0.5f, 1.5f));
    REQUIRE (repeatedBirth == secondBirth);
    REQUIRE (fourthBirth == firstBirth);
    REQUIRE (firstBirth == glm::vec3 (0.5f, 1.0f, 2.0f));
}

TEST_CASE ("Alpha-random exponent shapes one native unit draw", "[particle][initializer]") {
    REQUIRE (alphaRandomExponentSample (0.5f, 0.1f, 0.2f, 2.0f)
             == Catch::Approx (0.125f));
    REQUIRE (alphaRandomExponentSample (0.5f, 0.1f, 0.2f, 1.0f)
             == Catch::Approx (0.15f));
    REQUIRE (alphaRandomExponentSample (0.0f, 0.1f, 0.2f, 2.0f)
             == Catch::Approx (0.1f));
    REQUIRE (alphaRandomExponentSample (1.0f, 0.1f, 0.2f, 2.0f)
             == Catch::Approx (0.2f));
    REQUIRE (alphaRandomExponentSample (0.5f, 0.1f, 0.2f, 2.0f, 0.5f)
             == Catch::Approx (0.0625f));
    REQUIRE (alphaRandomExponentSample (0.5f, 0.1f, 0.2f, 1.0f, 0.5f)
             == Catch::Approx (0.075f));
}

TEST_CASE ("Static-child turbulent birth uses root speed unless preset suppresses patch",
           "[particle][turbulent-birth]") {
    REQUIRE (turbulentBirthSpeedRange (0.0f, 50.0f, 0.9f, true)
             == glm::vec2 (0.0f, 45.0f));
    REQUIRE (turbulentBirthSpeedRange (40.0f, 90.0f, 1.0f, true)
             == glm::vec2 (40.0f, 90.0f));
    REQUIRE (turbulentBirthSpeedRange (0.0f, 50.0f, 0.9f, false)
             == glm::vec2 (0.0f, 50.0f));
}

TEST_CASE ("Flag-4 particle perspective matches the orthographic canvas at z zero",
           "[particle][projection]") {
    REQUIRE (flag4OrthographicEyeDistance (95.0f, 2.0f / 2160.0f)
             == Catch::Approx (989.637695f).margin (0.001f));
    for (const float fov : {95.0f, 67.0f}) {
      for (const float height : {720.0f, 2160.0f, -720.0f}) {
        const float width = std::abs (height) * 16.0f / 9.0f;
        const glm::mat4 ortho = glm::ortho (
            -width * 0.5f, width * 0.5f, -height * 0.5f, height * 0.5f,
            0.0f, 10000.0f);
        const float eyeZ = flag4OrthographicEyeDistance (fov, ortho[1][1]);
        REQUIRE (eyeZ == Catch::Approx (height / (2.0f * std::tan (glm::radians (fov) * 0.5f))));
        for (const float nearZ : {0.0f, 0.01f}) {
            const glm::mat4 viewProjection = glm::perspective (
                glm::radians (fov), width / height, nearZ, 10000.0f)
                * glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, -eyeZ));
            const glm::vec4 scenePoint (width * 0.2f, -height * 0.15f, 0.0f, 1.0f);
            const glm::vec4 projected = viewProjection * scenePoint;
            const glm::vec4 orthographic = ortho * scenePoint;
            REQUIRE (projected.x / projected.w == Catch::Approx (orthographic.x / orthographic.w).margin (1e-5));
            REQUIRE (projected.y / projected.w == Catch::Approx (orthographic.y / orthographic.w).margin (1e-5));
            const glm::vec4 raised = viewProjection * glm::vec4 (scenePoint.x, scenePoint.y, 500.0f, 1.0f);
            REQUIRE (raised.x / raised.w == Catch::Approx (
                projected.x / projected.w * eyeZ / (eyeZ - 500.0f)).margin (1e-5));
        }
      }
    }
}

namespace {
std::vector<std::string> fields (const std::string& line) {
    std::vector<std::string> result;
    std::stringstream input (line);
    std::string value;
    while (std::getline (input, value, '\t')) result.push_back (value);
    return result;
}

float number (const std::string& value) { return std::stof (value); }

std::ifstream fixture (const char* name) {
    std::ifstream file (std::string (PARTICLE_CORE_FIXTURE_DIR) + "/" + name);
    REQUIRE (file.is_open ());
    return file;
}
}

TEST_CASE ("Fixed particle renderer axes follow native factory basis", "[particle][renderer]") {
    const auto oblique = fixedRendererBasis ({3.0f, 4.0f, 0.0f});
    REQUIRE (oblique.right.x == Catch::Approx (0.6f));
    REQUIRE (oblique.right.y == Catch::Approx (0.8f));
    REQUIRE (oblique.up.x == Catch::Approx (-0.8f));
    REQUIRE (oblique.up.y == Catch::Approx (0.6f));
    const auto singular = fixedRendererBasis ({0.0f, 1.0f, 0.0f});
    REQUIRE (singular.right.y == 1.0f);
    REQUIRE (singular.up.z == -1.0f);
    const auto defaultAxis = fixedRendererBasis ({0.0f, 0.0f, 0.0f});
    REQUIRE (defaultAxis.right.z == 1.0f);
    REQUIRE (defaultAxis.up.y == 1.0f);
    // Nonuniform scale survives the native M^T M path; a 90-degree rotation
    // cancels only when renderer flag bit 0 is clear.
    glm::mat3 rotatedScale (1.0f);
    rotatedScale[0] = {0.0f, 2.0f, 0.0f};
    rotatedScale[1] = {-3.0f, 0.0f, 0.0f};
    const auto local = fixedRendererLocalBasis ({3.0f, 4.0f, 0.0f}, rotatedScale, false);
    REQUIRE (local);
    REQUIRE (local->right.x == Catch::Approx (0.31622777f));
    REQUIRE (local->right.y == Catch::Approx (-0.9486833f));
    const auto skipped = fixedRendererLocalBasis ({3.0f, 4.0f, 0.0f}, rotatedScale, true);
    REQUIRE (skipped);
    REQUIRE (skipped->right.x == Catch::Approx (-0.66436384f));
    REQUIRE (skipped->right.y == Catch::Approx (-0.74740932f));
}

TEST_CASE ("Upright renderer projects view-right against authored or fixed up", "[particle][renderer]") {
    const auto tilted = uprightRendererLocalBasis ({0.3f, 0.9f, 0.3f},
        glm::mat3 (1.0f), {-1.0f, 0.0f, 0.0f}, false);
    REQUIRE (tilted);
    REQUIRE (tilted->right.x == Catch::Approx (0.9534626f));
    REQUIRE (tilted->right.y == Catch::Approx (0.2860388f));
    REQUIRE (tilted->right.z == Catch::Approx (-0.09534626f));
    REQUIRE (tilted->up.y == Catch::Approx (-0.904534f));
    const auto worldUp = uprightRendererLocalBasis ({0.3f, 0.9f, 0.3f},
        glm::mat3 (1.0f), {-1.0f, 0.0f, 0.0f}, true);
    REQUIRE (worldUp);
    REQUIRE (worldUp->right.x == Catch::Approx (1.0f));
    REQUIRE (worldUp->up.y == Catch::Approx (-1.0f));
}

TEST_CASE ("Native particle movement pairs use distinct integration and damping clocks", "[particle][capture]") {
    auto file = fixture ("movement.tsv");
    std::string line;
    std::getline (file, line); // header
    size_t components = 0;
    size_t distinctClocks = 0;
    while (std::getline (file, line)) {
        auto row = fields (line);
        REQUIRE (row.size () == 9);
        const MovementTime time { number (row[1]), number (row[2]) };
        distinctClocks += time.integration != time.damping;
        REQUIRE (std::abs (dampingTime (time.integration, time.integration) - time.damping)
                 < 0.0000001f);
        float position = number (row[3]);
        float velocity = number (row[4]);
        integrateAxis (position, velocity, number (row[5]), number (row[6]), time);
        REQUIRE (position == number (row[7]));
        REQUIRE (velocity == number (row[8]));
        ++components;
    }
    REQUIRE (components == 2877);
    REQUIRE (distinctClocks > 0);
}

TEST_CASE ("Damping cap preserves a positive high-drag velocity", "[particle][boundary]") {
    float position = 0.0f;
    float velocity = 10.0f;
    integrateAxis (position, velocity, 0.0f, 1000.0f, { 0.1f, 0.1f });
    REQUIRE (position == 1.0f);
    REQUIRE (velocity > 0.0f);
    REQUIRE (velocity == 10.0f * (1.0f - 0x1.fffffcp-1f));
}

TEST_CASE ("Angular initializer records accumulate and movement owns only the tick clocks", "[particle][angular]") {
    // Native 2.8.42 initializer cases 0x0a/0x0c write += to distinct
    // angular-velocity/orientation streams. Fixed-range samples make duplicate
    // records deterministic without depending on the native RNG sequence.
    float rotation = 0.0f;
    float velocity = 0.0f;
    addAngularSample (rotation, 0.25f);
    addAngularSample (rotation, 0.5f);
    addAngularSample (velocity, 1.0f);
    addAngularSample (velocity, 2.0f);
    REQUIRE (rotation == 0.75f);
    REQUIRE (velocity == 3.0f);

    integrateAngularAxis (rotation, velocity, 4.0f, 2.0f, { 0.1f, 0.04f });
    REQUIRE (std::abs (rotation - 1.09f) < 0.000001f); // (3 + 4*.1)*.1 + .75
    REQUIRE (std::abs (velocity - 3.128f) < 0.000001f); // (3 + 4*.1)*(1-2*.04)

    // A second operator record acts on the first record's resulting state.
    integrateAngularAxis (rotation, velocity, -1.0f, 0.0f, { 0.1f, 0.04f });
    REQUIRE (std::abs (rotation - 1.3928f) < 0.000001f);
    REQUIRE (std::abs (velocity - 3.028f) < 0.000001f);
}

TEST_CASE ("Angular rotation is not wrapped and invalid dynamic values cannot hang the tick", "[particle][angular]") {
    float rotation = 8.0f;
    float velocity = 2.0f;
    integrateAngularAxis (rotation, velocity, 0.0f, 0.0f, { 0.5f, 0.5f });
    REQUIRE (rotation == 9.0f);
    REQUIRE (velocity == 2.0f);

    addAngularSample (rotation, std::numeric_limits<float>::infinity ());
    REQUIRE (rotation == 9.0f);
    integrateAngularAxis (rotation, velocity, 0.0f, 0.0f,
                          { std::numeric_limits<float>::infinity (), 0.5f });
    REQUIRE (rotation == 9.0f);
    REQUIRE (velocity == 2.0f);
}

TEST_CASE ("Native shared operator envelope multiplies independent lifetime ramps", "[particle][envelope]") {
    const BlendEnvelope envelope { 0.2f, 0.4f, 0.7f, 0.9f };
    REQUIRE (blendWeight (0.1f, envelope) == 0.0f);
    REQUIRE (std::abs (blendWeight (0.3f, envelope) - 0.5f) < 0.000001f);
    REQUIRE (blendWeight (0.5f, envelope) == 1.0f);
    REQUIRE (std::abs (blendWeight (0.8f, envelope) - 0.5f) < 0.000001f);
    REQUIRE (blendWeight (0.95f, envelope) == 0.0f);

    const BlendEnvelope overlapping { 0.0f, 0.8f, 0.2f, 1.0f };
    REQUIRE (usesBlendOpcode (overlapping));
    REQUIRE (std::abs (blendWeight (0.5f, overlapping) - 0.390625f) < 0.000001f);

    // Native builder separates coincident endpoints by 0.0001, keeping
    // zero-duration transitions finite at their authored boundary.
    const BlendEnvelope coincident { 0.2f, 0.2f, 0.7f, 0.7f };
    REQUIRE (std::abs (blendWeight (0.2f, coincident) - 1.0f) < 0.001f);
    REQUIRE (std::abs (blendWeight (0.7f, coincident) - 1.0f) < 0.001f);
    REQUIRE (blendWeight (0.3f, BlendEnvelope {}) == 1.0f);
    REQUIRE_FALSE (usesBlendOpcode (BlendEnvelope {}));
    REQUIRE_FALSE (usesBlendOpcode ({ 0.2f, 0.0f, 1.0f, 1.0f }));

    // The turbulence runtime applies each record's envelope to its own
    // velocity delta; duplicate records retain independent weights.
    float velocity = 1.0f;
    velocity += blendedVelocityDelta (8.0f, 0.5f, overlapping);
    velocity += blendedVelocityDelta (4.0f, 0.5f, envelope);
    REQUIRE (std::abs (velocity - 8.125f) < 0.000001f);
    REQUIRE (blendedVelocityDelta (4.0f, 0.5f, BlendEnvelope {}) == 4.0f);
    REQUIRE (std::abs (blendedMultiplier (2.0f, 0.5f, overlapping) - 1.390625f) < 0.000001f);
    REQUIRE (blendedMultiplier (2.0f, 0.0f, overlapping) == 1.0f);
    REQUIRE (blendedMultiplier (2.0f, 0.5f, BlendEnvelope {}) == 2.0f);
}

TEST_CASE ("Native alpha and size oscillators reuse one particle random value and compose", "[particle][oscillator]") {
    // Factory 1401c5490 packs three min/span pairs. Runtime 14023fbc0
    // cases 0x1e/0x1f read the same random stream and multiply current alpha/size.
    for (float random : { 0.0f, 0.5f, 1.0f }) {
        const float angle = (0.25f + 0.4f + random * 0.2f) * (2.0f + random * 2.0f);
        const float expected = 0.75f + (std::cos (angle) + 1.0f) * random * 0.5f / 2.0f;
        const float actual = oscillatorMultiplier (
            0.25f, random, 2.0f, 4.0f, 0.4f, 0.6f,
            0.75f, 1.25f, 0.25f, std::nullopt);
        REQUIRE (std::abs (actual - expected) < 0.000001f);
    }

    // A non-unit frequency distinguishes (age + phase) * frequency from
    // age * frequency + phase. Distinct records multiply within one pass;
    // the interpreter restores initial streams before its next invocation.
    float alpha = 0.8f;
    float size = 2.0f;
    const float first = oscillatorMultiplier (
        0.25f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f,
        0.75f, 1.25f, 0.5f, std::nullopt);
    const float second = oscillatorMultiplier (
        0.25f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f,
        0.5f, 1.0f, 0.5f, std::nullopt);
    alpha *= first;
    alpha *= second;
    REQUIRE (std::abs (alpha - 0.8f * first * second) < 0.000001f);
    size *= first;
    restoreOperatorStreams (alpha, size, 0.8f, 2.0f, true);
    REQUIRE (alpha == 0.8f);
    REQUIRE (size == 2.0f);
    alpha *= first;
    REQUIRE (std::abs (alpha - 0.8f * first) < 0.000001f);
    restoreOperatorStreams (alpha, size, 0.8f, 2.0f, false);
    REQUIRE (std::abs (alpha - 0.8f * first) < 0.000001f);
    REQUIRE (std::abs (first - (0.75f + (std::cos (0.25f * 3.0f + 0.5f) + 1.0f) * 0.5f * 0.5f / 2.0f))
             > 0.01f);

    const BlendEnvelope overlapping { 0.0f, 0.8f, 0.2f, 1.0f };
    const float enveloped = oscillatorMultiplier (
        0.25f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f,
        0.75f, 1.25f, 0.5f, overlapping);
    REQUIRE (std::abs (enveloped - (1.0f + (first - 1.0f) * 0.390625f)) < 0.000001f);
}

TEST_CASE ("Two-pass particle dispatch restores streams before each oscillator list", "[particle][oscillator][clock]") {
    float alpha = 0.0f;
    float size = 0.0f;
    int aged = 0;
    int resets = 0;
    int passes = 0;
    const auto clock = tickClock (0.1f, 0.1f, 20);
    REQUIRE (clock.operatorPasses == 2);
    dispatchTick (clock,
                  [&] (float ageDelta) {
                      REQUIRE (ageDelta == 0.1f);
                      ++aged;
                  },
                  [&] {
                      restoreOperatorStreams (alpha, size, 0.8f, 2.0f, true);
                      ++resets;
                  },
                  [&] (MovementTime time) {
                      REQUIRE (time.integration == 0.05f);
                      REQUIRE (alpha == 0.8f);
                      REQUIRE (size == 2.0f);
                      const float first = oscillatorMultiplier (
                          0.25f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f,
                          0.75f, 1.25f, 0.5f, std::nullopt);
                      alpha *= first;
                      alpha *= 1.25f; // second alpha operator sees first result
                      size *= first;
                      ++passes;
                  });
    REQUIRE (aged == 1);
    REQUIRE (resets == 2);
    REQUIRE (passes == 2);
    const float first = oscillatorMultiplier (
        0.25f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f,
        0.75f, 1.25f, 0.5f, std::nullopt);
    REQUIRE (std::abs (alpha - 0.8f * first * 1.25f) < 0.000001f);
    REQUIRE (std::abs (size - 2.0f * first) < 0.000001f);
}

TEST_CASE ("Position oscillator uses shared birth random and cosine step difference", "[particle][oscillator][position]") {
    for (float random : { 0.0f, 0.5f, 1.0f }) {
        const float frequency = 2.0f + random * 2.0f;
        const float phase = 0.4f + random * 0.2f + random * 6.2831855f;
        const float scale = 0.75f + random * 0.5f;
        const float angle = (0.25f + phase) * frequency;
        const float expected = (std::cos (angle) - std::cos (angle - 0.1f * frequency)) * scale;
        const float actual = positionOscillationDelta (
            0.25f, 0.1f, random, 2.0f, 4.0f, 0.4f, 0.6f, 0.75f, 1.25f, 1.0f);
        REQUIRE (std::abs (actual - expected) < 0.000001f);
    }
    REQUIRE (positionOscillationDelta (0.25f, 0.0f, 0.5f,
                                      2.0f, 4.0f, 0.4f, 0.6f, 0.75f, 1.25f, 1.0f) == 0.0f);
    for (float speed : { 0.5f, 2.0f }) {
        const float frequency = 3.0f * speed;
        const float phase = 0.5f + 0.5f * 6.2831855f;
        const float angle = (0.25f + phase) * frequency;
        const float expected = std::cos (angle) - std::cos (angle - 0.1f * frequency);
        const float actual = positionOscillationDelta (
            0.25f, 0.1f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f, 1.0f, 1.0f, speed);
        REQUIRE (std::abs (actual - expected) < 0.000001f);
    }
    const float base = positionOscillationDelta (
        0.25f, 0.1f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f, 1.0f, 1.0f, 1.0f);
    const BlendEnvelope overlapping { 0.0f, 0.8f, 0.2f, 1.0f };
    const float weighted = positionOscillationDelta (
        0.25f, 0.1f, 0.5f, 2.0f, 4.0f, 0.4f, 0.6f, 1.0f, 1.0f, 1.0f,
        blendWeight (0.5f, overlapping));
    REQUIRE (std::abs (weighted - base * 0.390625f) < 0.000001f);
}

TEST_CASE ("Alpha and size oscillator parser defaults match native factory", "[particle][oscillator][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::OscillateAlphaOperator;
    using WallpaperEngine::Data::Model::OscillateSizeOperator;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"osc","particle":{"operator":[
        {"name":"oscillatealpha"},{"name":"oscillatesize"}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 2);
    const auto* alpha = particle->operators[0]->as<OscillateAlphaOperator> ();
    const auto* size = particle->operators[1]->as<OscillateSizeOperator> ();
    REQUIRE (alpha != nullptr);
    REQUIRE (size != nullptr);
    REQUIRE (alpha->frequencyMin->value->getFloat () == 1.0f);
    REQUIRE (alpha->frequencyMax->value->getFloat () == 10.0f);
    REQUIRE (size->frequencyMin->value->getFloat () == 1.0f);
    REQUIRE (size->frequencyMax->value->getFloat () == 10.0f);
    REQUIRE (std::abs (size->scaleMin->value->getFloat () - 0.8f) < 0.000001f);
    REQUIRE (std::abs (size->scaleMax->value->getFloat () - 1.2f) < 0.000001f);
}

TEST_CASE ("Vortex variants remain distinct with current orthographic defaults", "[particle][vortex][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::VortexOperator;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"vortices","particle":{"operator":[
        {"name":"vortex"},{"name":"vortex_v2"}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 2);
    const auto* oldVortex = particle->operators[0]->as<VortexOperator> ();
    const auto* newVortex = particle->operators[1]->as<VortexOperator> ();
    REQUIRE (oldVortex != nullptr);
    REQUIRE (newVortex != nullptr);
    REQUIRE (oldVortex->variant == VortexOperator::Variant::Vortex);
    REQUIRE (newVortex->variant == VortexOperator::Variant::VortexV2);
    REQUIRE (oldVortex->sceneDefaults.distanceInner);
    REQUIRE (oldVortex->sceneDefaults.distanceOuter);
    REQUIRE (oldVortex->sceneDefaults.speedInner);
    REQUIRE (newVortex->sceneDefaults.distanceInner);
    REQUIRE (oldVortex->distanceInner->value->getFloat () == 500.0f);
    REQUIRE (oldVortex->distanceOuter->value->getFloat () == 650.0f);
    REQUIRE (oldVortex->speedInner->value->getFloat () == 2500.0f);
    REQUIRE (newVortex->distanceInner->value->getFloat () == 500.0f);
    REQUIRE (newVortex->distanceOuter->value->getFloat () == 650.0f);
    REQUIRE (newVortex->speedInner->value->getFloat () == 2500.0f);
    REQUIRE (oldVortex->axis->value->getVec3 () == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (newVortex->axis->value->getVec3 () == glm::vec3 (1.0f, 0.0f, 0.0f));
    const auto perspective = vortexDefaults (false);
    const auto automatic = vortexDefaults (true);
    const auto sized = vortexDefaults (true);
    const auto missingAxis = vortexDefaults (false);
    REQUIRE (perspective.distanceInner == 1.0f);
    REQUIRE (perspective.distanceOuter == 2.0f);
    REQUIRE (perspective.speedInner == 1.0f);
    REQUIRE (automatic.distanceInner == 500.0f);
    REQUIRE (sized.distanceOuter == 650.0f);
    REQUIRE (missingAxis.speedInner == 1.0f);

    auto explicitData = data;
    explicitData["particle"]["operator"][0]["distanceinner"] = 9.0f;
    explicitData["particle"]["operator"][0]["distanceouter"] = 10.0f;
    explicitData["particle"]["operator"][0]["speedinner"] = 11.0f;
    const auto explicitObject = ObjectParser::parse (explicitData, project);
    const auto* explicitParticle = dynamic_cast<const Particle*> (explicitObject.get ());
    REQUIRE (explicitParticle != nullptr);
    const auto* explicitVortex = explicitParticle->operators[0]->as<VortexOperator> ();
    REQUIRE (explicitVortex != nullptr);
    REQUIRE_FALSE (explicitVortex->sceneDefaults.distanceInner);
    REQUIRE_FALSE (explicitVortex->sceneDefaults.distanceOuter);
    REQUIRE_FALSE (explicitVortex->sceneDefaults.speedInner);
    REQUIRE (explicitVortex->distanceInner->value->getFloat () == 9.0f);

    auto blendedData = data;
    for (auto& vortex : blendedData["particle"]["operator"]) {
        vortex["blendinstart"] = 0.0f;
        vortex["blendinend"] = 0.8f;
        vortex["blendoutstart"] = 0.2f;
        vortex["blendoutend"] = 1.0f;
    }
    const auto blendedObject = ObjectParser::parse (blendedData, project);
    const auto* blendedParticle = dynamic_cast<const Particle*> (blendedObject.get ());
    REQUIRE (blendedParticle != nullptr);
    REQUIRE (blendedParticle->operators[0]->as<VortexOperator> ()->blendEnvelope.has_value ());
    REQUIRE (blendedParticle->operators[1]->as<VortexOperator> ()->blendEnvelope.has_value ());
    const BlendEnvelope nativeV2Envelope { 0.0f, 0.8f, 0.2f, 1.0f };
    REQUIRE (usesBlendOpcode (nativeV2Envelope));
    REQUIRE (std::abs (blendWeight (0.5f, nativeV2Envelope) - 0.390625f) < 0.000001f);
}

TEST_CASE ("Legacy vortex clamps distance before velocity scaling", "[particle][vortex]") {
    REQUIRE (vortexRadialSpeed (0.0f, 1.0f, 3.0f, 8.0f, 2.0f) == 8.0f);
    REQUIRE (vortexRadialSpeed (2.0f, 1.0f, 3.0f, 8.0f, 2.0f) == 5.0f);
    REQUIRE (vortexRadialSpeed (4.0f, 1.0f, 3.0f, 8.0f, 2.0f) == 2.0f);
    REQUIRE (vortexRadialSpeed (2.0f, 2.0f, 2.0f, 8.0f, 2.0f) == 8.0f);
    REQUIRE (vortexRadialSpeed (2.5f, 2.0f, 2.0f, 8.0f, 2.0f) == 5.0f);
    REQUIRE (vortexRadialSpeed (3.0f, 2.0f, 2.0f, 8.0f, 2.0f) == 2.0f);
    // The interpreter scales both packed inner speed and speed span by q,
    // which differs from the integration clock for low frame-rate passes.
    const MovementTime clocks { 0.25f, 0.10f };
    REQUIRE (std::abs (vortexRadialVelocityScale (1.0f, 1.0f, 3.0f, 8.0f, 2.0f, clocks)
                       - 0.8f) < 0.000001f);
    REQUIRE (std::abs (vortexRadialVelocityScale (2.0f, 1.0f, 3.0f, 8.0f, 2.0f, clocks)
                       - 0.5f) < 0.000001f);
    REQUIRE (std::abs (vortexRadialVelocityScale (3.0f, 1.0f, 3.0f, 8.0f, 2.0f, clocks)
                       - 0.2f) < 0.000001f);
}

TEST_CASE ("Native vortex direction converts through simulation Y reflection and unequal clocks", "[particle][vortex][clock]") {
    // Native 0x0f/0x10 computes radial cross axis. The simulation is reflected
    // through Y, so its equivalent is converted-axis cross converted-radial.
    const glm::vec3 nativeRadial { 0.0f, 2.0f, 0.0f };
    const glm::vec3 nativeAxis { 1.0f, 0.0f, 0.0f };
    const glm::vec3 tangent = vortexTangent (
        toSimulationVector (nativeRadial), toSimulationVector (nativeAxis));
    REQUIRE (tangent == glm::vec3 (0.0f, 0.0f, -1.0f));
    const glm::vec3 asymmetricAxis = glm::normalize (glm::vec3 (1.0f, 2.0f, 3.0f));
    const glm::vec3 asymmetricRadial { 4.0f, 5.0f, 6.0f };
    const auto nativeTangent = glm::normalize (glm::cross (asymmetricRadial, asymmetricAxis));
    const auto convertedTangent = vortexTangent (
        toSimulationVector (asymmetricRadial), toSimulationVector (asymmetricAxis));
    REQUIRE (glm::length (convertedTangent - toSimulationVector (nativeTangent)) < 0.000001f);
    // For a world CP, the Linux basis is F * nativeInverse * F. Feeding it
    // F * authoredAxis must equal F * nativeInverse * authoredAxis.
    const glm::mat3 reflection (
        glm::vec3 (1.0f, 0.0f, 0.0f),
        glm::vec3 (0.0f, -1.0f, 0.0f),
        glm::vec3 (0.0f, 0.0f, 1.0f));
    const glm::mat3 nativeInverse (
        glm::vec3 (0.0f, 1.0f, 0.0f),
        glm::vec3 (-1.0f, 0.0f, 0.0f),
        glm::vec3 (0.0f, 0.0f, 1.0f));
    const glm::mat3 simulationBasis = reflection * nativeInverse * reflection;
    REQUIRE (glm::length (vortexAxis (toSimulationVector (asymmetricAxis), simulationBasis, true)
                          - toSimulationVector (nativeInverse * asymmetricAxis)) < 0.000001f);
    REQUIRE (vortexTangent ({ 2.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f })
             == glm::vec3 (0.0f));

    glm::vec3 legacyVelocity (0.0f);
    glm::vec3 v2Velocity (0.0f);
    int passes = 0;
    const auto clock = tickClock (0.1f, 0.2f, 20);
    REQUIRE (clock.operatorTime.integration != clock.operatorTime.damping);
    dispatchTick (clock, [] (float) {}, [&] (MovementTime time) {
        legacyVelocity += tangent * vortexRadialVelocityScale (
            2.0f, 1.0f, 3.0f, 8.0f, 2.0f, time);
        v2Velocity += tangent * vortexRadialSpeed (
            2.0f, 1.0f, 3.0f, 8.0f, 2.0f) * time.damping;
        ++passes;
    });
    REQUIRE (passes == 2);
    const float expectedZ = -5.0f * dampingTime (0.1f, 0.2f);
    REQUIRE (std::abs (legacyVelocity.z - expectedZ) < 0.000001f);
    REQUIRE (std::abs (v2Velocity.z - expectedZ) < 0.000001f);
    REQUIRE (legacyVelocity.x == 0.0f);
    REQUIRE (v2Velocity.y == 0.0f);
}

TEST_CASE ("Vortex v2 non-ring center force corrects predicted radial drift", "[particle][vortex]") {
    const MovementTime time { 0.5f, 0.1f };
    const glm::vec3 current { 2.0f, 0.0f, 0.0f };
    const auto stationary = vortexV2RadialCorrection (current, current, 1.0f, time);
    REQUIRE (stationary == glm::vec3 (0.0f));
    const auto outward = vortexV2RadialCorrection (current, { 2.5f, 0.0f, 0.0f }, 1.0f, time);
    const auto inward = vortexV2RadialCorrection (current, { 1.5f, 0.0f, 0.0f }, 1.0f, time);
    REQUIRE (std::abs (outward.x + 1.0f) < 0.000001f);
    REQUIRE (std::abs (inward.x - 1.0f) < 0.000001f);
    REQUIRE (outward.y == 0.0f);
    REQUIRE (inward.z == 0.0f);
    REQUIRE (vortexV2RadialCorrection (current, { 2.5f, 0.0f, 0.0f }, 0.0f, time)
             == glm::vec3 (0.0f));
    // The native v2 write still applies this finite correction when the
    // tangent vanishes because the current radial vector is parallel to axis.
    const auto parallelTangent = vortexTangent (current, { 1.0f, 0.0f, 0.0f });
    REQUIRE (parallelTangent == glm::vec3 (0.0f));
    REQUIRE (glm::length (parallelTangent * 5.0f * time.damping + outward
                          - glm::vec3 (-1.0f, 0.0f, 0.0f)) < 0.000001f);
}

TEST_CASE ("Vortex variants differ in offset and axis transformation", "[particle][vortex]") {
    glm::mat3 basis (1.0f);
    basis[0] = glm::vec3 (0.0f, 1.0f, 0.0f);
    basis[1] = glm::vec3 (-1.0f, 0.0f, 0.0f);
    const glm::vec3 controlPoint { 3.0f, 4.0f, 0.0f };
    const glm::vec3 offset { 2.0f, 0.0f, 0.0f };
    REQUIRE (vortexCenter (controlPoint, offset, false)
             == glm::vec3 (5.0f, 4.0f, 0.0f));
    REQUIRE (vortexCenter (controlPoint, offset, true)
             == glm::vec3 (3.0f, 4.0f, 0.0f));
    REQUIRE (vortexAxis (glm::vec3 (1.0f, 0.0f, 0.0f), basis, false)
             == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (vortexAxis (glm::vec3 (1.0f, 0.0f, 0.0f), basis, true)
             == glm::vec3 (0.0f, 1.0f, 0.0f));
    basis[0] = glm::vec3 (0.0f, 2.0f, 0.0f);
    REQUIRE (vortexAxis (glm::vec3 (1.0f, 0.0f, 0.0f), basis, true)
             == glm::vec3 (0.0f, 2.0f, 0.0f));
    REQUIRE (normalizedVortexAxis ({ 0.0f, 0.02f, 0.0f })
             == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (normalizedVortexAxis ({ 0.0f, 0.04f, 0.0f })
             == glm::vec3 (0.0f, 1.0f, 0.0f));
    REQUIRE (vortexControlPointIndex (0) == 0);
    REQUIRE (vortexControlPointIndex (7) == 7);
    REQUIRE (vortexControlPointIndex (8) == 7);
    REQUIRE (vortexControlPointIndex (-1) == 7);
}

TEST_CASE ("Vortex v2 ring interpolation and pull use separate transition bands", "[particle][vortex]") {
    const auto core = vortexV2RingInfluence (10.0f, 10.0f, 2.0f, 4.0f);
    const auto edge = vortexV2RingInfluence (12.0f, 10.0f, 2.0f, 4.0f);
    const auto outside = vortexV2RingInfluence (13.0f, 10.0f, 2.0f, 4.0f);
    const auto inside = vortexV2RingInfluence (7.0f, 10.0f, 2.0f, 4.0f);
    const auto beyond = vortexV2RingInfluence (16.0f, 10.0f, 2.0f, 4.0f);
    REQUIRE (core.speedInterpolation == 0.0f);
    REQUIRE (core.signedPull == 0.0f);
    REQUIRE (edge.speedInterpolation == 0.0f);
    REQUIRE (edge.signedPull == 0.0f);
    REQUIRE (outside.speedInterpolation == 0.25f);
    REQUIRE (outside.signedPull == -0.75f);
    REQUIRE (inside.speedInterpolation == 0.25f);
    REQUIRE (inside.signedPull == 0.75f);
    REQUIRE (beyond.speedInterpolation == 1.0f);
    REQUIRE (beyond.signedPull == 0.0f);
    REQUIRE (13.0f * outside.signedPull * 2.0f * 0.5f == -9.75f);
    REQUIRE (vortexV2RingInfluence (12.5f, 10.0f, 2.0f, 0.0f).speedInterpolation == 0.5f);
}

TEST_CASE ("Cap velocity reduces one vector magnitude and blends only the excess", "[particle][envelope][capvelocity]") {
    float x = 3.0f, y = 4.0f, z = 0.0f;
    const float capped = capVelocityFactor (x, y, z, 2.0f, 0.5f, std::nullopt);
    REQUIRE (std::abs (capped - 0.4f) < 0.000001f);
    x *= capped; y *= capped; z *= capped;
    REQUIRE (std::abs (std::sqrt (x*x + y*y + z*z) - 2.0f) < 0.000001f);

    const BlendEnvelope overlapping { 0.0f, 0.8f, 0.2f, 1.0f };
    const float partial = capVelocityFactor (3.0f, 4.0f, 0.0f, 2.0f, 0.5f, overlapping);
    REQUIRE (std::abs (partial - (1.0f + (0.4f - 1.0f) * 0.390625f)) < 0.000001f);
    REQUIRE (capVelocityFactor (3.0f, 4.0f, 0.0f, 2.0f, 0.0f, overlapping) == 1.0f);
    REQUIRE (capVelocityFactor (0.0f, 0.0f, 0.0f, 2.0f, 0.5f, overlapping) == 1.0f);
    REQUIRE (capVelocityFactor (1.0f, 0.0f, 0.0f, 2.0f, 0.5f, overlapping) == 1.0f);

    // A second record acts on the first record's reduced vector.
    const float second = capVelocityFactor (x, y, z, 1.0f, 0.5f, std::nullopt);
    REQUIRE (std::abs (second - 0.5f) < 0.000001f);
}

TEST_CASE ("Cap velocity parser distinguishes authored maxspeed from scene default", "[particle][capvelocity][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::CapVelocityOperator;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"cap","particle":{"operator":[
        {"name":"capvelocity","blendinstart":0,"blendinend":0.8,"blendoutstart":0.2,"blendoutend":1},
        {"name":"capvelocity","maxspeed":2}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 2);
    const auto* defaulted = particle->operators[0]->as<CapVelocityOperator> ();
    const auto* authored = particle->operators[1]->as<CapVelocityOperator> ();
    REQUIRE (defaulted != nullptr);
    REQUIRE (authored != nullptr);
    REQUIRE (defaulted->useSceneDefault);
    REQUIRE (defaulted->blendEnvelope.has_value ());
    REQUIRE_FALSE (authored->useSceneDefault);
    REQUIRE (authored->maxSpeed->value->getFloat () == 2.0f);
    REQUIRE (capVelocityDefault (true) == 100.0f);
    REQUIRE (capVelocityDefault (false) == 1.0f);
}

TEST_CASE ("Turbulent velocity keeps authored speed bounds separate from scene defaults",
           "[particle][turbulence][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::TurbulentVelocityRandomInitializer;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"turbulence","particle":{"initializer":[
        {"name":"turbulentvelocityrandom"},
        {"name":"turbulentvelocityrandom","speedmin":7},
        {"name":"turbulentvelocityrandom","speedmax":11,
         "audioprocessingmode":2,"audioprocessingbounds":"0.2 0.8",
         "audioprocessingexponent":3,"audioprocessingfrequencystart":4,
         "audioprocessingfrequencyend":9}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->initializers.size () == 3);
    const auto* omitted = particle->initializers[0]->as<TurbulentVelocityRandomInitializer> ();
    const auto* lowAuthored = particle->initializers[1]->as<TurbulentVelocityRandomInitializer> ();
    const auto* highAuthored = particle->initializers[2]->as<TurbulentVelocityRandomInitializer> ();
    REQUIRE (omitted != nullptr);
    REQUIRE (lowAuthored != nullptr);
    REQUIRE (highAuthored != nullptr);
    REQUIRE (omitted->speedMinDefault);
    REQUIRE (omitted->speedMaxDefault);
    REQUIRE_FALSE (lowAuthored->speedMinDefault);
    REQUIRE (lowAuthored->speedMaxDefault);
    REQUIRE (lowAuthored->speedMin->value->getFloat () == 7.0f);
    REQUIRE (highAuthored->speedMinDefault);
    REQUIRE_FALSE (highAuthored->speedMaxDefault);
    REQUIRE (highAuthored->speedMax->value->getFloat () == 11.0f);
    REQUIRE (omitted->audioProcessingMode->value->getInt () == 0);
    REQUIRE (omitted->audioProcessingBounds->value->getVec2 () == glm::vec2 (0.8f, 1.0f));
    REQUIRE (highAuthored->audioProcessingMode->value->getInt () == 2);
    REQUIRE (highAuthored->audioProcessingBounds->value->getVec2 () == glm::vec2 (0.2f, 0.8f));
    REQUIRE (highAuthored->audioProcessingExponent->value->getFloat () == 3.0f);
    REQUIRE (highAuthored->audioProcessingFrequencyStart->value->getInt () == 4);
    REQUIRE (highAuthored->audioProcessingFrequencyEnd->value->getInt () == 9);
    REQUIRE (turbulentVelocitySpeedDefaults (true) == glm::vec2 (100.0f, 250.0f));
    REQUIRE (turbulentVelocitySpeedDefaults (false) == glm::vec2 (0.5f, 1.0f));
}

TEST_CASE ("Default remapvalue multiplies size by lifetime fraction per operator pass", "[particle][remap]") {
    float size = 99.0f;
    float alpha = 99.0f;
    int passes = 0;
    dispatchTick (tickClock (0.1f, 0.1f, 20), [] (float) {},
                  [&] { size = 8.0f; alpha = 0.8f; },
                  [&] (MovementTime) {
                      size = remapLifetimeMultiply (size, 0.25f, 1.0f);
                      size = remapLifetimeMultiply (size, 0.25f, 1.0f);
                      alpha = remapLifetimeMultiply (alpha, 0.25f, 1.0f);
                      alpha = remapLifetimeMultiply (alpha, 0.25f, 1.0f);
                      REQUIRE (size == 0.5f); // duplicate records compose
                      REQUIRE (std::abs (alpha - 0.05f) < 0.000001f);
                      ++passes;
                  });
    REQUIRE (passes == 2);
    REQUIRE (size == 0.5f); // no cumulative cross-pass decay
    REQUIRE (std::abs (alpha - 0.05f) < 0.000001f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.0f, 1.0f) == 0.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 1.0f, 1.0f) == 8.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, -0.5f, 1.0f) == 0.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 1.5f, 1.0f) == 8.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, -0.5f, 1.0f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, 0.0f, 1.0f, 0 }) == -4.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 1.5f, 1.0f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, 0.0f, 1.0f, 0 }) == 12.0f);

    const ScalarRemapRange authored { 0.25f, 0.75f, 2.0f, 4.0f, 1 };
    REQUIRE (remapLifetimeMultiply (8.0f, 0.25f, 1.0f, std::nullopt, authored) == 16.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.5f, 1.0f, std::nullopt, authored) == 24.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.75f, 1.0f, std::nullopt, authored) == 32.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 1.0f, 1.0f, std::nullopt, authored) == 32.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.5f, 1.0f, std::nullopt,
             ScalarRemapRange { 0.25f, 0.75f, 2.0f, 4.0f, 3 }) == 8.0f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.5f, 1.0f, std::nullopt,
             ScalarRemapRange { 0.25f, 0.25f, 2.0f, 4.0f, 1 }) == 32.0f);
    REQUIRE (remapScalarMultiply (8.0f, 2.0f, 0.75f, std::nullopt,
             ScalarRemapRange { 0.0f, 4.0f, 0.0f, 1.0f, 1 }) == 4.0f);
    REQUIRE (gatedAngularSpeed (2.0f, false, false) == 0.0f);
    REQUIRE (gatedAngularSpeed (2.0f, true, false) == 2.0f);
    REQUIRE (gatedAngularSpeed (2.0f, false, true) == 2.0f);
    REQUIRE (gatedAngularSpeed (2.0f, true, true) == 2.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::X) == 1.0f);
    REQUIRE (reduceRemapVector (0.25f, 0.6f, 0.9f, RemapVectorComponent::All) == 0.25f);
    const float vectorAllInput = reduceRemapVector (
        0.25f, 0.6f, 0.9f, RemapVectorComponent::All);
    REQUIRE (remapScalarValue (8.0f, vectorAllInput, 0.5f,
             RemapOperation::Multiply) == 2.0f);
    REQUIRE (remapScalarValue (0.8f, vectorAllInput, 0.5f,
             RemapOperation::Set) == 0.25f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Y) == 2.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Z) == 3.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Sum) == 6.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Average) == 2.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Max) == 3.0f);
    REQUIRE (reduceRemapVector (1.0f, 2.0f, 3.0f, RemapVectorComponent::Min) == 1.0f);
    const glm::vec3 authoredMotion { 0.2f, 0.9f, 0.6f };
    const auto simulationMotion = toSimulationVector (authoredMotion);
    REQUIRE (simulationMotion.y == -0.9f);
    REQUIRE (remapVectorInput (simulationMotion, RemapVectorComponent::Y, true) == 0.9f);
    REQUIRE (std::abs (remapVectorInput (simulationMotion,
             RemapVectorComponent::Sum, true) - 1.7f) < 0.000001f);
    REQUIRE (remapVectorInput (simulationMotion, RemapVectorComponent::Max, true) == 0.9f);
    REQUIRE (remapVectorInput (simulationMotion, RemapVectorComponent::All, true) == 0.2f);
    REQUIRE (remapVectorInput (simulationMotion, RemapVectorComponent::Y, false) == -0.9f);
    const glm::vec3 negativeY = toSimulationVector ({ 0.2f, -0.9f, 0.6f });
    REQUIRE (remapVectorInput (negativeY, RemapVectorComponent::Min, true) == -0.9f);
    REQUIRE (std::abs (remapScalarValue (8.0f,
             reduceRemapVector (0.2f, 0.6f, 1.0f, RemapVectorComponent::Average),
             0.25f, RemapOperation::Multiply) - 4.8f) < 0.000001f);
    REQUIRE (remapScalarMultiply (8.0f, 8.0f, 0.75f, std::nullopt,
             ScalarRemapRange { 0.0f, 16.0f, 0.0f, 1.0f, 1 }) == 4.0f);
    REQUIRE (remapScalarValue (8.0f, 0.25f, 0.25f, RemapOperation::Set) == 0.25f);
    REQUIRE (remapScalarValue (8.0f, 0.25f, 0.25f, RemapOperation::Multiply) == 2.0f);
    REQUIRE (remapScalarValue (8.0f, 0.25f, 0.25f, RemapOperation::Add) == 8.25f);
    REQUIRE (remapScalarValue (8.0f, 0.25f, 0.25f, RemapOperation::Subtract) == 7.75f);
    REQUIRE (remapScalarMultiply (8.0f, 0.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.75f, 0.25f, 2.0f, 4.0f, 1 }) == 24.0f);
    REQUIRE (remapScalarMultiply (8.0f, 0.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, 4.0f, 2.0f, 1 }) == 24.0f);
    REQUIRE (remapScalarMultiply (8.0f, 0.25f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.25f, 0.25f, 2.0f, 4.0f, 1 }) == 16.0f);
    REQUIRE (remapScalarMultiply (8.0f, 1.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, -1.0f, 2.0f, 0 }) == 28.0f);
    REQUIRE (remapScalarMultiply (8.0f, 1.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, -1.0f, 2.0f, 1 }) == 16.0f);
    REQUIRE (remapScalarMultiply (8.0f, 1.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, -1.0f, 2.0f, 2 }) == 8.0f);
    REQUIRE (remapScalarMultiply (8.0f, 1.5f, 0.5f, std::nullopt,
             ScalarRemapRange { 0.0f, 1.0f, -1.0f, 2.0f, 3 }) == 8.0f);

    BlendEnvelope envelope { 0.0f, 0.5f, 0.5f, 1.0f };
    const float halfWeight = remapLifetimeMultiply (8.0f, 0.25f, 1.0f, envelope);
    REQUIRE (std::abs (halfWeight - 5.0f) < 0.00001f);
    REQUIRE (remapLifetimeMultiply (8.0f, 0.0f, 1.0f, envelope) == 8.0f);
    BlendEnvelope beforeBirth { -1.0f, -0.5f, -0.4f, -0.2f };
    REQUIRE (std::abs (remapLifetimeMultiply (8.0f, -0.3f, 1.0f, beforeBirth) - 4.0f)
             < 0.00001f); // envelope clock uses raw age/lifetime, input clamps to zero
    REQUIRE (std::abs (remapScalarMultiply (8.0f, 0.125f, 0.125f, envelope,
             ScalarRemapRange { 0.0f, 0.5f, 0.0f, 2.0f, 1 }) - 7.0f) < 0.00001f);
    REQUIRE (std::abs (remapScalarValue (8.0f, 0.25f, 0.25f,
             RemapOperation::Set, envelope) - 4.125f) < 0.00001f);
    REQUIRE (std::abs (remapScalarValue (8.0f, 0.25f, 0.25f,
             RemapOperation::Multiply, envelope) - 5.0f) < 0.00001f);
    REQUIRE (std::abs (remapScalarValue (8.0f, 0.25f, 0.25f,
             RemapOperation::Add, envelope) - 8.125f) < 0.00001f);
    REQUIRE (std::abs (remapScalarValue (8.0f, 0.25f, 0.25f,
             RemapOperation::Subtract, envelope) - 7.875f) < 0.00001f);
}

TEST_CASE ("Ordered scalar remaps read the current size and opacity streams", "[particle][remap]") {
    float size = 99.0f;
    float alpha = 99.0f;
    int passes = 0;
    dispatchTick (tickClock (0.1f, 0.1f, 20), [] (float) {},
                  [&] { size = 8.0f; alpha = 0.5f; },
                  [&] (MovementTime) {
                      size = remapScalarMultiply (size, alpha, 0.5f);
                      alpha = remapScalarMultiply (alpha, size, 0.5f, std::nullopt,
                          ScalarRemapRange { 0.0f, 8.0f, 0.0f, 1.0f, 1 });
                      REQUIRE (size == 4.0f);
                      REQUIRE (alpha == 0.25f);
                      ++passes;
                  });
    REQUIRE (passes == 2);
    REQUIRE (size == 4.0f);
    REQUIRE (alpha == 0.25f);
}

TEST_CASE ("Remap parser accepts mapped scalar outputs only", "[particle][remap][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::ScalarRemapValueOperator;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"remap","particle":{"operator":[
        {"name":"remapvalue"},{"name":"remapvalue","output":"opacity"},
        {"name":"remapvalue","flags":0},
        {"name":"remapvalue","inputrangemin":0,"inputrangemax":1,
         "outputrangemin":0,"outputrangemax":1,
         "inputcontrolpoint0":0,"inputcontrolpoint1":1,
         "outputcontrolpoint0":0,"outputcontrolpoint1":1,
         "transforminputscale":2,"transformoctaves":3},
        {"name":"remapvalue","blendinend":0.5},
        {"name":"remapvalue","flags":2},
        {"name":"remapvalue","inputrangemin":0.2,"inputrangemax":0.8,
         "outputrangemin":2,"outputrangemax":4},
        {"name":"remapvalue","input":"maxlifetime"},
        {"name":"remapvalue","input":"size"},
        {"name":"remapvalue","input":"opacity"},
        {"name":"remapvalue","input":"speed"},
        {"name":"remapvalue","input":"rotation"},
        {"name":"remapvalue","input":"angularspeed"},
        {"name":"remapvalue","input":"color","inputcomponent":"sum"},
        {"name":"remapvalue","input":"position","inputcomponent":"y"},
        {"name":"remapvalue","input":"velocity","inputcomponent":"max"},
        {"name":"remapvalue","input":"velocity"},
        {"name":"remapvalue","operation":"remap"},
        {"name":"remapvalue","operation":"add"},
        {"name":"remapvalue","operation":"subtract"},
        {"name":"remapvalue","output":"speed","operation":"remap"},
        {"name":"remapvalue","inputrangemin":"userValue"},
        {"name":"remapvalue","flags":4},
        {"name":"remapvalue","operation":"divide"}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 21);
    const auto* size = particle->operators[0]->as<ScalarRemapValueOperator> ();
    const auto* opacity = particle->operators[1]->as<ScalarRemapValueOperator> ();
    REQUIRE (size != nullptr);
    REQUIRE (opacity != nullptr);
    REQUIRE (size->output == ScalarRemapValueOperator::Output::Size);
    REQUIRE (opacity->output == ScalarRemapValueOperator::Output::Opacity);
    REQUIRE (size->flags == 1);
    REQUIRE (particle->operators[2]->as<ScalarRemapValueOperator> ()->flags == 0);
    REQUIRE (particle->operators[3]->as<ScalarRemapValueOperator> () != nullptr);
    REQUIRE (particle->operators[4]->blendEnvelope.has_value ());
    REQUIRE (particle->operators[5]->as<ScalarRemapValueOperator> ()->flags == 2);
    const auto* ranged = particle->operators[6]->as<ScalarRemapValueOperator> ();
    REQUIRE (ranged != nullptr);
    REQUIRE (ranged->inputMin == 0.2f);
    REQUIRE (ranged->outputMax == 4.0f);
    REQUIRE (particle->operators[7]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::MaxLifetime);
    REQUIRE (particle->operators[8]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Size);
    REQUIRE (particle->operators[9]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Opacity);
    REQUIRE (particle->operators[10]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Speed);
    REQUIRE (particle->operators[11]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Rotation);
    REQUIRE (particle->operators[12]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::AngularSpeed);
    REQUIRE (particle->operators[13]->as<ScalarRemapValueOperator> ()->inputComponent
             == ScalarRemapValueOperator::InputComponent::Sum);
    REQUIRE (particle->operators[14]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Position);
    REQUIRE (particle->operators[15]->as<ScalarRemapValueOperator> ()->inputComponent
             == ScalarRemapValueOperator::InputComponent::Max);
    REQUIRE (particle->operators[16]->as<ScalarRemapValueOperator> ()->inputComponent
             == ScalarRemapValueOperator::InputComponent::All);
    REQUIRE (particle->operators[16]->as<ScalarRemapValueOperator> ()->input
             == ScalarRemapValueOperator::Input::Velocity);
    REQUIRE (particle->operators[17]->as<ScalarRemapValueOperator> ()->operation
             == ScalarRemapValueOperator::Operation::Set);
    REQUIRE (particle->operators[18]->as<ScalarRemapValueOperator> ()->operation
             == ScalarRemapValueOperator::Operation::Add);
    REQUIRE (particle->operators[19]->as<ScalarRemapValueOperator> ()->operation
             == ScalarRemapValueOperator::Operation::Subtract);
    REQUIRE (particle->operators[20]->as<ScalarRemapValueOperator> ()->output
             == ScalarRemapValueOperator::Output::Speed);
}

TEST_CASE ("Random sprite frame follows birth random through particle compaction", "[particle][frame]") {
    using WallpaperEngine::Render::Objects::ParticleInstance;
    REQUIRE (randomFrameLifetime (0.0f) == 0.0f);
    REQUIRE (randomFrameLifetime (0.3125f) == 0.3125f);
    REQUIRE (randomFrameLifetime (1.0f) == 0.0f); // shader frac boundary
    ParticleInstance particles[3];
    particles[0].oscillatorRandom = 0.0625f;
    particles[1].oscillatorRandom = 0.3125f;
    particles[2].oscillatorRandom = 0.8125f;
    particles[0].alive = false;
    particles[1].alive = true;
    particles[2].alive = true;
    // CParticle's order-preserving compaction assigns the whole instance.
    particles[0] = particles[1];
    particles[1] = particles[2];
    constexpr float frames = 4.0f;
    REQUIRE (std::floor (randomFrameLifetime (particles[0].oscillatorRandom) * frames) == 1.0f);
    REQUIRE (std::floor (randomFrameLifetime (particles[1].oscillatorRandom) * frames) == 3.0f);
    REQUIRE (particles[0].oscillatorRandom == 0.3125f);
    REQUIRE (particles[1].oscillatorRandom == 0.8125f);
    // The shader computes a blend fraction, but only samples it when the
    // material's SPRITESHEETBLEND combo is enabled.
    REQUIRE (randomFrameLifetime (particles[0].oscillatorRandom) * frames - 1.0f == 0.25f);
}

TEST_CASE ("Control-point pair remap selects both authored indices", "[particle][remap][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::ScalarRemapValueOperator;
    using WallpaperEngine::Data::Model::VectorRemapValueOperator;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"pair","particle":{"operator":[
        {"name":"remapvalue","input":"positionbetweentwocontrolpoints",
         "inputcontrolpoint0":2,"inputcontrolpoint1":5,"output":"size"},
        {"name":"remapvalue","input":"positionbetweentwocontrolpoints",
         "inputcontrolpoint0":-1,"inputcontrolpoint1":9,"output":"color"},
        {"name":"remapvalue","input":"positionbetweentwocontrolpoints",
         "output":"opacity"},
        {"name":"remapvalue","input":"positionbetweentwocontrolpoints",
         "inputcontrolpoint1":"bad","output":"size"}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 3);
    const auto* scalar = particle->operators[0]->as<ScalarRemapValueOperator> ();
    const auto* vector = particle->operators[1]->as<VectorRemapValueOperator> ();
    const auto* defaults = particle->operators[2]->as<ScalarRemapValueOperator> ();
    REQUIRE (scalar != nullptr);
    REQUIRE (vector != nullptr);
    REQUIRE (defaults != nullptr);
    REQUIRE (scalar->input == ScalarRemapValueOperator::Input::PositionBetweenTwoControlPoints);
    REQUIRE (scalar->inputControlPoint0 == 2);
    REQUIRE (scalar->inputControlPoint1 == 5);
    REQUIRE (vector->inputControlPoint0 == 7);
    REQUIRE (vector->inputControlPoint1 == 7);
    REQUIRE (defaults->inputControlPoint0 == 0);
    REQUIRE (defaults->inputControlPoint1 == 1);
}

TEST_CASE ("Vector remap ranges and component stores follow native XYZ branches", "[particle][remap][vector]") {
    const glm::vec3 velocity { 3.0f, 4.0f, 0.0f };
    REQUIRE (remapSpeedOutput (velocity, 10.0f) == glm::vec3 (6.0f, 8.0f, 0.0f));
    REQUIRE (remapSpeedOutput (velocity, 7.5f) == glm::vec3 (4.5f, 6.0f, 0.0f));
    REQUIRE (remapSpeedOutput (glm::vec3 (0.0f), 10.0f) == glm::vec3 (0.0f));
    REQUIRE (remapSpeedOutput (velocity, 0.0f) == glm::vec3 (0.0f));
    REQUIRE (remapSpeedOutput (glm::vec3 (2.0f, 3.0f, 6.0f), -14.0f)
             == glm::vec3 (-4.0f, -6.0f, -12.0f));
    const ScalarRemapRange speedRange { 0.0f, 1.0f, 0.0f, 10.0f, 1 };
    REQUIRE (remapSpeedOutput (velocity, remapScalarValue (5.0f, 0.75f, 0.5f,
             RemapOperation::Set, std::nullopt, speedRange))
             == glm::vec3 (4.5f, 6.0f, 0.0f));
    REQUIRE (remapSpeedOutput (velocity, remapScalarValue (5.0f, 0.75f, 0.5f,
             RemapOperation::Add, std::nullopt, speedRange))
             == glm::vec3 (7.5f, 10.0f, 0.0f));
    const BlendEnvelope speedEnvelope { 0.0f, 0.5f, 0.5f, 1.0f };
    const ScalarRemapRange speedEnvelopeRange { 0.0f, 1.0f, 0.0f, 14.0f, 1 };
    const float blendedSpeed = remapScalarValue (7.0f, 1.0f, 0.25f,
        RemapOperation::Set, speedEnvelope, speedEnvelopeRange);
    REQUIRE (blendedSpeed == 10.5f);
    REQUIRE (remapSpeedOutput (glm::vec3 (2.0f, 3.0f, 6.0f), blendedSpeed)
             == glm::vec3 (3.0f, 4.5f, 9.0f));
    const glm::vec3 input { 0.25f, 0.5f, 0.75f };
    const glm::vec3 current { 10.0f, 20.0f, 30.0f };
    const glm::vec3 zeros (0.0f), ones (1.0f);
    const glm::vec3 low { 1.0f, 2.0f, 3.0f }, high { 5.0f, 6.0f, 7.0f };
    REQUIRE (remapVectorValue (current, input, 0.5f, RemapOperation::Set,
             zeros, ones, low, high, 1, std::nullopt, 0) == glm::vec3 (2.0f, 4.0f, 6.0f));
    REQUIRE (remapVectorValue (current, input, 0.5f, RemapOperation::Multiply,
             zeros, ones, low, high, 1, std::nullopt, 0) == glm::vec3 (20.0f, 80.0f, 180.0f));
    REQUIRE (remapVectorValue (current, input, 0.5f, RemapOperation::Add,
             zeros, ones, low, high, 1, std::nullopt, 0) == glm::vec3 (12.0f, 24.0f, 36.0f));
    REQUIRE (remapVectorValue (current, input, 0.5f, RemapOperation::Subtract,
             zeros, ones, low, high, 1, std::nullopt, 0) == glm::vec3 (8.0f, 16.0f, 24.0f));
    REQUIRE (remapVectorValue (current, input, 0.5f, RemapOperation::Set,
             zeros, ones, low, high, 1, std::nullopt, 2) == glm::vec3 (10.0f, 4.0f, 30.0f));
    REQUIRE (remapVectorValue (current, { -1.0f, 2.0f, 0.75f }, 0.5f,
             RemapOperation::Set, zeros, ones, low, high, 1, std::nullopt, 0)
             == glm::vec3 (1.0f, 6.0f, 6.0f));
    const auto sine = remapVectorValue (current, { 0.25f, 0.5f, 0.75f }, 0.5f,
        RemapOperation::Set, zeros, ones, zeros, ones, 1, std::nullopt, 0,
        RemapTransform::Sine, 2.0f);
    REQUIRE (std::abs (sine.x - 0.5f) < 0.000001f);
    REQUIRE (std::abs (sine.y - 1.0f) < 0.000001f);
    REQUIRE (std::abs (sine.z - 0.5f) < 0.000001f);
    const auto sineScaleFour = remapVectorValue (current, { 0.25f, 0.5f, 0.75f }, 0.5f,
        RemapOperation::Set, zeros, ones, zeros, ones, 1, std::nullopt, 0,
        RemapTransform::Sine, 4.0f);
    REQUIRE (std::abs (sineScaleFour.x - 1.0f) < 0.000001f);
    REQUIRE (std::abs (sineScaleFour.y - 0.0f) < 0.000001f);
    const auto triangle = remapVectorValue (current, { 0.125f, 0.375f, 0.625f }, 0.5f,
        RemapOperation::Set, zeros, ones, zeros, ones, 1, std::nullopt, 0,
        RemapTransform::Triangle, 2.0f);
    REQUIRE (triangle == glm::vec3 (0.5f, 0.5f, 0.5f));
    const auto triangleNegativeScale = remapScalarValue (0.0f, 0.375f, 0.5f,
        RemapOperation::Set, std::nullopt,
        ScalarRemapRange { 0.0f, 1.0f, 0.0f, 1.0f, 1, RemapTransform::Triangle, -2.0f });
    REQUIRE (triangleNegativeScale == 0.5f);
    const auto waveform = [] (float input, RemapTransform transform, float scale,
                              int flags = 1) {
        return remapScalarValue (0.0f, input, 0.5f, RemapOperation::Set,
            std::nullopt, ScalarRemapRange { 0.0f, 1.0f, 0.0f, 1.0f,
                                             flags, transform, scale });
    };
    REQUIRE (waveform (0.0f, RemapTransform::Square, 2.0f) == 0.0f);
    REQUIRE (waveform (0.2f, RemapTransform::Square, 2.0f) == 0.0f);
    REQUIRE (waveform (0.4f, RemapTransform::Square, 2.0f) == 1.0f);
    REQUIRE (waveform (0.2f, RemapTransform::Square, -2.0f) == 1.0f);
    REQUIRE (waveform (0.4f, RemapTransform::Square, -2.0f) == 0.0f);
    REQUIRE (waveform (0.5f, RemapTransform::Square, 1.0f) == 0.0f);
    const int originalRounding = std::fegetround ();
    REQUIRE (originalRounding != -1);
    struct RoundingRestore {
        int mode;
        ~RoundingRestore () { std::fesetround (mode); }
    } restoreRounding { originalRounding };
    REQUIRE (std::fesetround (FE_UPWARD) == 0);
    const float squareUpward = waveform (0.4f, RemapTransform::Square, 1.0f);
    REQUIRE (std::fesetround (FE_DOWNWARD) == 0);
    const float squareDownward = waveform (0.4f, RemapTransform::Square, 1.0f);
    REQUIRE (std::fesetround (originalRounding) == 0);
    REQUIRE (squareUpward == 0.0f);
    REQUIRE (squareDownward == 0.0f);
    REQUIRE (waveform (0x1p-148f, RemapTransform::Square, 1.0f, 0) == 0.0f);
    REQUIRE (waveform (0x1p-147f, RemapTransform::Square, 1.0f, 0) == 0.0f);
    REQUIRE (waveform (0x1p-146f, RemapTransform::Square, 1.0f, 0) == 0.0f);
    REQUIRE (waveform (-0x1p-146f, RemapTransform::Square, 1.0f, 0) == 1.0f);
    REQUIRE (waveform (0.0f, RemapTransform::Saw, 2.0f) == 0.0f);
    REQUIRE (waveform (0x1p-148f, RemapTransform::Saw, 1.0f, 0) == 0x1p-148f);
    REQUIRE (waveform (0x1p-147f, RemapTransform::Saw, 1.0f, 0) == 0x1p-147f);
    REQUIRE (waveform (0x1p-146f, RemapTransform::Saw, 1.0f, 0) == 0x1p-146f);
    REQUIRE (waveform (-0x1p-146f, RemapTransform::Saw, 1.0f, 0) == 1.0f);
    REQUIRE (std::abs (waveform (0.6f, RemapTransform::Saw, 2.0f) - 0.2f) < 0.000001f);
    REQUIRE (std::abs (waveform (0.2f, RemapTransform::Saw, -2.0f) + 0.4f) < 0.000001f);
    REQUIRE (std::abs (waveform (-0.2f, RemapTransform::Saw, 2.0f, 0) - 0.6f) < 0.000001f);
    // Independent scalar translation of native fallback 1400fb820, with
    // x = normalized*scale, y = 0, and float birth-random bits as hash seed.
    REQUIRE (std::abs (remapSimplexNoise (0x3e800000u, 0.3f) - 0.6666548f) < 0.00001f);
    REQUIRE (std::abs (remapSimplexNoise (0x3e800000u, 0.8f) + 0.3103085f) < 0.00001f);
    REQUIRE (std::abs (remapSimplexNoise (0x3e800000u, -0.6f) + 0.2930338f) < 0.00001f);
    REQUIRE (std::abs (remapSimplexNoise (0x3e800000u, 0.7319508f) + 0.5722526f) < 0.00001f);
    REQUIRE (std::abs (remapSimplexNoise (0x3e800000u, 0.7321508f) + 0.5715732f) < 0.00001f);
    REQUIRE (std::abs (remapSimplexNoise (0x7fc00001u, 0.3f) - 0.2432410f) < 0.00001f);
    REQUIRE (remapSimplexNoise (0u, 0.0f) == 0.0f);
    const auto simplexVector = remapVectorValue (current, glm::vec3 (0.15f), 0.5f,
        RemapOperation::Set, zeros, ones, zeros, ones, 1, std::nullopt, 0,
        RemapTransform::SimplexNoise, 2.0f, 0x3e800000u, true);
    REQUIRE (std::abs (simplexVector.x - 0.8333274f) < 0.00001f);
    REQUIRE (std::abs (simplexVector.y - 0.3976590f) < 0.00001f);
    REQUIRE (std::abs (simplexVector.z - 0.3400206f) < 0.00001f);
    const auto simplexScalarInput = remapVectorValue (current, glm::vec3 (0.15f), 0.5f,
        RemapOperation::Set, zeros, ones, zeros, ones, 1, std::nullopt, 0,
        RemapTransform::SimplexNoise, 2.0f, 0x3e800000u, false);
    REQUIRE (std::abs (simplexScalarInput.x - simplexScalarInput.y) < 0.000001f);
    REQUIRE (std::abs (simplexScalarInput.y - simplexScalarInput.z) < 0.000001f);
    const glm::vec3 rainAuthored = remapVectorValue (glm::vec3 (0.0f),
        glm::vec3 (0.15f), 0.15f, RemapOperation::Set, zeros, ones,
        glm::vec3 (-100.0f, -50.0f, 0.0f), glm::vec3 (100.0f, -500.0f, 0.0f),
        1, std::nullopt, 0, RemapTransform::SimplexNoise, 10.0f,
        0x3e800000u, false);
    REQUIRE (std::abs (rainAuthored.x - 2.8657303f) < 0.0001f);
    REQUIRE (std::abs (rainAuthored.y + 281.4479f) < 0.0001f);
    REQUIRE (rainAuthored.z == 0.0f);
    REQUIRE (toSimulationVector (rainAuthored).y > 0.0f);
    REQUIRE (std::abs (remapFBMNoise (0x3e800000u, 0.3f, 0) - 0.6666548f) < 0.00001f);
    REQUIRE (std::abs (remapFBMNoise (0x3e800000u, 0.3f, 1) - 0.6666548f) < 0.00001f);
    REQUIRE (std::abs (remapFBMNoise (0x3e800000u, 0.3f, 2) - 0.1812461f) < 0.00001f);
    REQUIRE (std::abs (remapFBMNoise (0x3e800000u, 0.3f, 3) - 0.1943595f) < 0.00001f);
    REQUIRE (std::abs (remapFBMNoise (0x3e800000u, 0.3f, 5) - 0.1612843f) < 0.00001f);
    const ScalarRemapRange clampedSine { 0.0f, 1.0f, 0.0f, 1.0f,
        1, RemapTransform::Sine, 2.0f };
    const ScalarRemapRange extrapolatedSine { 0.0f, 1.0f, 0.0f, 1.0f,
        0, RemapTransform::Sine, 2.0f };
    REQUIRE (std::abs (remapScalarValue (8.0f, -0.25f, 0.25f,
             RemapOperation::Set, std::nullopt, clampedSine)) < 0.000001f);
    REQUIRE (std::abs (remapScalarValue (8.0f, -0.25f, 0.25f,
             RemapOperation::Set, std::nullopt, extrapolatedSine) - 0.5f) < 0.000001f);
    const BlendEnvelope ramp { 0.0f, 0.5f, 0.5f, 1.0f };
    REQUIRE (std::abs (remapScalarValue (8.0f, 0.25f, 0.25f,
             RemapOperation::Multiply, ramp, clampedSine) - 6.0f) < 0.000001f);
    const glm::vec3 nativeParticle { 7.0f, -1.0f, 2.0f };
    const glm::vec3 nativeControlPoint { -3.0f, 4.0f, 5.0f };
    REQUIRE (localControlPointPosition (nativeControlPoint)
             == glm::vec3 (-3.0f, -4.0f, 5.0f));
    REQUIRE (localControlPointPosition (nativeControlPoint)
             - toSimulationVector (nativeParticle)
             == toSimulationVector (nativeControlPoint - nativeParticle));
    const float nativeDistance = remapControlPointDistance (nativeParticle, nativeControlPoint);
    const float simulationDistance = remapControlPointDistance (
        toSimulationVector (nativeParticle), toSimulationVector (nativeControlPoint));
    REQUIRE (std::abs (nativeDistance - std::sqrt (134.0f)) < 0.000001f);
    REQUIRE (simulationDistance == nativeDistance);
    const auto simulationParticle = toSimulationVector (nativeParticle);
    const auto simulationControlPoint = localControlPointPosition (nativeControlPoint);
    const glm::vec3 nativeEmitterOrigin { 11.0f, -8.0f, 2.0f };
    REQUIRE (toSimulationVector (nativeEmitterOrigin) + simulationControlPoint
             == toSimulationVector (nativeEmitterOrigin + nativeControlPoint));
    REQUIRE (remapControlPointVector (simulationParticle, simulationControlPoint,
             RemapControlPointVector::Position) == nativeControlPoint);
    REQUIRE (remapControlPointVector (simulationParticle, simulationControlPoint,
             RemapControlPointVector::Delta) == glm::vec3 (-10.0f, 5.0f, 3.0f));
    const auto nativeDirection = remapControlPointVector (
        simulationParticle, simulationControlPoint, RemapControlPointVector::Direction);
    REQUIRE (glm::length (nativeDirection - glm::vec3 (-10.0f, 5.0f, 3.0f) /
             std::sqrt (134.0f)) < 0.000001f);
    REQUIRE (remapControlPointVector (simulationControlPoint, simulationControlPoint,
             RemapControlPointVector::Direction) == glm::vec3 (0.0f));
    const glm::vec3 pair0 { 1.0f, 2.0f, 3.0f };
    const glm::vec3 pair1 { 5.0f, 8.0f, 10.0f };
    const glm::vec3 between = pair0 + 0.25f * (pair1 - pair0) + glm::vec3 (3.0f, -2.0f, 0.0f);
    REQUIRE (std::abs (remapPositionBetweenControlPoints (
        toSimulationVector (between), localControlPointPosition (pair0),
        localControlPointPosition (pair1)) - 0.25f) < 0.000001f);
    REQUIRE (std::abs (remapPositionBetweenControlPoints (
        pair0 - 0.5f * (pair1 - pair0), pair0, pair1) + 0.5f) < 0.000001f);
    REQUIRE (std::abs (remapPositionBetweenControlPoints (
        pair0 + 1.5f * (pair1 - pair0), pair0, pair1) - 1.5f) < 0.000001f);
    REQUIRE (remapPositionBetweenControlPoints (between, pair0, pair0) == 0.0f);
    const glm::vec3 previewPoint = toSimulationVector ({ 205.0f, 40.0f, 0.0f });
    const glm::vec3 previewControlPoint = toSimulationVector ({ 30.0f, 40.0f, 0.0f });
    const float previewDistance = remapControlPointDistance (previewPoint, previewControlPoint);
    REQUIRE (previewDistance == 175.0f);
    const auto previewColor = remapVectorValue (
        glm::vec3 (1.0f), glm::vec3 (previewDistance), 0.5f,
        RemapOperation::Set, glm::vec3 (150.0f), glm::vec3 (200.0f),
        { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, 1, std::nullopt, 0);
    REQUIRE (previewColor == glm::vec3 (0.5f, 0.0f, 0.5f));

    glm::vec3 color { 99.0f };
    int resets = 0, passes = 0;
    dispatchTick (tickClock (0.1f, 0.1f, 20), [] (float) {},
        [&] { color = { 0.2f, 0.4f, 0.6f }; ++resets; },
        [&] (MovementTime) {
            color = remapVectorValue (color, glm::vec3 (0.5f), 0.5f,
                RemapOperation::Multiply, zeros, ones, zeros, ones,
                1, std::nullopt, 0);
            color = remapVectorValue (color, glm::vec3 (0.5f), 0.5f,
                RemapOperation::Add, zeros, ones, zeros, ones,
                1, std::nullopt, 2);
            REQUIRE (color == glm::vec3 (0.1f, 0.7f, 0.3f));
            ++passes;
        });
    REQUIRE (resets == 2);
    REQUIRE (passes == 2);
}

TEST_CASE ("Vector remap parser retains authored ranges and output components", "[particle][remap][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::VectorRemapValueOperator;
    using WallpaperEngine::Data::Model::ScalarRemapValueOperator;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"vectorremap","particle":{"operator":[
        {"name":"remapvalue","output":"color","outputrangemin":"1 0 0",
         "outputrangemax":"0 0 1","operation":"remap",
         "transformfunction":"sine","transforminputscale":4},
        {"name":"remapvalue","output":"velocity","outputcomponent":"y",
         "input":"position","inputcomponent":"all","outputrangemin":[-1,-2,-3],
         "outputrangemax":[1,2,3],"operation":"add"},
        {"name":"remapvalue","output":"position","outputrangemin":2},
        {"name":"remapvalue","input":"distancetocontrolpoint",
         "inputcontrolpoint0":1,"inputrangemin":150,"inputrangemax":200,
         "output":"color","outputrangemin":"1 0 0","outputrangemax":"0 0 1",
         "operation":"remap"},
        {"name":"remapvalue","input":"distancetocontrolpoint",
         "inputcontrolpoint0":8,"output":"color"},
        {"name":"remapvalue","input":"distancetocontrolpoint",
         "inputcontrolpoint0":-1,"output":"color"},
        {"name":"remapvalue","output":"color","transformfunction":"triangle",
         "transforminputscale":-2},
        {"name":"remapvalue","output":"color","transformfunction":"square"},
        {"name":"remapvalue","output":"color","transformfunction":"saw"},
        {"name":"remapvalue","input":"controlpoint","inputcomponent":"y",
         "inputcontrolpoint0":2,"output":"size"},
        {"name":"remapvalue","input":"deltatocontrolpoint","inputcomponent":"sum",
         "inputcontrolpoint0":2,"output":"color"},
        {"name":"remapvalue","input":"directiontocontrolpoint","inputcomponent":"all",
         "inputcontrolpoint0":2,"output":"color"},
        {"name":"remapvalue","output":"velocity","operation":"remap",
         "outputrangemin":"-100 -50 0","outputrangemax":"100 -500 0",
         "transformfunction":"simplexnoise","transforminputscale":10},
        {"name":"remapvalue","output":"opacity","transformfunction":"fbmnoise",
         "transformoctaves":5},
        {"name":"remapvalue","output":"opacity","transformfunction":"fbmnoise",
         "transformoctaves":33}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 14);
    const auto* color = particle->operators[0]->as<VectorRemapValueOperator> ();
    const auto* velocity = particle->operators[1]->as<VectorRemapValueOperator> ();
    const auto* position = particle->operators[2]->as<VectorRemapValueOperator> ();
    const auto* distance = particle->operators[3]->as<VectorRemapValueOperator> ();
    REQUIRE (color != nullptr);
    REQUIRE (velocity != nullptr);
    REQUIRE (position != nullptr);
    REQUIRE (distance != nullptr);
    REQUIRE (particle->operators[6]->as<VectorRemapValueOperator> ()->transform
             == VectorRemapValueOperator::Transform::Triangle);
    REQUIRE (particle->operators[7]->as<VectorRemapValueOperator> ()->transform
             == VectorRemapValueOperator::Transform::Square);
    REQUIRE (particle->operators[8]->as<VectorRemapValueOperator> ()->transform
             == VectorRemapValueOperator::Transform::Saw);
    const auto* cpPosition = particle->operators[9]->as<ScalarRemapValueOperator> ();
    const auto* cpDelta = particle->operators[10]->as<VectorRemapValueOperator> ();
    const auto* cpDirection = particle->operators[11]->as<VectorRemapValueOperator> ();
    REQUIRE (cpPosition != nullptr);
    REQUIRE (cpDelta != nullptr);
    REQUIRE (cpDirection != nullptr);
    REQUIRE (cpPosition->input == ScalarRemapValueOperator::Input::ControlPoint);
    REQUIRE (cpPosition->inputControlPoint0 == 2);
    REQUIRE (cpDelta->input == VectorRemapValueOperator::Input::DeltaToControlPoint);
    REQUIRE (cpDirection->input == VectorRemapValueOperator::Input::DirectionToControlPoint);
    const auto* rainNoise = particle->operators[12]->as<VectorRemapValueOperator> ();
    REQUIRE (rainNoise != nullptr);
    REQUIRE (rainNoise->transform == VectorRemapValueOperator::Transform::SimplexNoise);
    REQUIRE (rainNoise->output == VectorRemapValueOperator::Output::Velocity);
    REQUIRE (rainNoise->outputMin == glm::vec3 (-100.0f, -50.0f, 0.0f));
    REQUIRE (rainNoise->outputMax == glm::vec3 (100.0f, -500.0f, 0.0f));
    REQUIRE (rainNoise->transformScale == 10.0f);
    REQUIRE (rainNoise->input == VectorRemapValueOperator::Input::LifetimeFraction);
    const auto* fbm = particle->operators[13]->as<ScalarRemapValueOperator> ();
    REQUIRE (fbm != nullptr);
    REQUIRE (fbm->transform == ScalarRemapValueOperator::Transform::FBMNoise);
    REQUIRE (fbm->transformOctaves == 5);
    REQUIRE (color->output == VectorRemapValueOperator::Output::Color);
    REQUIRE (color->outputMin == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (color->outputMax == glm::vec3 (0.0f, 0.0f, 1.0f));
    REQUIRE (color->transform == VectorRemapValueOperator::Transform::Sine);
    REQUIRE (color->transformScale == 4.0f);
    REQUIRE (velocity->output == VectorRemapValueOperator::Output::Velocity);
    REQUIRE (velocity->outputComponent == VectorRemapValueOperator::OutputComponent::Y);
    REQUIRE (velocity->inputComponent == VectorRemapValueOperator::InputComponent::All);
    REQUIRE (velocity->outputMin == glm::vec3 (-1.0f, -2.0f, -3.0f));
    REQUIRE (position->outputMin == glm::vec3 (2.0f));
    REQUIRE (distance->input == VectorRemapValueOperator::Input::DistanceToControlPoint);
    REQUIRE (distance->inputControlPoint0 == 1);
    REQUIRE (distance->inputMin == glm::vec3 (150.0f));
    REQUIRE (distance->inputMax == glm::vec3 (200.0f));
    REQUIRE (particle->operators[4]->as<VectorRemapValueOperator> ()->inputControlPoint0 == 7);
    REQUIRE (particle->operators[5]->as<VectorRemapValueOperator> ()->inputControlPoint0 == 7);
}

TEST_CASE ("Production birth remap writes ordered CP components and preserves the basis",
           "[particle][birth][remap][controlpoint]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":2,
         "operation":"remap","outputrangemin":"40 50 60","outputrangemax":"40 50 60"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":2,
         "operation":"add","outputcomponent":"x",
         "outputrangemin":"2 99 99","outputrangemax":"2 99 99"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":2,
         "operation":"subtract","outputcomponent":"y",
         "outputrangemin":"99 3 99","outputrangemax":"99 3 99"},
        {"name":"remapinitialvalue","output":"controlpoint","outputcontrolpoint0":2,
         "operation":"multiply","outputcomponent":"z",
         "outputrangemin":"99 99 0.5","outputrangemax":"99 99 0.5"}
    ],"operator":[
        {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,
         "output":"position","operation":"remap","flags":0}
    ]}})"), project);
    const auto& model = *object->as<Particle> ();
    std::vector<ControlPointData> cps (8);
    cps[2].position = toSimulationVector ({ 10.0f, 20.0f, 30.0f });
    cps[2].basis = glm::mat3 (glm::rotate (glm::mat4 (1.0f), 0.5f, glm::vec3 (0.0f, 0.0f, 1.0f)));
    const auto initialBasis = cps[2].basis;
    std::vector<ParticleInstance> particles (1);
    particles[0].alive = true;
    particles[0].lifetime = 1.0f;
    std::vector<OperatorFunc> writers;
    for (const auto& initializer : model.initializers) {
        const auto& remap = *initializer->as<RemapInitialValueInitializer> ()
            ->remap->as<VectorRemapValueOperator> ();
        writers.push_back (createVectorRemapOperator (remap, true, false, false, false));
        REQUIRE (writers.back ());
    }
    writers.push_back (createVectorRemapOperator (
        *model.operators[0]->as<VectorRemapValueOperator> (), false, false, false, false));
    REQUIRE (writers.back ());
    const std::array<glm::vec3, 4> ordered {
        glm::vec3 (40.0f, 50.0f, 60.0f), glm::vec3 (42.0f, 50.0f, 60.0f),
        glm::vec3 (42.0f, 47.0f, 60.0f), glm::vec3 (42.0f, 47.0f, 30.0f) };
    for (size_t index = 0; index < ordered.size (); ++index) {
        writers[index] (particles, 1, cps, 0.0f, MovementTime { 0.0f, 0.0f });
        REQUIRE (toAuthoredVector (cps[2].position) == ordered[index]);
        REQUIRE (cps[2].basis == initialBasis);
        REQUIRE (cps[1].position == glm::vec3 (0.0f));
    }
    writers[4] (particles, 1, cps, 0.0f, MovementTime { 0.0f, 0.0f });
    REQUIRE (toAuthoredVector (particles[0].position) == ordered[3]);
    // The retained translation is the next birth's current value. It is not
    // an initial-particle stream and must accumulate across separate births.
    for (int birth = 1; birth <= 4; ++birth) {
        particles[0].position = glm::vec3 (0.0f);
        writers[1] (particles, 1, cps, 0.0f, MovementTime { 0.0f, 0.0f });
        writers[4] (particles, 1, cps, 0.0f, MovementTime { 0.0f, 0.0f });
        REQUIRE (toAuthoredVector (particles[0].position)
            == glm::vec3 (42.0f + birth * 2.0f, 47.0f, 30.0f));
        REQUIRE (cps[2].basis == initialBasis);
    }
    const auto& cpWriter = *model.initializers[0]->as<RemapInitialValueInitializer> ()
        ->remap->as<VectorRemapValueOperator> ();
    REQUIRE_FALSE (createVectorRemapOperator (cpWriter, false, false, false, false));
    particles[0].position = glm::vec3 (0.0f);
    writers[4] (particles, 1, cps, 0.0f, MovementTime { 0.0f, 0.0f });
    REQUIRE (toAuthoredVector (particles[0].position) == glm::vec3 (50.0f, 47.0f, 30.0f));

    for (int authoredTarget = -1; authoredTarget <= 8; ++authoredTarget) {
        JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
            {"name":"remapinitialvalue","output":"controlpoint","operation":"remap",
             "outputrangemin":"3 5 7","outputrangemax":"3 5 7"}
        ]}})");
        data["particle"]["initializer"][0]["outputcontrolpoint0"] = authoredTarget;
        const auto targetObject = ObjectParser::parse (data, project);
        const auto& targetRemap = *targetObject->as<Particle> ()->initializers[0]
            ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> ();
        auto writeTarget = createVectorRemapOperator (targetRemap, true, false, false, false);
        std::vector<ControlPointData> targetCps (8);
        writeTarget (particles, 1, targetCps, 0.0f, MovementTime { 0.0f, 0.0f });
        const int target = authoredTarget < 0 || authoredTarget > 7 ? 7 : authoredTarget;
        for (int index = 0; index < 8; ++index) {
            REQUIRE (toAuthoredVector (targetCps[index].position)
                == (index == target ? glm::vec3 (3.0f, 5.0f, 7.0f) : glm::vec3 (0.0f)));
        }
    }
}

TEST_CASE ("Production birth CP inputs clear translation before scalar and vector reductions",
           "[particle][birth][remap][controlpoint]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const std::array<const char*, 3> inputs {
        "controlpoint", "deltatocontrolpoint", "directiontocontrolpoint" };
    const std::array<const char*, 8> components { "all", "x", "y", "z", "sum", "average", "max", "min" };
    // Independent coefficients for authored P=(30,40,0), not the old CP=(60,80,7).
    const std::array<glm::vec3, 3> sources {
        glm::vec3 (0.0f), glm::vec3 (-30.0f, -40.0f, 0.0f), glm::vec3 (-0.6f, -0.8f, 0.0f) };
    const std::array<std::array<float, 8>, 3> reduced {{
        { 0, 0, 0, 0, 0, 0, 0, 0 },
        { -30, -30, -40, 0, -70, -70.0f / 3.0f, 0, -40 },
        { -0.6f, -0.6f, -0.8f, 0, -1.4f, -1.4f / 3.0f, 0, -0.8f } }};
    for (size_t input = 0; input < inputs.size (); ++input) {
        for (size_t component = 0; component < components.size (); ++component) {
            JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[]}})");
            data["particle"]["initializer"] = JSON::array ({
                JSON {{"name", "remapinitialvalue"}, {"input", inputs[input]},
                    {"inputcomponent", components[component]}, {"inputcontrolpoint0", 2},
                    {"output", "size"}, {"operation", "remap"}, {"flags", 0},
                    {"outputrangemin", 4}, {"outputrangemax", 6}},
                JSON {{"name", "remapinitialvalue"}, {"input", inputs[input]},
                    {"inputcomponent", components[component]}, {"inputcontrolpoint0", 2},
                    {"output", "position"}, {"operation", "remap"}, {"flags", 0}} });
            const auto object = ObjectParser::parse (data, project);
            const auto& model = *object->as<Particle> ();
            REQUIRE (model.initializers.size () == 2);
            REQUIRE (model.controlPoints.empty ()); // inputs never claim generated ownership
            const auto scalar = createScalarRemapOperator (*model.initializers[0]
                ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (),
                true, false, false, false);
            const auto vector = createVectorRemapOperator (*model.initializers[1]
                ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> (),
                true, false, false, false);
            std::vector<ParticleInstance> particles (1);
            particles[0].alive = true;
            const glm::vec3 initialPosition = toSimulationVector ({ 30.0f, 40.0f, 0.0f });
            std::vector<ControlPointData> cps (8);
            cps[2].basis = glm::mat3 (glm::rotate (glm::mat4 (1.0f), 0.75f, glm::vec3 (0, 0, 1)));
            cps[2].offset = glm::vec3 (11, 13, 17);
            cps[2].previousPosition = glm::vec3 (19, 23, 29);
            const auto original = cps[2];
            for (const auto& closure : { scalar, vector }) {
                cps[2].position = toSimulationVector ({ 60.0f, 80.0f, 7.0f });
                particles[0].position = initialPosition;
                closure (particles, 1, cps, 0.0f, MovementTime {});
                REQUIRE (cps[2].position == glm::vec3 (0.0f));
                REQUIRE (cps[2].basis == original.basis);
                REQUIRE (cps[2].offset == original.offset);
                REQUIRE (cps[2].previousPosition == original.previousPosition);
                REQUIRE (cps[2].flags == 0);
                REQUIRE (cps[1].position == glm::vec3 (0.0f));
            }
            REQUIRE (particles[0].size == Catch::Approx (4.0f + 2.0f * reduced[input][component]));
            const glm::vec3 expected = component == 0 ? sources[input] : glm::vec3 (reduced[input][component]);
            const auto actual = toAuthoredVector (particles[0].position);
            for (int axis = 0; axis < 3; ++axis) REQUIRE (actual[axis] == Catch::Approx (expected[axis]));
        }
    }
}

TEST_CASE ("Birth CP input mutation precedes destination reads and disabled angular outputs",
           "[particle][birth][remap][controlpoint]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto object = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue","input":"controlpoint","inputcontrolpoint0":2,
         "output":"controlpoint","outputcontrolpoint0":2,"operation":"add",
         "outputrangemin":"10 20 30","outputrangemax":"10 20 30","flags":0},
        {"name":"remapinitialvalue","input":"controlpoint","inputcontrolpoint0":2,
         "output":"angularspeed","operation":"remap","outputrangemin":99,"outputrangemax":99}
    ],"operator":[
        {"name":"remapvalue","input":"controlpoint","inputcontrolpoint0":2,
         "output":"position","operation":"remap","flags":0}
    ]}})"), project);
    const auto& model = *object->as<Particle> ();
    REQUIRE (model.initializers.size () == 2);
    const auto writer = createVectorRemapOperator (*model.initializers[0]
        ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> (), true, false, false, false);
    const auto disabledAngular = createScalarRemapOperator (*model.initializers[1]
        ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
    const auto reader = createVectorRemapOperator (*model.operators[0]
        ->as<VectorRemapValueOperator> (), false, false, false, false);
    std::vector<ParticleInstance> particles (1);
    particles[0].alive = true;
    particles[0].angularVelocity.z = 7.0f;
    std::vector<ControlPointData> cps (8);
    cps[2].position = toSimulationVector ({ 60.0f, 80.0f, 90.0f });
    cps[2].basis = glm::mat3 (2.0f);
    cps[2].flags = 0x10000u;
    for (int birth = 0; birth < 4; ++birth) {
        writer (particles, 1, cps, 0.0f, MovementTime {});
        reader (particles, 1, cps, 0.0f, MovementTime {});
        REQUIRE (toAuthoredVector (cps[2].position) == glm::vec3 (10, 20, 30));
        REQUIRE (toAuthoredVector (particles[0].position) == glm::vec3 (10, 20, 30));
        REQUIRE (cps[2].basis == glm::mat3 (2.0f));
    }
    disabledAngular (particles, 1, cps, 0.0f, MovementTime {});
    reader (particles, 1, cps, 0.0f, MovementTime {});
    REQUIRE (particles[0].angularVelocity.z == 7.0f);
    REQUIRE (particles[0].position == glm::vec3 (0.0f));
    REQUIRE (cps[2].position == glm::vec3 (0.0f));
    REQUIRE (cps[2].basis == glm::mat3 (2.0f));
    REQUIRE (cps[2].flags == 0x10000u);
}

TEST_CASE ("Production birth CP direction retains IEEE outcomes after mutation",
           "[particle][birth][remap][controlpoint]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const float infinity = std::numeric_limits<float>::infinity ();
    const float nan = std::numeric_limits<float>::quiet_NaN ();
    const auto speedObject = ObjectParser::parse (JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue","input":"directiontocontrolpoint","inputcontrolpoint0":2,
         "output":"speed","operation":"remap","flags":0}
    ]}})"), project);
    const auto speed = createScalarRemapOperator (*speedObject->as<Particle> ()->initializers[0]
        ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
    for (const auto velocity : { glm::vec3 (3, 4, 0), glm::vec3 (0.0f) }) {
        std::vector<ParticleInstance> particles (1);
        particles[0].alive = true;
        particles[0].velocity = velocity;
        std::vector<ControlPointData> cps (8);
        cps[2].position = glm::vec3 (5, 7, 11);
        speed (particles, 1, cps, 0.0f, MovementTime {});
        REQUIRE (cps[2].position == glm::vec3 (0.0f));
        for (int axis = 0; axis < 3; ++axis) REQUIRE (std::isnan (particles[0].velocity[axis]));
    }
    for (const auto component : { "max", "min" }) {
        JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
            {"name":"remapinitialvalue","input":"deltatocontrolpoint","inputcontrolpoint0":2,
             "output":"size","operation":"remap","flags":0},
            {"name":"remapinitialvalue","input":"deltatocontrolpoint","inputcontrolpoint0":2,
             "output":"position","operation":"remap","flags":0}
        ]}})");
        for (auto& initializer : data["particle"]["initializer"]) initializer["inputcomponent"] = component;
        const auto object = ObjectParser::parse (data, project);
        const auto& model = *object->as<Particle> ();
        const auto scalar = createScalarRemapOperator (*model.initializers[0]
            ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
        const auto vector = createVectorRemapOperator (*model.initializers[1]
            ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> (), true, false, false, false);
        std::vector<ParticleInstance> particles (1);
        particles[0].alive = true;
        std::vector<ControlPointData> cps (8);
        const float z = std::string (component) == "max" ? -10.0f : 10.0f;
        for (const auto& closure : { scalar, vector }) {
            particles[0].position = toSimulationVector ({ -1, nan, z });
            cps[2].position = glm::vec3 (5, 7, 11);
            closure (particles, 1, cps, 0.0f, MovementTime {});
            REQUIRE (cps[2].position == glm::vec3 (0.0f));
        }
        REQUIRE (particles[0].size == 1.0f);
        REQUIRE (toAuthoredVector (particles[0].position) == glm::vec3 (1.0f));
    }
    for (int flags = 0; flags <= 3; ++flags) {
        JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
            {"name":"remapinitialvalue","input":"directiontocontrolpoint","inputcontrolpoint0":2,
             "output":"size","operation":"remap"},
            {"name":"remapinitialvalue","input":"directiontocontrolpoint","inputcontrolpoint0":2,
             "output":"position","operation":"remap"}
        ]}})");
        for (auto& initializer : data["particle"]["initializer"]) initializer["flags"] = flags;
        const auto object = ObjectParser::parse (data, project);
        const auto& model = *object->as<Particle> ();
        const auto scalar = createScalarRemapOperator (*model.initializers[0]
            ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
        const auto vector = createVectorRemapOperator (*model.initializers[1]
            ->as<RemapInitialValueInitializer> ()->remap->as<VectorRemapValueOperator> (), true, false, false, false);
        for (const auto authored : { glm::vec3 (0.0f), glm::vec3 (infinity, 0, 0), glm::vec3 (nan, 1, 2) }) {
            std::vector<ParticleInstance> particles (1);
            particles[0].alive = true;
            std::vector<ControlPointData> cps (8);
            for (const auto& closure : { scalar, vector }) {
                cps[2].position = glm::vec3 (5, 7, 11);
                particles[0].position = toSimulationVector (authored);
                closure (particles, 1, cps, 0.0f, MovementTime {});
                REQUIRE (cps[2].position == glm::vec3 (0.0f));
            }
            REQUIRE (std::isnan (particles[0].size));
            REQUIRE (std::isnan (particles[0].position.x));
            if (authored.x == infinity) {
                REQUIRE (particles[0].position.y == 0.0f);
                REQUIRE (particles[0].position.z == 0.0f);
            } else {
                REQUIRE (std::isnan (particles[0].position.y));
                REQUIRE (std::isnan (particles[0].position.z));
            }
        }
    }
    for (const auto transform : { "simplexnoise", "fbmnoise" }) {
        JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
            {"name":"remapinitialvalue","input":"directiontocontrolpoint","inputcontrolpoint0":2,
             "output":"size","operation":"remap"}
        ]}})");
        data["particle"]["initializer"][0]["transformfunction"] = transform;
        const auto object = ObjectParser::parse (data, project);
        const auto& remap = *object->as<Particle> ()->initializers[0]
            ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> ();
        REQUIRE (remap.transform != ScalarRemapValueOperator::Transform::Identity);
        const auto closure = createScalarRemapOperator (remap, true, false, false, false);
        std::vector<ParticleInstance> particles (1);
        particles[0].alive = true;
        particles[0].size = 37.0f;
        std::vector<ControlPointData> cps (8);
        cps[2].position = glm::vec3 (5, 7, 11);
        closure (particles, 1, cps, 0.0f, MovementTime {});
        REQUIRE (cps[2].position == glm::vec3 (0.0f));
        REQUIRE (std::isnan (particles[0].size)); // native raw noise after source CP mutation
    }
}

TEST_CASE ("Extracted production runtime CP readers preserve translation and finite guards",
           "[particle][remap][controlpoint]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const std::array<const char*, 3> inputs { "controlpoint", "deltatocontrolpoint", "directiontocontrolpoint" };
    const std::array<glm::vec3, 3> expected {
        glm::vec3 (60, 80, 0), glm::vec3 (30, 40, 0), glm::vec3 (0.6f, 0.8f, 0) };
    for (size_t index = 0; index < inputs.size (); ++index) {
        JSON data = JSON::parse (R"({"id":1,"particle":{"operator":[]}})");
        for (const auto output : { "size", "position" }) data["particle"]["operator"].push_back (
            JSON {{"name", "remapvalue"}, {"input", inputs[index]}, {"inputcontrolpoint0", 2},
                {"output", output}, {"operation", "remap"}, {"flags", 0}});
        const auto object = ObjectParser::parse (data, project);
        const auto& model = *object->as<Particle> ();
        const auto scalar = createScalarRemapOperator (*model.operators[0]->as<ScalarRemapValueOperator> (),
            false, false, false, false);
        const auto vector = createVectorRemapOperator (*model.operators[1]->as<VectorRemapValueOperator> (),
            false, false, false, false);
        std::vector<ParticleInstance> particles (1);
        particles[0].alive = true;
        std::vector<ControlPointData> cps (8);
        cps[2].position = toSimulationVector ({ 60, 80, 0 });
        for (const auto& closure : { scalar, vector }) {
            particles[0].position = toSimulationVector ({ 30, 40, 0 });
            closure (particles, 1, cps, 0.0f, MovementTime {});
            REQUIRE (toAuthoredVector (cps[2].position) == glm::vec3 (60, 80, 0));
        }
        REQUIRE (particles[0].size == Catch::Approx (expected[index].x));
        const auto actual = toAuthoredVector (particles[0].position);
        for (int axis = 0; axis < 3; ++axis) REQUIRE (actual[axis] == Catch::Approx (expected[index][axis]));
        if (index == 2) {
            particles[0].position = cps[2].position;
            scalar (particles, 1, cps, 0.0f, MovementTime {});
            particles[0].position = cps[2].position;
            vector (particles, 1, cps, 0.0f, MovementTime {});
            REQUIRE (particles[0].size == 0.0f);
            REQUIRE (particles[0].position == glm::vec3 (0.0f));
            REQUIRE (toAuthoredVector (cps[2].position) == glm::vec3 (60, 80, 0));
        }
    }
}

TEST_CASE ("Each parsed turbulence record retains its own blend envelope", "[particle][envelope][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"envelopes","particle":{
        "operator":[
            {"name":"turbulence","blendinstart":0.2,"blendinend":0.4},
            {"name":"turbulence","blendoutstart":0.6,"blendoutend":0.8}
        ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 2);
    REQUIRE (particle->operators[0]->blendEnvelope.has_value ());
    REQUIRE (particle->operators[1]->blendEnvelope.has_value ());
    const auto& first = *particle->operators[0]->blendEnvelope;
    const auto& second = *particle->operators[1]->blendEnvelope;
    REQUIRE (first.inStart->value->getFloat () == 0.2f);
    REQUIRE (first.inEnd->value->getFloat () == 0.4f);
    REQUIRE (first.outStart->value->getFloat () == 1.0f);
    REQUIRE (second.inStart->value->getFloat () == 0.0f);
    REQUIRE (second.outStart->value->getFloat () == 0.6f);
    REQUIRE (second.outEnd->value->getFloat () == 0.8f);
}

TEST_CASE ("Particle instance control points retain authored positions", "[particle][parser][controlpoint]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":24061,"name":"birds",
        "instanceoverride":{"controlpoint1":"4058.95117 199.17044 0",
                            "controlpoint2":"3333.52368 420.87238 0",
                            "controlpointangle1":"0.1 0.2 0.3"},
        "particle":{"controlpoint":[{"id":1,"offset":"1500 0 0"},
                                    {"id":2,"offset":"123 0 0"}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    const auto& overrides = particle->instanceOverride.controlPoints;
    REQUIRE (overrides[0] == nullptr);
    REQUIRE (overrides[1] != nullptr);
    REQUIRE (overrides[2] != nullptr);
    REQUIRE (overrides[1]->value->getVec3 ().x == Catch::Approx (4058.95117f));
    REQUIRE (overrides[1]->value->getVec3 ().y == Catch::Approx (199.17044f));
    REQUIRE (overrides[2]->value->getVec3 ().x == Catch::Approx (3333.52368f));
    REQUIRE (overrides[2]->value->getVec3 ().y == Catch::Approx (420.87238f));
    REQUIRE (overrides[3] == nullptr);
    const auto& angles = particle->instanceOverride.controlPointAngles;
    REQUIRE (angles[0] == nullptr);
    REQUIRE (angles[1] != nullptr);
    REQUIRE (angles[1]->value->getVec3 ().x == Catch::Approx (0.1f));
    REQUIRE (angles[1]->value->getVec3 ().y == Catch::Approx (0.2f));
    REQUIRE (angles[1]->value->getVec3 ().z == Catch::Approx (0.3f));
    REQUIRE (angles[2] == nullptr);
    std::map<std::string, WallpaperEngine::Data::Model::DynamicValue*> scriptSettings;
    WallpaperEngine::Scripting::forEachParticleScriptSetting (*particle,
        [&scriptSettings] (const std::string& key, auto& value) {
            scriptSettings.emplace (key, &value);
        });
    REQUIRE (scriptSettings.at ("instance_controlpoint1") == overrides[1]->value.get ());
    REQUIRE (scriptSettings.at ("instance_controlpointangle1") == angles[1]->value.get ());
}

TEST_CASE ("Instance control points use authored overrides only on writable slots", "[particle][controlpoint]") {
    const glm::vec3 offset {1500.0f, 27.0f, 0.0f};
    const glm::vec3 override {4058.95117f, 199.17044f, 0.0f};
    const glm::vec3 absent {std::numeric_limits<float>::max (), 0.0f, 0.0f};
    REQUIRE (instanceControlPointPosition (offset, override, 0u)
             == glm::vec3 (override.x, -override.y, 0.0f));
    REQUIRE (instanceControlPointPosition (offset, absent, 0u)
             == glm::vec3 (offset.x, -offset.y, 0.0f));
    REQUIRE (instanceControlPointPosition (offset, override, 0x10005u)
             == glm::vec3 (offset.x, -offset.y, 0.0f));
}

TEST_CASE ("Parsed particle rate scales node age and emitter delay exactly once", "[particle][rate][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"rate","instanceoverride":{"rate":1},"particle":{
        "emitter":[{"name":"boxrandom","rate":10,"delay":0.2,"duration":0.3}]}})");
    struct Snapshot { float age; float delay; uint32_t births; };
    auto run = [&] (float rate, int frames) {
        data["instanceoverride"]["rate"] = rate;
        const auto object = ObjectParser::parse (data, project);
        const auto* particle = dynamic_cast<const Particle*> (object.get ());
        REQUIRE (particle != nullptr);
        REQUIRE (particle->emitters.size () == 1);
        const float parsedRate = particle->instanceOverride.rate->value->getFloat ();
        const auto& emitter = particle->emitters.front ();
        auto schedule = scheduleConfig (emitter, emitter.rate);
        auto state = initialState (schedule);
        Snapshot result { 0.0f, 0.0f, 0 };
        for (int i = 0; i < frames; ++i) {
            const auto clock = tickClock (0.1f, 0.1f * nodeRate (parsedRate), 30);
            dispatchTick (clock, [&] (float dt) {
                result.age += dt;
                result.births += advanceEmitter (schedule, state, dt, 100,
                    [] (float min, float) { return min; });
            }, [] (MovementTime) {});
        }
        result.delay = state.delayRemaining;
        return result;
    };

    const auto slow = run (0.5f, 2);
    REQUIRE (slow.age == 0.1f);
    REQUIRE (std::abs (slow.delay - 0.1f) < 0.000001f);
    REQUIRE (slow.births == 0);
    const auto fast = run (2.0f, 2);
    REQUIRE (fast.age == 0.4f);
    REQUIRE (fast.delay == 0.0f);
    REQUIRE (fast.births == 2); // emitter rate 10 * node dt .2, not 10*2*.2
    REQUIRE (nodeRate (0.0f) == 0.01f);
    REQUIRE (warmupClock (0.05f, 30).nodeDuration == 0.05f);
}

TEST_CASE ("Native scene clock splits only operator passes at configured low FPS", "[particle][static]") {
    const auto low = tickClock (0.1f, 0.2f, 20);
    REQUIRE (low.nodeDuration == 0.2f);
    REQUIRE (low.operatorPasses == 2);
    REQUIRE (low.operatorTime.integration == 0.1f);
    REQUIRE (low.operatorTime.damping == dampingTime (0.1f, 0.2f) * 0.5f);
    REQUIRE (low.operatorTime.damping != dampingTime (0.2f, 0.2f) * 0.5f);

    int ageEmissionCalls = 0;
    int operatorCalls = 0;
    float ageDuration = 0.0f;
    float integrationTotal = 0.0f;
    float dampingTotal = 0.0f;
    dispatchTick (low, [&] (float duration) {
        ++ageEmissionCalls;
        ageDuration += duration;
    }, [&] (MovementTime time) {
        ++operatorCalls;
        integrationTotal += time.integration;
        dampingTotal += time.damping;
    });
    REQUIRE (ageEmissionCalls == 1);
    REQUIRE (operatorCalls == 2);
    REQUIRE (ageDuration == 0.2f);
    REQUIRE (integrationTotal == 0.2f);
    REQUIRE (std::abs (dampingTotal - dampingTime (0.1f, 0.2f)) < 0.000001f);

    for (uint32_t fps : { 0u, 21u, 30u }) {
        const auto single = tickClock (0.1f, 0.2f, fps);
        REQUIRE (single.operatorPasses == 1);
        REQUIRE (single.operatorTime.integration == 0.2f);
        REQUIRE (single.operatorTime.damping == dampingTime (0.1f, 0.2f));
    }
    REQUIRE (tickClock (0.1f, 0.2f, 1).operatorPasses == 2);
    REQUIRE (tickClock (0.1f, 0.2f, 0).operatorPasses == 1);
    REQUIRE (tickClock (0.0f, 0.2f, 20).operatorPasses == 0);
    REQUIRE (tickClock (0.1f, std::numeric_limits<float>::infinity (), 20).operatorPasses == 0);
}

TEST_CASE ("Native particle warm-up uses full fixed steps and a saturated damping clock", "[particle][static]") {
    const auto small = warmupPlan (0.1f, 499);
    REQUIRE (small.step == 0.05f);
    REQUIRE (small.steps == 2);
    REQUIRE_FALSE (small.truncated);

    const auto large = warmupPlan (0.21f, 500);
    REQUIRE (large.step == 0.2f);
    REQUIRE (large.steps == 2); // The second full step crosses starttime.
    REQUIRE_FALSE (large.truncated);
    REQUIRE (warmupPlan (0.80000001f, 499).steps == 16);
    REQUIRE (warmupPlan (5.0f, 500).steps == 26); // Repeated float32 addition, not ceil(5 / .2).
    REQUIRE (warmupPlan (0.0f, 499).steps == 0);
    REQUIRE (warmupPlan (std::numeric_limits<float>::infinity (), 499).steps == 0);

    const auto capped = warmupPlan (1.0e9f, 499);
    REQUIRE (capped.steps == MAX_WARMUP_STEPS);
    REQUIRE (capped.truncated);

    const auto split = warmupClock (small.step, 20);
    REQUIRE (split.nodeDuration == 0.05f);
    REQUIRE (split.operatorPasses == 2);
    REQUIRE (split.operatorTime.integration == 0.025f);
    REQUIRE (split.operatorTime.damping == 0.025f);
    REQUIRE (warmupClock (large.step, 30).operatorTime.damping == large.step);
    REQUIRE (warmupClock (0.0f, 20).operatorPasses == 0);

    int ageEmissionCalls = 0;
    int operatorCalls = 0;
    auto ageAndEmit = [&] (float) { ++ageEmissionCalls; };
    auto operators = [&] (MovementTime) { ++operatorCalls; };
    dispatchTick (warmupClock (0.05f, 20), ageAndEmit, operators);
    dispatchTick (tickClock (0.016f, 0.016f, 20), ageAndEmit, operators);
    dispatchTick (tickClock (0.0f, 0.0f, 20), ageAndEmit, operators);
    REQUIRE (ageEmissionCalls == 2); // Warm-up and first positive live tick.
    REQUIRE (operatorCalls == 4);
}

TEST_CASE ("Lifetime expiry uses a strict boundary and preserves zero lifetime", "[particle][boundary]") {
    REQUIRE (isAlive (true, 0.5f, 0.5f));
    REQUIRE_FALSE (isAlive (true, std::nextafter (0.5f, 1.0f), 0.5f));
    REQUIRE (isAlive (true, 1.0f, 0.0f));
    REQUIRE_FALSE (isAlive (false, 0.0f, 0.0f));
}

TEST_CASE ("Event-child emitter completion releases its active slot", "[particle][child]") {
    EmitterScheduleConfig oneShot;
    oneShot.instantaneous = 1;
    auto state = initialState (oneShot);
    REQUIRE (emitterCanProduceMore (oneShot, state));
    REQUIRE (advanceEmitter (oneShot, state, 0.02f, 1,
        [] (float, float) { return 0.0f; }) == 1);
    REQUIRE_FALSE (emitterCanProduceMore (oneShot, state));

    EmitterScheduleConfig delayed = oneShot;
    delayed.delay = 0.1f;
    state = initialState (delayed);
    REQUIRE (advanceEmitter (delayed, state, 0.05f, 1,
        [] (float, float) { return 0.0f; }) == 0);
    REQUIRE (emitterCanProduceMore (delayed, state));

    EmitterScheduleConfig finite;
    finite.rate = 10.0f;
    finite.duration = 0.1f;
    state = initialState (finite);
    REQUIRE (emitterCanProduceMore (finite, state));
    advanceEmitter (finite, state, 0.1f, 1,
        [] (float, float) { return 0.0f; });
    REQUIRE_FALSE (emitterCanProduceMore (finite, state));

    EmitterScheduleConfig continuous;
    continuous.rate = 10.0f;
    state = initialState (continuous);
    advanceEmitter (continuous, state, 0.1f, 1,
        [] (float, float) { return 0.0f; });
    REQUIRE (emitterCanProduceMore (continuous, state));
}

TEST_CASE ("Scripted particle births borrow future emitter rate credit", "[particle][script]") {
    EmitterScheduleConfig config;
    config.rate = 4.0f;
    config.instantaneous = 2;
    auto state = initialState (config);
    const auto zeroRandom = [] (float, float) { return 0.0f; };
    REQUIRE (advanceEmitterForced (config, state, 3, 2, zeroRandom) == 2);
    REQUIRE (state.instantaneousRemaining == 0);
    REQUIRE (state.fractional == -5.0f); // Requested births, before pool clipping.
    REQUIRE (advanceEmitter (config, state, 1.0f, 10, zeroRandom) == 0);
    REQUIRE (state.fractional == -1.0f);
    REQUIRE (advanceEmitter (config, state, 0.5f, 10, zeroRandom) == 1);
    REQUIRE (state.fractional == 0.0f);

    config.delay = 0.5f;
    state = initialState (config);
    REQUIRE (advanceEmitterForced (config, state, 3, 10, zeroRandom) == 3);
    REQUIRE (state.instantaneousRemaining == 2);
    REQUIRE (state.fractional == 0.0f);

    config.delay = 0.0f;
    config.periodic = true;
    config.minActiveDuration = config.maxActiveDuration = 0.25f;
    state = initialState (config);
    bool periodRestarted = false;
    REQUIRE (advanceEmitterForced (config, state, 1, 10,
        [] (float min, float) { return min; }, &periodRestarted) == 3);
    REQUIRE (periodRestarted);
    REQUIRE (state.periodTimer == 0.25f);
    REQUIRE (state.fractional == -3.0f);
    state.periodTimer = 0.0f;
    REQUIRE (advanceEmitterForced (config, state, 1, 10,
        [] (float min, float) { return min; }, &periodRestarted) == 3);
    REQUIRE (periodRestarted);
    REQUIRE (state.instantaneousRemaining == 0);
    REQUIRE (state.fractional == -6.0f);
    REQUIRE (advanceEmitterForced (config, state, 1, 10,
        [] (float min, float) { return min; }, &periodRestarted) == 1);
    REQUIRE_FALSE (periodRestarted);

    config.periodic = false;
    config.instantaneous = 0;
    state = initialState (config);
    state.fractional = 1.75f;
    REQUIRE (advanceEmitterForced (config, state, 2, 10, zeroRandom) == 3);
    REQUIRE (state.fractional == -1.25f);

    // Native's paused forced path bypasses scheduler credit consumption. A
    // scripted birth during pause must not delay subsequent rate emissions.
    config.rate = 2.0f;
    config.instantaneous = 2;
    config.periodic = true;
    state = initialState (config);
    state.fractional = 0.25f;
    state.periodTimer = 0.0f;
    int randomCalls = 0;
    periodRestarted = true;
    REQUIRE (advanceEmitterForced (config, state, 3, 2,
        [&] (float min, float) { ++randomCalls; return min; }, &periodRestarted, true) == 2);
    REQUIRE (state.fractional == 0.25f);
    REQUIRE (state.instantaneousRemaining == 2);
    REQUIRE (state.periodTimer == 0);
    REQUIRE (randomCalls == 0);
    REQUIRE_FALSE (periodRestarted);
    config.periodic = false;
    config.instantaneous = 0;
    state.instantaneousRemaining = 0;
    REQUIRE (advanceEmitter (config, state, 0.4f, 10, zeroRandom) == 1);
    REQUIRE (state.fractional == Catch::Approx (0.05f));
    state.expired = true;
    REQUIRE (advanceEmitterForced (config, state, 1, 10, zeroRandom) == 1);
    REQUIRE (state.expired);
    REQUIRE (state.fractional == Catch::Approx (0.05f));
}

TEST_CASE ("Native scene MT draw uses top 24 bits across twists", "[particle][random]") {
    // Constants come from tools/reversing/particle_mt19937_reference.py,
    // an independent scalar MT19937 implementation.
    struct Sample { int index; uint32_t raw; float unit; };
    constexpr Sample samples[] = {
        {0, 3499211612u, 0.8147236704826355f},
        {1, 581869302u, 0.1354769468307495f},
        {2, 3890346734u, 0.9057918787002563f},
        {9, 1323567403u, 0.308167040348053f},
        {622, 2227348307u, 0.5185949206352234f},
        {623, 4020325887u, 0.9360550045967102f},
        {624, 4178893912u, 0.9729745388031006f},
        {625, 610818241u, 0.14221715927124023f},
        {1246, 2862235859u, 0.6664161682128906f},
        {1247, 2538210759u, 0.5909731984138489f},
        {1248, 358555951u, 0.08348280191421509f},
        {1249, 2442940989u, 0.5687915086746216f},
    };
    std::mt19937 raw (5489), unit (5489);
    for (int i = 0; i < 1250; ++i) {
        const uint32_t value = raw ();
        const float fraction = nativeRandomUnit (unit);
        for (const auto& sample : samples) {
            if (sample.index != i) continue;
            REQUIRE (value == sample.raw);
            REQUIRE (fraction == sample.unit);
        }
    }
}

TEST_CASE ("Native particle slots refill lowest holes below high water", "[particle][child][pool]") {
    NativeSlotAllocation slots;
    REQUIRE (slots.allocate (3) == 0u);
    REQUIRE (slots.allocate (3) == 1u);
    REQUIRE (slots.allocate (3) == 2u);
    REQUIRE (slots.highWater () == 3u);
    REQUIRE_FALSE (slots.allocate (3).has_value ());
    slots.release (2);
    slots.release (0);
    REQUIRE (slots.allocate (3) == 0u);
    REQUIRE (slots.allocate (3) == 2u);
    REQUIRE (slots.highWater () == 3u);
    slots.release (2);
    REQUIRE_FALSE (slots.allocate (2).has_value ());
    REQUIRE (slots.allocate (3) == 2u);
}

TEST_CASE ("Rope rows remain with birth identities when a low SoA slot refills", "[particle][rope][pool]") {
    RopeTrailHistory history (3, 3);
    history.born (0, { 10.0f, 0.0f, 0.0f });
    history.born (1, { 20.0f, 0.0f, 0.0f });
    history.sample (0, { 11.0f, 0.0f, 0.0f });
    history.sample (1, { 21.0f, 0.0f, 0.0f });
    history.compact (0, 1); // slot 0 expires; slot 1 survives
    history.born (1, { 30.0f, 0.0f, 0.0f }); // new particle reuses slot 0
    history.reorder ({1, 0});
    REQUIRE (history.at (0, 0).x == 30.0f);
    REQUIRE (history.counts[0] == 1);
    REQUIRE (history.at (1, 0).x == 21.0f);
    REQUIRE (history.at (1, 1).x == 20.0f);
    REQUIRE (history.counts[1] == 2);
}

TEST_CASE ("Particle random ranges preserve native draw order and reversed bounds", "[particle][random]") {
    // Independent scalar reference: tools/reversing/particle_mt19937_reference.py.
    std::mt19937 rng (5489u);
    const glm::vec3 value = WallpaperEngine::Maths::randomVec3 (
        rng, {1.0f, 9.0f, -7.0f}, {10.0f, -3.0f, 7.0f});
    REQUIRE (value.x == 8.332512855529785f);
    REQUIRE (value.y == 7.374276638031006f);
    REQUIRE (value.z == 5.681086540222168f);
    REQUIRE (WallpaperEngine::Maths::randomFloat (rng, 4.0f, 4.0f) == 4.0f);
    REQUIRE (WallpaperEngine::Maths::randomFloat (rng, 0.0f, 1.0f)
             == 0.1269868016242981f);
}

TEST_CASE ("Native color-random initializer shares one birth draw across RGB", "[particle][random][color]") {
    std::mt19937 rng (5489u);
    const glm::vec3 sampled = colorRandomSample (
        rng, glm::vec3 (0.0f, 10.0f, 20.0f), glm::vec3 (10.0f, 20.0f, 30.0f));
    const float first = static_cast<float> (3499211612u >> 8) * (1.0f / 16777216.0f);
    REQUIRE (sampled.x == Catch::Approx (10.0f * first));
    REQUIRE (sampled.y == Catch::Approx (10.0f + 10.0f * first));
    REQUIRE (sampled.z == Catch::Approx (20.0f + 10.0f * first));
    REQUIRE (rng () == 581869302u); // second MT word remains for the next initializer
}

TEST_CASE ("Native box emitter consumes Z Y X draws and maps signed ranges", "[particle][random][box]") {
    // Independent scalar reference: tools/reversing/particle_mt19937_reference.py.
    std::mt19937 rng (5489u);
    const glm::vec3 displacement = nativeBoxDisplacement (
        rng, {2.0f, -0.5f, 1.5f}, {0.2f, 0.3f, 0.4f}, {1.2f, 1.3f, 1.4f});
    REQUIRE (displacement.x == 1.8231675624847412f);
    REQUIRE (displacement.y == 0.6645230054855347f);
    REQUIRE (displacement.z == 1.3441710472106934f);
    REQUIRE (rng () == 3586334585u); // speed's fourth draw in opcode 2

    std::mt19937 zeroRange (5489u);
    REQUIRE (nativeBoxDisplacement (zeroRange, glm::vec3 (0.0f),
                                    glm::vec3 (0.0f), glm::vec3 (0.0f))
             == glm::vec3 (0.0f));
    REQUIRE (zeroRange () == 3586334585u);
    REQUIRE (nativeBoxAxis (0.5f, 1.0f, 0.2f, 1.2f) == 0.0f);
    REQUIRE (nativeBoxAxis (0.0f, 1.0f, 0.2f, 1.2f) == -1.2f);
}

TEST_CASE ("Native box fallback uses the one-step inverse-sqrt threshold", "[particle][random][box]") {
    // Independent bit-level reference: tools/reversing/particle_box_threshold_reference.py.
    REQUIRE (nativeBoxNeedsFallback (glm::vec3 (0.0f)));
    REQUIRE (nativeBoxNeedsFallback (glm::vec3 (
        std::bit_cast<float> (0x38d15c2bu), 0.0f, 0.0f)));
    REQUIRE_FALSE (nativeBoxNeedsFallback (glm::vec3 (
        std::bit_cast<float> (0x38d15c2cu), 0.0f, 0.0f)));
    REQUIRE_FALSE (nativeBoxNeedsFallback (glm::vec3 (
        9.985000360757113e-05f, 0.0f, 0.0f)));
}

TEST_CASE ("Native sphere cone uses angle axial and cube-root radius draws", "[particle][random][sphere]") {
    std::mt19937 rng (5489u);
    const float angle = nativeRandomUnit (rng);
    const float axial = nativeRandomUnit (rng);
    const float radius = nativeRandomUnit (rng);
    const auto ordinary = nativeSphereDisplacement (
        angle, axial, radius, glm::vec3 (1.0f, 1.0f, 0.0f),
        2.0f, 8.0f, 0.0f, glm::ivec3 (0));
    const auto asymmetric = nativeSphereDisplacement (
        angle, axial, radius, glm::vec3 (0.8f, 1.4f, 0.6f),
        2.0f, 8.0f, 0.35f, glm::ivec3 (1, -1, 0));
    // Independent scalar values: tools/reversing/particle_sphere_reference.py.
    REQUIRE (std::abs (ordinary.x + 5.74702006f) < 0.001f);
    REQUIRE (std::abs (ordinary.y + 4.95553798f) < 0.001f);
    REQUIRE (ordinary.z == 0.0f);
    REQUIRE (std::abs (asymmetric.x - 1.51481683f) < 0.001f);
    REQUIRE (std::abs (asymmetric.y + 9.15512272f) < 0.001f);
    REQUIRE (std::abs (asymmetric.z - 1.68982179f) < 0.001f);
    REQUIRE (nativeSphereDisplacement (angle, axial, radius, glm::vec3 (0.0f),
        2.0f, 8.0f, 0.0f, glm::ivec3 (0)) == glm::vec3 (0.0f));
    REQUIRE (rng () == 3586334585u);
}

TEST_CASE ("Image emitter retains alpha-threshold pixels in X-major order", "[particle][imageemitter]") {
    REQUIRE (imageEmitterReadbackSize (1, 1) == glm::uvec2 (2, 2));
    REQUIRE (imageEmitterReadbackSize (8, 6) == glm::uvec2 (2, 2));
    REQUIRE (imageEmitterReadbackSize (3840, 2160) == glm::uvec2 (960, 540));
    REQUIRE (imageEmitterReadbackSize (8000, 2000) == glm::uvec2 (960, 240));
    REQUIRE (imageEmitterReadbackSize (2000, 8000) == glm::uvec2 (135, 540));
    // Source 8x6 and quarter readback 2x2: source-width ratio 4 also
    // controls Y, and source center uses integer halves.
    const std::vector<uint8_t> rgba {
        10, 11, 12, 126, 30, 31, 32, 127,
        20, 21, 22, 255, 40, 41, 42, 0,
    };
    const auto samples = imageEmitterSamples (rgba, 2, 2, 8, 6);
    REQUIRE (samples.size () == 2);
    REQUIRE (samples[0].red == 20);
    REQUIRE (samples[0].green == 21);
    REQUIRE (samples[0].bone == 0xff);
    REQUIRE (samples[0].x == -2);
    REQUIRE (samples[0].y == 3);
    REQUIRE (samples[1].red == 30);
    REQUIRE (samples[1].x == 2);
    REQUIRE (samples[1].y == -1);

    std::mt19937 one (5489u);
    REQUIRE (nativeImageSampleIndex (one, 1) == 0);
    REQUIRE (one () == 581869302u); // one-entry selection still draws
    std::mt19937 two (5489u);
    REQUIRE (nativeImageSampleIndex (two, 2) == 1);
    REQUIRE (two () == 581869302u);
    std::mt19937 three (5489u);
    REQUIRE (nativeImageSampleIndex (three, 3) == 2);
    REQUIRE (three () == 581869302u);
    std::mt19937 empty (5489u);
    REQUIRE (nativeImageSampleIndex (empty, 0) == 0);
    REQUIRE (empty () == 3499211612u);
    // Independent 64-bit product: 3499211612*0xc0000000 has low word 0,
    // below threshold 0x40000000; next word 581869302 selects 436401976.
    std::mt19937 rejected (5489u);
    REQUIRE (nativeImageSampleIndex (rejected, 0xc0000000u) == 436401976u);
    REQUIRE (rejected () == 3890346734u);
}

TEST_CASE ("Local control-point angles rotate box axes after Y reflection", "[particle][child][box]") {
    const glm::mat3 basis = localControlPointBasis (
        {0.0f, 0.0f, glm::half_pi<float> ()});
    const glm::vec3 x = basis * glm::vec3 (10.0f, 0.0f, 0.0f);
    REQUIRE (std::abs (x.x) < 1e-5f);
    REQUIRE (std::abs (x.y + 10.0f) < 1e-5f);
    REQUIRE (std::abs (x.z) < 1e-5f);
    const glm::mat3 oblique = localControlPointBasis ({0.2f, -0.4f, 0.7f});
    const glm::vec3 sample = oblique * glm::vec3 (2.0f, -3.0f, 4.0f);
    REQUIRE (std::abs (sample.x + 1.3183942f) < 1e-5f);
    REQUIRE (std::abs (sample.y + 1.6947151f) < 1e-5f);
    REQUIRE (std::abs (sample.z - 4.9386008f) < 1e-5f);
}

TEST_CASE ("Emitter rate saturation avoids undefined integer conversion", "[particle][boundary]") {
    EmitterScheduleConfig config;
    config.rate = 1.0e30f;
    auto state = initialState (config);
    REQUIRE (advanceEmitter (config, state, 0.1f, 3,
        [] (float, float) { return 0.0f; }) == 3);
    REQUIRE (state.fractional == 0.0f);

    config.rate = std::numeric_limits<float>::max ();
    state = initialState (config);
    REQUIRE (advanceEmitter (config, state, std::numeric_limits<float>::max (), 2,
        [] (float, float) { return 0.0f; }) == 2);
    REQUIRE (std::isfinite (state.fractional));

    config.rate = std::numeric_limits<float>::infinity ();
    state = initialState (config);
    REQUIRE (advanceEmitter (config, state, 0.1f, 3,
        [] (float, float) { return 0.0f; }) == 0);
    REQUIRE (state.fractional == 0.0f);
    REQUIRE (effectiveRate (1.0e30f, 1.0e30f) == std::numeric_limits<float>::max ());
    REQUIRE (effectiveRate (7.0f, -1.0f) == 0.0f);

    config.rate = 200.0f;
    config.instantaneous = std::numeric_limits<uint32_t>::max ();
    state = initialState (config);
    REQUIRE (advanceEmitter (config, state, 0.01f, 1,
        [] (float, float) { return 0.0f; }) == 1);
}

TEST_CASE ("Native periodic quota counts the capped request before pool clamping", "[particle][static]") {
    EmitterScheduleConfig config;
    config.rate = 200.0f;
    config.periodic = true;
    config.maxPerPeriod = 2;
    config.minActiveDuration = config.maxActiveDuration = 0.4f;
    auto state = initialState (config);
    REQUIRE (advanceEmitter (config, state, 0.01f, 1,
        [] (float min, float) { return min; }) == 1);
    REQUIRE (state.emittedThisPeriod == 2);
    REQUIRE (advanceEmitter (config, state, 0.01f, 1,
        [] (float min, float) { return min; }) == 0);
}

TEST_CASE ("Parsed emitter cap and rate feed the renderer's shared schedule adapter", "[particle][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"schedule", "particle":{
        "emitter":[{"name":"boxrandom","rate":7,"flags":4,
        "minperiodicduration":0.4,"maxperiodicduration":0.4,
        "minperiodicdelay":0.2,"maxperiodicdelay":0.2,
        "maxtoemitperperiod":2}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->emitters.size () == 1);
    const auto& emitter = particle->emitters.front ();
    REQUIRE (emitter.maxToEmitPerPeriod == 2);
    const auto config = scheduleConfig (emitter, effectiveRate (emitter.rate, 1.0f));
    REQUIRE (config.periodic);
    REQUIRE (config.maxPerPeriod == 2);
    REQUIRE (config.rate == 7.0f);

    auto invalid = data;
    invalid["particle"]["emitter"][0]["rate"] = -1.0f;
    REQUIRE_THROWS_AS (ObjectParser::parse (invalid, project), std::invalid_argument);

    auto large = data;
    large["particle"]["emitter"][0]["rate"] = 1.0e30f;
    const auto largeObject = ObjectParser::parse (large, project);
    const auto* largeParticle = dynamic_cast<const Particle*> (largeObject.get ());
    REQUIRE (largeParticle != nullptr);
    const auto largeConfig = scheduleConfig (largeParticle->emitters.front (),
        effectiveRate (largeParticle->emitters.front ().rate, 1.0f));
    auto largeState = initialState (largeConfig);
    REQUIRE (advanceEmitter (largeConfig, largeState, 0.1f, 3,
        [] (float min, float) { return min; }) == 2);
}

TEST_CASE ("Particle pool keeps authored capacity while count controls production", "[particle][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"count","particle":{"maxcount":0}})");
    auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->maxCount == 0);
    REQUIRE (particleCapacity (particle->maxCount) == 0);

    data["particle"]["maxcount"] = 100;
    data["instanceoverride"] = JSON::parse (R"({"count":0})");
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particleCapacity (particle->maxCount) == 100);
    REQUIRE (effectiveRate (10.0f,
        particle->instanceOverride.count->value->getFloat ()) == 0.0f);
    REQUIRE (particleCapacity (40) == 40);
    REQUIRE (effectiveRate (100.0f, 0.07f) == Catch::Approx (7.0f));
    REQUIRE (particleCapacity (25000) == 25000);
    REQUIRE (particleCapacity (250000) == MAX_PARTICLE_CAPACITY);

    data["particle"]["maxcount"] = -1;
    REQUIRE_THROWS_AS (ObjectParser::parse (data, project), std::invalid_argument);
    data["particle"]["maxcount"] = 2.5;
    REQUIRE_THROWS_AS (ObjectParser::parse (data, project), std::invalid_argument);
}

TEST_CASE ("Child particle records retain native flags and event configuration", "[particle][child][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"child-parent","particle":{"children":[
        {"type":"static","particle":"particles/child.json","flags":5,"controlpointstartindex":3,
         "origin":"2 4 6","angles":"10 20 30","scale":"1 2 3"},
        {"type":"eventdeath","particle":"particles/other.json","probability":0.25,"maxcount":7}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->children.size () == 2);
    REQUIRE (particle->children[0].flags == 5);
    REQUIRE (particle->children[0].controlPointStartIndex == 3);
    REQUIRE (particle->children[0].origin == glm::vec3 (2.0f, 4.0f, 6.0f));
    REQUIRE (particle->children[0].angles == glm::vec3 (10.0f, 20.0f, 30.0f));
    REQUIRE (particle->children[0].scale == glm::vec3 (1.0f, 2.0f, 3.0f));
    REQUIRE (particle->children[1].type == "eventdeath");
    REQUIRE (particle->children[1].flags == 0);
    REQUIRE (particle->children[1].probability == 0.25f);
    REQUIRE (particle->children[1].maxCount == 7);
}

TEST_CASE ("Particle control points retain parent and angle records", "[particle][child][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"control-points","particle":{
        "controlpoint":[
            {"id":2,"flags":4,"parentcontrolpoint":5,
             "offset":"2 -3 4","angles":"10 20 -30"},
            {"id":3,"offset":"1 2 3"}
        ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->controlPoints.size () == 2);
    REQUIRE (particle->controlPoints[0].flags == 4);
    REQUIRE (particle->controlPoints[0].parentControlPoint == 5);
    REQUIRE (particle->controlPoints[0].offset == glm::vec3 (2.0f, -3.0f, 4.0f));
    REQUIRE (particle->controlPoints[0].angles == glm::vec3 (10.0f, 20.0f, -30.0f));
    REQUIRE (particle->controlPoints[1].parentControlPoint == 0);
    REQUIRE (particle->controlPoints[1].angles == glm::vec3 (0.0f));
}

TEST_CASE ("Particle renderer retains orientation axis and byte flags", "[particle][renderer][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"axis","particle":{"renderer":[
        {"name":"sprite","orientation":"fixed","axis":"2 -3 4","flags":257},
        {"name":"spritetrail","orientation":"upright","axis":"-1 2 0","flags":2}
    ]}})");
    auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->renderers.size () == 2);
    REQUIRE (particle->renderers[0].orientation == "fixed");
    REQUIRE (particle->renderers[0].axis == glm::vec3 (2.0f, -3.0f, 4.0f));
    REQUIRE (particle->renderers[0].flags == 1);
    REQUIRE (particle->renderers[1].orientation == "upright");
    REQUIRE (particle->renderers[1].axis == glm::vec3 (-1.0f, 2.0f, 0.0f));
    REQUIRE (particle->renderers[1].flags == 2);

    data["particle"]["renderer"] = JSON::array ();
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->renderers.size () == 1);
    REQUIRE (particle->renderers[0].orientation == "screen");
    REQUIRE (particle->renderers[0].axis == glm::vec3 (0.0f));
    REQUIRE (particle->renderers[0].flags == 0);
}

TEST_CASE ("Rope geometry rejects oversized or uncastable subdivisions before allocation", "[particle][boundary]") {
    const auto shipped = ropeGeometry (25000, 3.0f, 26);
    REQUIRE (shipped.has_value ());
    REQUIRE (shipped->subdivision == 3);
    REQUIRE (shipped->floatCount == static_cast<size_t> (24999) * 3 * 4 * 26);
    REQUIRE (shipped->indexCount == static_cast<size_t> (24999) * 3 * 6);
    REQUIRE_FALSE (ropeGeometry (MAX_PARTICLE_CAPACITY, 1000.0f, 26).has_value ());
    REQUIRE_FALSE (ropeGeometry (1, std::numeric_limits<float>::max (), 26).has_value ());
    REQUIRE_FALSE (ropeGeometry (1, std::numeric_limits<float>::infinity (), 26).has_value ());
    REQUIRE_FALSE (ropeGeometry (1, -1.0f, 26).has_value ());
    const auto zero = ropeGeometry (0, 0.0f, 26);
    REQUIRE (zero.has_value ());
    REQUIRE (zero->floatCount == 0);
    REQUIRE (zero->indexCount == 0);
}

TEST_CASE ("Ordinary rope authored subdivision counts interior vertices", "[particle][rope]") {
    for (const auto [authored, rendered] : std::array<std::pair<float, int>, 6> {
             {{-2.0f, 1}, {0.0f, 1}, {1.0f, 2}, {3.0f, 4}, {32.0f, 33}, {99.0f, 33}}}) {
        const auto geometry = ropeOrdinaryGeometry (2, authored, 28);
        REQUIRE (geometry.has_value ());
        CHECK (geometry->subdivision == rendered);
        CHECK (geometry->floatCount == static_cast<size_t> (rendered * 4 * 28));
        CHECK (geometry->indexCount == static_cast<size_t> (rendered * 6));
    }
    CHECK_FALSE (ropeOrdinaryGeometry (2, std::numeric_limits<float>::quiet_NaN (), 28));
}

TEST_CASE ("Ordinary rope UV follows birth index and age, not curve distance", "[particle][rope]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    const auto young = ropeOrdinaryUV (2, 32, .94f, 8.0f, .2f, 1.0f,
                                      false, true);
    REQUIRE (young.trailLength == Catch::Approx (2.0f));
    REQUIRE (young.positionOffset == 0.0f);
    const auto first = ropeOrdinarySegmentUV (0, young);
    REQUIRE (first.start == Catch::Approx (1.0f));
    REQUIRE (first.end == Catch::Approx (0.0f));
    REQUIRE (ropeTrailSubsegmentUV (first, .25f) == Catch::Approx (.84375f));

    // Lifetime smoothing is an oldest-particle age phase and starts only as
    // the live count approaches the rate * lifetime window.
    const auto mature = ropeOrdinaryUV (7, 32, .94f, 8.0f, .91f, 1.0f,
                                       false, true);
    REQUIRE (mature.trailLength == Catch::Approx (6.52f));
    REQUIRE (mature.positionOffset == Catch::Approx (-.76f).margin (.00001f));
    const auto a = ropeOrdinarySegmentUV (0, mature);
    const auto b = ropeOrdinarySegmentUV (1, mature);
    REQUIRE (a.end == Catch::Approx (b.start));
    REQUIRE (ropeTrailSubsegmentUV (a, 1.0f)
             == Catch::Approx (ropeTrailSubsegmentUV (b, 0.0f)));
    const auto terminal = ropeOrdinarySegmentUV (5, mature);
    REQUIRE (terminal.end == Catch::Approx (1.0f - (6.0f - .76f) / 5.52f));

    const auto unsmoothed = ropeOrdinaryUV (7, 32, .94f, 8.0f, .91f, 2.0f,
                                           false, false);
    REQUIRE (unsmoothed.trailLength == Catch::Approx (3.5f));
    REQUIRE (unsmoothed.positionOffset == 0.0f);
    const auto scrolled = ropeOrdinaryUV (7, 32, .94f, 8.0f, .91f, 1.0f,
                                         true, true, 3);
    REQUIRE (scrolled.trailLength == Catch::Approx (6.52f));
    REQUIRE (scrolled.positionOffset == Catch::Approx (3.0f));
    const auto capped = ropeOrdinaryUV (10, 10, 1.0f, 100.0f, .995f, 1.0f,
                                       false, true);
    REQUIRE (capped.trailLength == Catch::Approx (9.0f));
    REQUIRE (capped.positionOffset == Catch::Approx (-.95f));
    const auto negativeScale = ropeOrdinaryUV (2, 32, .94f, 8.0f, .2f, -2.0f,
                                              false, false);
    REQUIRE (negativeScale.trailLength == Catch::Approx (-1.0f));
}

TEST_CASE ("Rope trail history follows surviving particles and samples once per tick", "[particle][rope]") {
    RopeTrailHistory history (3, 4);
    history.born (0, { 1.0f, 2.0f, 3.0f });
    history.born (1, { 10.0f, 20.0f, 30.0f });
    history.born (2, { 100.0f, 200.0f, 300.0f });
    REQUIRE (ropeTrailUVLength (history.counts[2], 2.0f) == 0.5f);
    history.sample (0, { 2.0f, 3.0f, 4.0f });
    history.sample (2, { 101.0f, 201.0f, 301.0f });

    // Particle one expires. Compaction must carry particle two's own row,
    // never connect its history to the first particle or dead middle one.
    history.compact (1, 2);
    REQUIRE (history.counts[0] == 2);
    REQUIRE (history.counts[1] == 2);
    REQUIRE (ropeTrailUVLength (history.counts[1], 2.0f) == 1.0f);
    REQUIRE (history.at (0, 0) == glm::vec3 (2.0f, 3.0f, 4.0f));
    REQUIRE (history.at (0, 1) == glm::vec3 (1.0f, 2.0f, 3.0f));
    REQUIRE (history.at (1, 0) == glm::vec3 (101.0f, 201.0f, 301.0f));
    REQUIRE (history.at (1, 1) == glm::vec3 (100.0f, 200.0f, 300.0f));
    REQUIRE (history.at (1, 2) == glm::vec3 (100.0f, 200.0f, 300.0f));

    // The native countdown has one expiry branch, not a catch-up loop.
    float countdown = 0.02f;
    const bool sampled = history.advance (0.25f, 0.05f, countdown, 2,
        [] (uint32_t i) { return i == 0 ? glm::vec3 (3.0f, 4.0f, 5.0f)
                                           : glm::vec3 (102.0f, 202.0f, 302.0f); });
    REQUIRE (sampled);
    REQUIRE (countdown == 0.05f);
    REQUIRE (history.counts[1] == 3);
    REQUIRE (ropeTrailUVLength (history.counts[1], 2.0f) == 1.5f);
    REQUIRE (history.at (1, 0) == glm::vec3 (102.0f, 202.0f, 302.0f));
    REQUIRE (ropeTrailUVLength (history.counts[1], 0.0f) == 3.0f);
    REQUIRE (history.at (1, 1) == glm::vec3 (101.0f, 201.0f, 301.0f));
    REQUIRE_FALSE (history.advance (0.01f, 0.05f, countdown, 2,
        [] (uint32_t) { return glm::vec3 (999.0f); }));
    REQUIRE (history.at (1, 0) == glm::vec3 (102.0f, 202.0f, 302.0f));
}

TEST_CASE ("Rope trail allocation covers independent history rows", "[particle][rope][boundary]") {
    const auto geometry = ropeTrailGeometry (3, 4, 2.0f, 26);
    REQUIRE (geometry.has_value ());
    REQUIRE (geometry->subdivision == 3);
    REQUIRE (geometry->indexCount == 3 * 4 * 3 * 6);
    REQUIRE (geometry->floatCount == 3 * 4 * 3 * 4 * 26);
    REQUIRE (ropeTrailGeometry (1, 4, -5.0f, 26)->subdivision == 1);
    REQUIRE (ropeTrailGeometry (1, 4, 99.0f, 26)->subdivision == 33);
    REQUIRE_FALSE (ropeTrailGeometry (1, 1, 0.0f, 26).has_value ());
    REQUIRE_FALSE (ropeTrailGeometry (MAX_PARTICLE_CAPACITY, 32, 32.0f, 26).has_value ());
}

TEST_CASE ("Rope curve follows native smoothstep cubic controls", "[particle][rope]") {
    const glm::vec3 previous (-2.0f, 5.0f, 0.0f);
    const glm::vec3 start (0.0f, 0.0f, 0.0f);
    const glm::vec3 end (4.0f, 0.0f, 0.0f);
    const glm::vec3 next (6.0f, 8.0f, 0.0f);
    REQUIRE (ropeBezierPosition (previous, start, end, next, 0.0f) == start);
    REQUIRE (ropeBezierPosition (previous, start, end, next, 1.0f) == end);
    const glm::vec3 middle = ropeBezierPosition (previous, start, end, next, 0.5f);
    REQUIRE (middle.x == Catch::Approx (2.0f));
    REQUIRE (middle.y == Catch::Approx (-0.73125f));
    const glm::vec3 quarter = ropeBezierPosition (previous, start, end, next, 0.25f);
    REQUIRE (quarter.x == Catch::Approx (0.50717163f));
    REQUIRE (quarter.y == Catch::Approx (-0.32444f));
}

TEST_CASE ("Rope interiors interpolate sized endpoint rights without renormalizing",
           "[particle][rope]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    const glm::vec3 eye (0.0f, 0.0f, -1.0f);
    const auto start = ropeSizedRight (eye, {3.0f, 1.0f, 0.0f}, 1.0f);
    const auto end = ropeSizedRight (eye, {1.0f, 3.0f, 0.0f}, 1.0f);
    const auto quarter = ropeInterpolatedRight (start, end, .25f);
    const auto middle = ropeInterpolatedRight (start, end, .5f);
    REQUIRE (quarter.x == Catch::Approx (.41504894f).margin (.000001f));
    REQUIRE (quarter.y == Catch::Approx (-.84986212f).margin (.000001f));
    REQUIRE (glm::length (middle) == Catch::Approx (.89442719f).margin (.000001f));
    const auto reversedEnd = ropeSizedRight (eye, {-1.0f, 1.0f, 0.0f}, 1.0f);
    const auto narrow = ropeInterpolatedRight (start, reversedEnd, .5f);
    REQUIRE (glm::length (narrow) == Catch::Approx (.5257311f).margin (.000001f));
    REQUIRE (ropeSizedRight (eye, glm::vec3 (0.0f), 1.0f) == glm::vec3 (0.0f));
}

TEST_CASE ("Rope CPU right retains native UV edges in reflected simulation space",
           "[particle][rope]") {
    const auto reflectY = [] (glm::vec3 value) {
        return glm::vec3 (value.x, -value.y, value.z);
    };
    const glm::vec3 eye (0.0f, 0.0f, -1.0f);
    const glm::vec3 tangent (3.0f, 1.0f, 0.0f);
    const glm::vec3 position (5.0f, 8.0f, 0.0f);
    const glm::vec3 nativeRight = ropeSizedRight (eye, tangent, 2.0f);
    const glm::vec3 linuxRight = ropeReflectedSizedRight (
        reflectY (eye), reflectY (tangent), 2.0f);
    REQUIRE (glm::length (linuxRight - reflectY (nativeRight)) < 0.000001f);
    // Shipped genericropeparticle.geom emits U=0 at position-right, then U=1
    // at position+right. The relation must hold for both world and local
    // particle model bases, including nonuniform transformed local geometry.
    const glm::mat3 reflect (glm::vec3 (1, 0, 0), glm::vec3 (0, -1, 0),
                             glm::vec3 (0, 0, 1));
    glm::mat3 nativeModel (1.0f);
    nativeModel[0][0] = 2.0f;
    nativeModel[1][1] = 0.5f;
    for (const glm::mat3 model : {glm::mat3 (1.0f), nativeModel}) {
        const glm::mat3 linuxModel = reflect * model * reflect;
        for (const float edge : {-1.0f, 1.0f}) {
            const glm::vec3 nativeVertex = model * (position + edge * nativeRight);
            const glm::vec3 linuxVertex = linuxModel * (reflectY (position) + edge * linuxRight);
            REQUIRE (glm::length (linuxVertex - reflectY (nativeVertex)) < 0.000002f);
        }
    }
}

TEST_CASE ("Rope trail CPU subdivision preserves native segment UV phase", "[particle][rope]") {
    const auto head = ropeTrailSegmentUV (0, 3.0f, 3.5f, 0.25f, false);
    const auto next = ropeTrailSegmentUV (1, 3.0f, 3.5f, 0.25f, false);
    REQUIRE (head.start == 0.0f);
    REQUIRE (head.end == Catch::Approx (1.0f / 9.0f));
    REQUIRE (next.start == Catch::Approx (head.end));
    REQUIRE (ropeTrailSubsegmentUV (head, 0.0f) == head.start);
    REQUIRE (ropeTrailSubsegmentUV (head, 0.5f) == Catch::Approx (head.end * 0.5f));
    REQUIRE (ropeTrailSubsegmentUV (head, 1.0f) == head.end);

    const auto scrollingHead = ropeTrailSegmentUV (0, 3.0f, 3.5f, 0.25f, true);
    const auto scrollingNext = ropeTrailSegmentUV (1, 3.0f, 3.5f, 0.25f, true);
    REQUIRE (scrollingHead.start == Catch::Approx (0.785714286f));
    REQUIRE (scrollingHead.end == Catch::Approx (scrollingNext.start));
    REQUIRE (scrollingHead.end == Catch::Approx (0.714285714f));
    REQUIRE (ropeTrailSegmentUV (0, 1.0f, 3.5f, 0.0f, false).start == 0.0f);
    REQUIRE (ropeTrailScrollMaxCount (4, 2.0f) == 1.5f);
    const auto scaledHead = ropeTrailSegmentUV (0, 3.0f,
        ropeTrailScrollMaxCount (4, 2.0f), 0.25f, true);
    REQUIRE (scaledHead.start == Catch::Approx (0.5f));
    REQUIRE (scaledHead.end == Catch::Approx (1.0f / 3.0f));
    REQUIRE (ropeTrailFade (0, 4, 0.25f, false) == 0.0f);
    REQUIRE (ropeTrailFade (1, 4, 0.25f, false)
             == Catch::Approx (ropeTrailFade (0, 4, 0.25f, true)));
}

TEST_CASE ("Rope CPU shader override carries original-segment UV and rights", "[particle][rope]") {
    const std::string native =
        "void main() { v_TexCoord.y = mix(uvMinimum, uvMinimum + uvDelta, uvs.y); "
        "vec3 eyeDirection = mul(g_OrientationForward, CAST3X3(g_ModelMatrixInverse)); "
        "vec3 trailRightStart = cross(eyeDirection, trailDelta + CPStart); "
        "vec3 trailRightEnd = cross(eyeDirection, trailDelta - CPEnd); "
        "trailRightStart = normalize(trailRightStart) * sizeStart; "
        "trailRightEnd = normalize(trailRightEnd) * sizeEnd; "
        "position += right * uvs.x * 2.0 - 1.0; "
        "colorStart.a *= trailFadeIn; colorEnd.a *= trailFadeOut; "
        "sizeStart.w *= trailFadeIn; sizeEnd.w *= trailFadeOut; }";
    const auto cpu = WallpaperEngine::Render::Shaders::particleRopeCpuUVSource (native);
    REQUIRE (cpu.find ("attribute vec2 a_CpuRopeUV;") != std::string::npos);
    REQUIRE (cpu.find ("attribute vec3 a_CpuRopeRightStart;") != std::string::npos);
    REQUIRE (cpu.find ("attribute vec3 a_CpuRopeRightEnd;") != std::string::npos);
    REQUIRE (cpu.find ("attribute vec3 a_CpuRopeEyeDirection;") != std::string::npos);
    REQUIRE (cpu.find ("mix(a_CpuRopeUV.x, a_CpuRopeUV.y, uvs.y)") != std::string::npos);
    REQUIRE (cpu.find ("vec3 trailRightStart = a_CpuRopeRightStart;") != std::string::npos);
    REQUIRE (cpu.find ("vec3 trailRightEnd = a_CpuRopeRightEnd;") != std::string::npos);
    REQUIRE (cpu.find ("vec3 eyeDirection = a_CpuRopeEyeDirection;") != std::string::npos);
    REQUIRE (cpu.find ("position += right * (uvs.x * 2.0 - 1.0);") != std::string::npos);
    REQUIRE (cpu.find ("mix(uvMinimum, uvMinimum + uvDelta, uvs.y)") == std::string::npos);
    REQUIRE (cpu.find ("colorStart.a *= trailFadeIn") == std::string::npos);
    REQUIRE (cpu.find ("sizeStart.w *= trailFadeIn") == std::string::npos);
    REQUIRE_THROWS (WallpaperEngine::Render::Shaders::particleRopeCpuUVSource ("void main() {}"));
}

TEST_CASE ("Sequence initializer rejects zero and invalid dynamic counts before modulo", "[particle][parser]") {
    using WallpaperEngine::Data::Parsers::ObjectParser;
    using WallpaperEngine::Data::Model::MapSequenceAroundControlPointInitializer;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::JSON::JSON;
    Project project {};
    const auto data = JSON::parse (R"({"id":1,"name":"sequence","particle":{
        "initializer":[{"name":"mapsequencearoundcontrolpoint","count":0}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->initializers.size () == 1);
    const auto* initializer = particle->initializers[0]->as<MapSequenceAroundControlPointInitializer> ();
    REQUIRE (initializer != nullptr);
    int index = 0;
    REQUIRE_FALSE (nextSequenceAngle (initializer->count->value->getFloat (), index).has_value ());
    REQUIRE (index == 0);
    REQUIRE_FALSE (nextSequenceAngle (-1.0f, index).has_value ());
    REQUIRE_FALSE (nextSequenceAngle (std::numeric_limits<float>::infinity (), index).has_value ());
    REQUIRE_FALSE (nextSequenceAngle (std::numeric_limits<float>::max (), index).has_value ());
    REQUIRE (nextSequenceAngle (3.0f, index) == 0.0f);
    REQUIRE (index == 1);
    REQUIRE (nextSequenceAngle (3.0f, index).has_value ());
    REQUIRE (index == 2);
    REQUIRE (nextSequenceAngle (3.0f, index).has_value ());
    REQUIRE (index == 0);
}

TEST_CASE ("Native around-control-point phase wraps, mirrors, and preserves orbit geometry", "[particle][script]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    float phase = 0.0f;
    float step = 0.5f;
    const glm::vec2 bounds (0.25f, 0.75f);
    REQUIRE (sequenceAngle (phase, step, bounds, false) == Catch::Approx (glm::half_pi<float> ()));
    REQUIRE (phase == Catch::Approx (0.5f));
    REQUIRE (sequenceAngle (phase, step, bounds, false) == Catch::Approx (glm::pi<float> ()));
    REQUIRE (phase == Catch::Approx (1.0f));
    sequenceAngle (phase, step, bounds, false);
    REQUIRE (phase == Catch::Approx (0.5f));
    phase = 1.0f;
    step = 0.5f;
    sequenceAngle (phase, step, bounds, true);
    REQUIRE (phase == Catch::Approx (0.5f));
    REQUIRE (step == Catch::Approx (-0.5f));
    sequenceAngle (phase, step, bounds, true);
    REQUIRE (phase == Catch::Approx (0.0f));
    sequenceAngle (phase, step, bounds, true);
    REQUIRE (phase == Catch::Approx (0.5f));
    REQUIRE (step == Catch::Approx (0.5f));
    phase = 0.0f;
    step = 2.0f;
    sequenceAngle (phase, step, bounds, true);
    REQUIRE (phase == Catch::Approx (0.0f)); // one reflection, even for overshoot
    REQUIRE (step == Catch::Approx (-2.0f));

    const auto orbit = sequenceOrbit (glm::vec3 (12.0f, 20.0f, 33.0f),
                                      glm::vec3 (10.0f, 20.0f, 30.0f),
                                      glm::vec3 (0.0f, 0.0f, 1.0f), 0.0f);
    REQUIRE (orbit.position.x == Catch::Approx (10.0f));
    REQUIRE (orbit.position.y == Catch::Approx (18.0f));
    REQUIRE (orbit.position.z == Catch::Approx (33.0f));
    REQUIRE (orbit.radial.y == Catch::Approx (-1.0f));
    REQUIRE (orbit.tangent.x == Catch::Approx (1.0f));
    const glm::vec3 existingVelocity (7.0f, 8.0f, 9.0f);
    const glm::vec3 updatedVelocity = existingVelocity + orbit.tangent * 2.0f
        + orbit.radial * 3.0f + orbit.axis * 4.0f;
    REQUIRE (updatedVelocity.x == Catch::Approx (9.0f));
    REQUIRE (updatedVelocity.y == Catch::Approx (5.0f));
    REQUIRE (updatedVelocity.z == Catch::Approx (13.0f));

    const auto negativeZ = sequenceBasis (glm::vec3 (0.0f, 0.0f, -1.0f));
    REQUIRE (negativeZ.c.y == Catch::Approx (-1.0f));
    glm::mat3 rolled (glm::vec3 (0.0f, 1.0f, 0.0f),
                      glm::vec3 (-1.0f, 0.0f, 0.0f),
                      glm::vec3 (0.0f, 0.0f, 1.0f));
    auto rolledBasis = sequenceBasis (glm::vec3 (0.0f, 0.0f, 1.0f));
    rolledBasis.axis = rolled * rolledBasis.axis;
    rolledBasis.b = rolled * rolledBasis.b;
    rolledBasis.c = rolled * rolledBasis.c;
    const auto rolledOrbit = sequenceOrbitInBasis (glm::vec3 (10.0f, 0.0f, 0.0f),
                                                   glm::vec3 (0.0f), rolledBasis, 0.0f);
    REQUIRE (rolledOrbit.position.x == Catch::Approx (10.0f));
    REQUIRE (rolledOrbit.position.y == Catch::Approx (0.0f));
    REQUIRE (rolledOrbit.tangent.y == Catch::Approx (1.0f));
    // Native 1401c19e0's +Z frame has c=+Y. A local CP rotation +90° maps
    // that to authored -X; F*M*F acting on the reflected frame must keep -X.
    const auto cpBasis = localControlPointBasis (
        glm::vec3 (0.0f, 0.0f, glm::half_pi<float> ()));
    const auto nativePhaseZero = cpBasis * sequenceBasis (glm::vec3 (0, 0, 1)).c;
    REQUIRE (nativePhaseZero.x == Catch::Approx (-1.0f));
    REQUIRE (nativePhaseZero.y == Catch::Approx (0.0f).margin (0.000001f));
    // The five authored phases of 2880877046 must retain their angular order
    // after reflection, beginning at visual +Y / simulation -Y.
    for (int i = 0; i < 5; ++i) {
        const float angle = static_cast<float> (i) * glm::two_pi<float> () / 5.0f;
        const auto phaseOrbit = sequenceOrbit (
            glm::vec3 (1.0f, 0.0f, 0.0f), glm::vec3 (0.0f),
            glm::vec3 (0.0f, 0.0f, 1.0f), angle);
        REQUIRE (phaseOrbit.position.x == Catch::Approx (std::sin (angle)).margin (0.000001f));
        REQUIRE (phaseOrbit.position.y == Catch::Approx (-std::cos (angle)).margin (0.000001f));
    }
    std::vector<std::pair<float, float>> sampledBounds;
    const glm::vec3 sampled = sequenceSpeed (
        glm::vec3 (1.0f, 10.0f, 100.0f), glm::vec3 (3.0f, 30.0f, 300.0f),
        [&sampledBounds] (float low, float high) {
            sampledBounds.emplace_back (low, high);
            return low;
        });
    const std::vector<std::pair<float, float>> expectedBounds {
        {100.0f, 300.0f}, {1.0f, 3.0f}, {10.0f, 30.0f}};
    REQUIRE (sampledBounds == expectedBounds);
    REQUIRE (sampled == glm::vec3 (1.0f, 10.0f, 100.0f));
}

TEST_CASE ("Box and sphere emitter birth speed obeys instance speed binding", "[particle][emitter]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    // 2880877046 object 27 authors speed 0..20 and instance speed 0.35.
    const glm::vec2 inherited = emitterSpeedBounds (0.0f, 20.0f, 0.35f, 0u);
    REQUIRE (inherited.x == Catch::Approx (0.0f));
    REQUIRE (inherited.y == Catch::Approx (7.0f));
    const glm::vec2 suppressed = emitterSpeedBounds (0.0f, 20.0f, 0.35f, 0x10u);
    REQUIRE (suppressed.x == Catch::Approx (0.0f));
    REQUIRE (suppressed.y == Catch::Approx (20.0f));
    const glm::vec2 nonzeroMin = emitterSpeedBounds (2.0f, 6.0f, 0.5f, 0u);
    REQUIRE (nonzeroMin.x == Catch::Approx (1.0f));
    REQUIRE (nonzeroMin.y == Catch::Approx (3.0f));
}

TEST_CASE ("Normal periodic birth reloads instant count and borrows rate credit", "[particle][script]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    EmitterScheduleConfig config {};
    config.periodic = true;
    config.instantaneous = 2;
    config.rate = 0.5f;
    config.minActiveDuration = config.maxActiveDuration = 1.0f;
    auto state = initialState (config);
    bool restarted = false;
    const auto fixed = [] (float low, float) { return low; };
    REQUIRE (advanceEmitter (config, state, 0.1f, 10, fixed, &restarted) == 2);
    REQUIRE (restarted);
    REQUIRE (state.fractional == Catch::Approx (-1.95f));
    state.periodTimer = 0.0f;
    state.instantaneousRemaining = 0;
    REQUIRE (advanceEmitter (config, state, 0.1f, 10, fixed, &restarted) == 2);
    REQUIRE (restarted);
    REQUIRE (state.fractional == Catch::Approx (-3.9f));
    auto clipped = initialState (config);
    REQUIRE (advanceEmitter (config, clipped, 0.1f, 1, fixed) == 1);
    REQUIRE (clipped.fractional == Catch::Approx (-1.95f));
    float phase = 0.75f, signedStep = -2.0f;
    resetSequencePhase (phase, signedStep);
    REQUIRE (phase == Catch::Approx (0.0f));
    REQUIRE (signedStep == Catch::Approx (-2.0f));
    sequenceAngle (phase, signedStep, glm::vec2 (0.0f, 1.0f), true);
    REQUIRE (phase == Catch::Approx (2.0f));
    REQUIRE (signedStep == Catch::Approx (2.0f));
}

TEST_CASE ("Native box and sphere schedule traces share the same clock rules", "[particle][capture]") {
    auto file = fixture ("scheduling.tsv");
    std::string line;
    std::getline (file, line); // header
    std::map<std::string, EmitterScheduleState> states;
    std::map<std::string, size_t> counts;
    while (std::getline (file, line)) {
        auto row = fields (line);
        REQUIRE (row.size () == 10);
        const std::string& name = row[0];
        const bool periodic = name.find ("_periodic") != std::string::npos;
        const bool full = name.find ("_pool_full") != std::string::npos;
        const uint32_t capacity = full ? 1 : 4;
        const EmitterScheduleConfig config {
            .rate = 7.0f,
            .delay = 0.3f,
            .duration = periodic ? 0.0f : 1.2f,
            .periodic = periodic,
            .minActiveDuration = 0.4f,
            .maxActiveDuration = 0.4f,
            .minOffDelay = 0.2f,
            .maxOffDelay = 0.2f,
            .maxPerPeriod = periodic ? 2u : 0u,
        };
        if (!states.contains (name)) states[name] = initialState (config);
        auto& state = states[name];
        const uint32_t activeBefore = std::stoul (row[2]);
        const uint32_t activeAfter = std::stoul (row[3]);
        REQUIRE (activeBefore <= capacity);
        REQUIRE (activeAfter >= activeBefore);
        const uint32_t emitted = advanceEmitter (
            config, state, number (row[1]), capacity - activeBefore,
            [] (float min, float max) { REQUIRE (min == max); return min; });
        REQUIRE (activeBefore + emitted == activeAfter);
        REQUIRE (std::abs (state.durationRemaining - number (row[4])) < 0.000002f);
        REQUIRE (std::abs (state.delayRemaining - number (row[5])) < 0.000002f);
        REQUIRE (std::abs (state.fractional - number (row[6])) < 0.000002f);
        REQUIRE (std::abs (state.periodTimer - number (row[7])) < 0.000002f);
        REQUIRE (state.emittedThisPeriod == std::stoul (row[8]));
        REQUIRE ((state.expired ? 1u : 0u) == (std::stoul (row[9]) >> 31));
        ++counts[name];
    }
    REQUIRE (counts.size () == 6);
    for (const auto& [name, count] : counts) REQUIRE (count == 96);
}

TEST_CASE ("Native turbulence simplex primitive matches the PE-table scalar oracle",
           "[particle][turbulence]") {
    // Independent PE-table reference: tools/reversing/particle_simplex3_reference.py.
    const std::array<std::pair<glm::vec3, float>, 5> values {{
        {{0.0f, 0.0f, 0.0f}, 0.0f},
        {{0.17f, -0.31f, 0.53f}, 0.5633527637f},
        {{-2.125f, 1.375f, 0.25f}, 0.1856310666f},
        {{3.999f, -1.001f, 2.499f}, 0.0542450547f},
        {{0.49999f, 0.50001f, -0.33337f}, 0.4671141803f},
    }};
    for (const auto& [position, expected] : values) {
        const auto sampled = WallpaperEngine::Render::Utils::nativeSimplex3 (position);
        REQUIRE (sampled.has_value ());
        REQUIRE (*sampled == Catch::Approx (expected).margin (0.000002f));
    }
    REQUIRE_FALSE (WallpaperEngine::Render::Utils::nativeSimplex3 (
        {std::numeric_limits<float>::infinity (), 0.0f, 0.0f}).has_value ());

    // Two complete four-lane pairs captured at the actual 2.8.42 opcode
    // 0x0e noise call. The fourth-corner hash differs from standard simplex.
    const std::array<std::pair<glm::vec3, float>, 8> nativeLanes {{
        {{0.9039207101f, 0.8677521348f, 0.0f}, -0.4097816646f},
        {{0.8908262253f, 0.8415972590f, 0.0f}, -0.3230914772f},
        {{0.8794067502f, 0.8225078583f, 0.0f}, -0.2566938400f},
        {{0.8073109984f, 0.7588959336f, 0.0f}, -0.0610876605f},
        {{0.8526679277f, 0.7809768915f, 0.0f}, -0.1193731055f},
        {{0.8346726894f, 0.7574375272f, 0.0f}, -0.0516832843f},
        {{0.8205256462f, 0.7402571440f, 0.0f}, -0.0101621822f},
        {{0.7480441928f, 0.6830062270f, 0.0f}, 0.0393137932f},
    }};
    for (const auto& [position, expected] : nativeLanes) {
        const auto sampled = WallpaperEngine::Render::Utils::nativeSimplex3 (position);
        REQUIRE (sampled.has_value ());
        REQUIRE (*sampled == Catch::Approx (expected).margin (0.000002f));
    }

    // The interpreter's seven-entry jump table rotates the same coordinate
    // triplet for each enabled velocity axis; its mask-zero path skips samples.
    const glm::vec3 axisValues (0.5633527637f, 0.3682731092f, -0.2984927297f);
    for (uint32_t mask = 0; mask < 8; ++mask) {
        const auto sampled = WallpaperEngine::Render::Utils::nativeTurbulenceNoise (
            {0.17f, -0.31f, 0.53f}, mask);
        REQUIRE (sampled.has_value ());
        for (int axis = 0; axis < 3; ++axis)
            REQUIRE ((*sampled)[axis] == Catch::Approx ((mask & (1u << axis))
                ? axisValues[axis] : 0.0f).margin (0.000002f));
    }
}

TEST_CASE ("Native turbulence lane uses shared birth random and the damped clock",
           "[particle][turbulence]") {
    using WallpaperEngine::Render::Utils::nativeTurbulenceVelocityDelta;
    // Independent PE-table arithmetic in particle_simplex3_reference.py.
    // The 0.125 clock is deliberately unequal to the 0.5 integration control.
    const glm::vec3 position (0.17f, -0.31f, 0.53f);
    const glm::vec3 mask (1.0f, -0.5f, 0.25f);
    const auto delta = nativeTurbulenceVelocityDelta (
        position, 0.375f, 0.2f, 0.8f, -4.0f, 12.0f, 0.75f, 0.4f, 2.25f,
        mask, 0.125f, 0.8f);
    REQUIRE (delta.has_value ());
    REQUIRE (delta->x == Catch::Approx (0.0056898450f).margin (0.000002f));
    REQUIRE (delta->y == Catch::Approx (-0.0138712777f).margin (0.000002f));
    REQUIRE (delta->z == Catch::Approx (0.0006463223f).margin (0.000002f));
    const auto fourfoldClock = nativeTurbulenceVelocityDelta (
        position, 0.375f, 0.2f, 0.8f, -4.0f, 12.0f, 0.75f, 0.4f, 2.25f,
        mask, 0.5f, 0.8f);
    REQUIRE (fourfoldClock.has_value ());
    REQUIRE (fourfoldClock->x == Catch::Approx (0.0227593798f).margin (0.000002f));
    REQUIRE (fourfoldClock->y == Catch::Approx (-0.0554851107f).margin (0.000002f));
    REQUIRE (fourfoldClock->z == Catch::Approx (0.0025852893f).margin (0.000002f));
    const auto negativeSpeed = nativeTurbulenceVelocityDelta (
        position, 0.375f, 0.2f, 0.8f, -8.0f, 4.0f, 0.75f, 0.4f, 2.25f,
        mask, 0.125f, 0.8f);
    REQUIRE (negativeSpeed.has_value ());
    REQUIRE (negativeSpeed->x == Catch::Approx (-0.0739679784f).margin (0.000002f));
    REQUIRE (negativeSpeed->y == Catch::Approx (0.1803266108f).margin (0.000002f));
    REQUIRE (negativeSpeed->z == Catch::Approx (-0.0084021902f).margin (0.000002f));
    const auto zeroMask = nativeTurbulenceVelocityDelta (
        {std::numeric_limits<float>::infinity (), 0.0f, 0.0f}, 0.375f,
        0.2f, 0.8f, -4.0f, 12.0f, 0.75f, 0.4f, 2.25f, {}, 0.125f, 0.8f);
    REQUIRE (zeroMask.has_value ());
    REQUIRE (*zeroMask == glm::vec3 (0.0f));
}

TEST_CASE ("Native particle scene clock retains double precision and wraps on float threshold",
           "[particle][scene-clock]") {
    using WallpaperEngine::Render::Wallpapers::advanceParticleSceneClock;
    double accumulator = 0.0;
    REQUIRE (advanceParticleSceneClock (accumulator, 0.1f)
             == Catch::Approx (0.1f));
    REQUIRE (advanceParticleSceneClock (accumulator, 0.1f)
             == Catch::Approx (static_cast<float> (double (0.1f) * 2.0)));
    REQUIRE (accumulator == Catch::Approx (double (0.1f) * 2.0));
    accumulator = 432000.0;
    REQUIRE (advanceParticleSceneClock (accumulator, 0.0f) == 432000.0f);
    REQUIRE (accumulator == 432000.0);
    // The comparison follows the published float, not the double sum.
    accumulator = 432000.0 + 0.001;
    REQUIRE (advanceParticleSceneClock (accumulator, 0.0f) == 432000.0f);
    REQUIRE (accumulator > 432000.0);
    REQUIRE (advanceParticleSceneClock (accumulator, 0.125f) == 0.0f);
    REQUIRE (accumulator == 0.0);
}

TEST_CASE ("Native turbulent birth uses one-dimensional gradient and phase-before-speed draws",
           "[particle][turbulent-birth]") {
    using WallpaperEngine::Render::Utils::nativeParticleGradientNoise;
    using WallpaperEngine::Render::Utils::nativeTurbulentBirthVelocity;
    // Independently derived from the matching PE table by
    // tools/reversing/particle_gradient1_reference.py.
    for (const auto [coordinate, expected] : std::array<std::pair<float, float>, 5> {{
             {0.125f, 0.3698422015f}, {-0.25f, -0.5559886098f},
             {1.5f, 0.1874707043f}, {10.125f, 0.0401031300f},
             {255.5f, -0.1874707043f}}}) {
        const auto value = nativeParticleGradientNoise (coordinate);
        REQUIRE (value.has_value ());
        REQUIRE (*value == Catch::Approx (expected).margin (0.000002f));
    }
    const auto vapor = nativeTurbulentBirthVelocity (
        .22103399f, .5f, 0.0f, 0.0f, 250.0f, 0.0f, 1.25f, .5f,
        .1f, 0.0f, {0, 1, 0}, {0, 0, 1});
    REQUIRE (vapor.has_value ());
    REQUIRE (vapor->x == Catch::Approx (-15.034916f).margin (.0002f));
    REQUIRE (vapor->y == Catch::Approx (249.54750f).margin (.0002f));
    REQUIRE (vapor->z == Catch::Approx (0.0f).margin (.000001f));
    const auto oblique = nativeTurbulentBirthVelocity (
        .375f, .625f, -.2f, .9f, -100.0f, 400.0f, 1.25f,
        .45f, .35f, .2f, {1, 2, 3}, {2, -1, 4});
    REQUIRE (oblique.has_value ());
    REQUIRE (oblique->x == Catch::Approx (7.6423755f).margin (.002f));
    REQUIRE (oblique->y == Catch::Approx (241.53067f).margin (.002f));
    REQUIRE (oblique->z == Catch::Approx (506.56149f).margin (.002f));
    REQUIRE_FALSE (nativeParticleGradientNoise (std::numeric_limits<float>::infinity ()).has_value ());
}

TEST_CASE ("Birth noise kernels match captured native instruction bit vectors",
           "[particle][birth][remap][noise]") {
    using namespace WallpaperEngine::Render::Utils;
    // Original 2.8.42 instructions 14027b090/14027b4b0, saved native leaf
    // outputs under masked IEEE/RNE (25 gradient inputs and FBM scale 2).
    const std::array<std::pair<uint32_t, uint32_t>, 25> gradientVectors {{
        { 0x00000000u, 0x00000000u },
        { 0x80000000u, 0x80000000u },
        { 0x3e000000u, 0x3ebd5bf5u },
        { 0xbe800000u, 0xbf0e5545u },
        { 0x3fc00000u, 0x3e3ff852u },
        { 0x41220000u, 0x3d24432eu },
        { 0x437f8000u, 0xbe3ff852u },
        { 0x43808000u, 0x00000000u },
        { 0xbf800000u, 0x00000000u },
        { 0xbf7fffffu, 0x33fccccdu },
        { 0xbf800001u, 0xb47ccccdu },
        { 0x3f7fffffu, 0xb2ca3d71u },
        { 0x3f800001u, 0x334a3d71u },
        { 0x49742400u, 0x80000000u },
        { 0x4effffffu, 0x00000000u },
        { 0x4f000000u, 0x7f800000u },
        { 0xcf000000u, 0x00000000u },
        { 0xcf000001u, 0xff800000u },
        { 0x7f7fffffu, 0x7f800000u },
        { 0xff7fffffu, 0xff800000u },
        { 0x7fc00001u, 0x7fc00001u },
        { 0xffc00001u, 0xffc00001u },
        { 0x7f800000u, 0x7f800000u },
        { 0xff800000u, 0xff800000u },
        { 0x00000001u, 0x00000003u },
    }};
    for (const auto [input, expected] : gradientVectors) {
        const float value = nativeBirthParticleGradientNoise (std::bit_cast<float> (input));
        REQUIRE (std::bit_cast<uint32_t> (value) == expected);
    }
    const std::array<std::pair<int, uint32_t>, 6> fbmVectors {{
        { 0, 0xffc00000u }, { 1, 0x3f197274u }, { 2, 0x3f0ba025u },
        { 3, 0x3eef5badu }, { 5, 0x3ed831c5u }, { 32, 0x3ed17037u }
    }};
    for (const auto [count, expected] : fbmVectors) {
        const float value = nativeBirthParticleFBMNoise (0.125f, 2.0f, count);
        if (count == 0) REQUIRE (std::isnan (value));
        else REQUIRE (std::bit_cast<uint32_t> (value) == expected);
    }
    for (const float input : { std::numeric_limits<float>::quiet_NaN (),
                              std::numeric_limits<float>::infinity (),
                              -std::numeric_limits<float>::infinity () }) {
        REQUIRE (std::isnan (nativeBirthParticleFBMNoise (input, 2.0f, 0)));
        const float value = nativeBirthParticleFBMNoise (input, 2.0f, 3);
        if (std::isnan (input)) REQUIRE (std::isnan (value));
        else REQUIRE (value == input);
        REQUIRE_FALSE (nativeParticleGradientNoise (input).has_value ());
    }
    REQUIRE_FALSE (nativeParticleGradientNoise (1.0e6f).has_value ());
    REQUIRE (std::bit_cast<uint32_t> (*nativeParticleGradientNoise (0.125f)) == 0x3ebd5bf5u);
    // Existing seeded runtime kernels retain their accepted values/count policy.
    REQUIRE (remapSimplexNoise (0x3e800000u, 0.3f) == Catch::Approx (0.6666548f));
    REQUIRE (remapFBMNoise (0x3e800000u, 0.3f, 0) == remapFBMNoise (0x3e800000u, 0.3f, 1));
}

TEST_CASE ("Production birth noise uses native kernels for scalar and vector inputs",
           "[particle][birth][remap][noise]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    for (const auto transform : { "simplexnoise", "fbmnoise" }) {
        for (const int count : { 0, 1, 2, 3, 32 }) {
            JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
                {"name":"remapinitialvalue","input":"size","output":"opacity",
                 "operation":"remap","flags":0,"transforminputscale":2},
                {"name":"remapinitialvalue","input":"position","output":"velocity",
                 "operation":"remap","flags":0,"transforminputscale":2}
            ]}})");
            for (auto& initializer : data["particle"]["initializer"]) {
                initializer["transformfunction"] = transform;
                if (std::string (transform) == "fbmnoise") initializer["transformoctaves"] = count;
            }
            const auto object = ObjectParser::parse (data, project);
            const auto& model = *object->as<Particle> ();
            REQUIRE (model.initializers.size () == 2);
            const auto& scalarModel = *model.initializers[0]->as<RemapInitialValueInitializer> ()
                ->remap->as<ScalarRemapValueOperator> ();
            const auto& vectorModel = *model.initializers[1]->as<RemapInitialValueInitializer> ()
                ->remap->as<VectorRemapValueOperator> ();
            REQUIRE (scalarModel.transformOctaves == (std::string (transform) == "fbmnoise" ? count : 3));
            const auto scalar = createScalarRemapOperator (scalarModel, true, false, false, false);
            const auto vector = createVectorRemapOperator (vectorModel, true, false, false, false);
            std::vector<ParticleInstance> particles (2);
            std::vector<ControlPointData> cps (8);
            for (auto& p : particles) {
                p.alive = true; p.lifetime = 10.0f; p.size = 0.125f;
                p.position = toSimulationVector (glm::vec3 (0.125f));
            }
            particles[0].oscillatorRandom = 0.25f;
            particles[1].oscillatorRandom = std::bit_cast<float> (0x7fc00001u);
            scalar (particles, 2, cps, 0.0f, MovementTime {});
            vector (particles, 2, cps, 0.0f, MovementTime {});
            // Constants independently captured from native leaf instructions.
            const uint32_t raw = std::string (transform) == "simplexnoise" || count == 1 ? 0x3f197274u
                : count == 2 ? 0x3f0ba025u : count == 3 ? 0x3eef5badu : 0x3ed17037u;
            const float expected = std::bit_cast<float> (raw) * 0.5f + 0.5f;
            for (const auto& p : particles) {
                if (std::string (transform) == "fbmnoise" && count == 0) {
                    REQUIRE (std::isnan (p.alpha));
                    for (int axis = 0; axis < 3; ++axis) REQUIRE (std::isnan (p.velocity[axis]));
                } else {
                    REQUIRE (std::bit_cast<uint32_t> (p.alpha) == std::bit_cast<uint32_t> (expected));
                    REQUIRE (toAuthoredVector (p.velocity) == glm::vec3 (expected));
                }
            }
            // Actual runtime closures still evaluate the seeded fallback kernel.
            const auto runtime = createScalarRemapOperator (scalarModel, false, false, false, false);
            runtime (particles, 2, cps, 0.0f, MovementTime {});
            for (const auto& p : particles) {
                const uint32_t seed = std::bit_cast<uint32_t> (p.oscillatorRandom);
                const float runtimeExpected = (std::string (transform) == "simplexnoise"
                    ? remapSimplexNoise (seed, 0.25f) : remapFBMNoise (seed, 0.25f, count)) * 0.5f + 0.5f;
                // Runtime keeps nonfinite current values unchanged after birth FBM0.
                if (std::string (transform) == "fbmnoise" && count == 0) REQUIRE (std::isnan (p.alpha));
                else REQUIRE (p.alpha == runtimeExpected);
            }
        }
    }
}

TEST_CASE ("Production birth noise preserves per-axis and raw output arithmetic",
           "[particle][birth][remap][noise]") {
    using namespace WallpaperEngine::Data::Model;
    using namespace WallpaperEngine::Render::Objects;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const float nan = std::numeric_limits<float>::quiet_NaN ();
    JSON data = JSON::parse (R"({"id":1,"particle":{"initializer":[
        {"name":"remapinitialvalue","input":"position","output":"velocity",
         "operation":"remap","flags":0,"transformfunction":"simplexnoise","transforminputscale":1},
        {"name":"remapinitialvalue","input":"position","inputcomponent":"max","output":"opacity",
         "operation":"remap","flags":0,"transformfunction":"simplexnoise","transforminputscale":1}
    ]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto& model = *object->as<Particle> ();
    const auto vector = createVectorRemapOperator (*model.initializers[0]->as<RemapInitialValueInitializer> ()
        ->remap->as<VectorRemapValueOperator> (), true, false, false, false);
    const auto scalar = createScalarRemapOperator (*model.initializers[1]->as<RemapInitialValueInitializer> ()
        ->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
    std::vector<ParticleInstance> particles (1);
    particles[0].alive = true;
    particles[0].position = toSimulationVector ({ 0.125f, -0.25f, 1.5f });
    std::vector<ControlPointData> cps (8);
    vector (particles, 1, cps, 0.0f, MovementTime {});
    const auto velocity = toAuthoredVector (particles[0].velocity);
    const std::array<uint32_t, 3> expected { 0x3f2f56fdu, 0x3e635576u, 0x3f17ff0au };
    for (int axis = 0; axis < 3; ++axis)
        REQUIRE (std::bit_cast<uint32_t> (velocity[axis]) == expected[axis]);
    particles[0].position = toSimulationVector ({ 0.125f, nan, 1.5f });
    scalar (particles, 1, cps, 0.0f, MovementTime {});
    REQUIRE (std::bit_cast<uint32_t> (particles[0].alpha) == expected[0]);

    for (const auto output : { "size", "opacity", "maxlifetime", "rotation", "angularspeed", "speed" }) {
        for (int flags = 0; flags <= 3; ++flags) {
            JSON definition = JSON::parse (R"({"id":1,"particle":{"initializer":[
                {"name":"remapinitialvalue","input":"size","operation":"remap",
                 "transformfunction":"fbmnoise","transformoctaves":0}
            ]}})");
            auto& entry = definition["particle"]["initializer"][0];
            entry["output"] = output; entry["flags"] = flags;
            const auto parsed = ObjectParser::parse (definition, project);
            REQUIRE (parsed->as<Particle> ()->initializers.size () == 1);
            const auto* remap = parsed->as<Particle> ()->initializers[0]
                ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> ();
            REQUIRE (remap != nullptr);
            const auto closure = createScalarRemapOperator (*remap, true, false, false, true);
            particles[0] = ParticleInstance {};
            particles[0].alive = true; particles[0].size = 0.125f;
            particles[0].velocity = { 3, 4, 0 };
            closure (particles, 1, cps, 0.0f, MovementTime {});
            const float result = std::string (output) == "size" ? particles[0].size
                : std::string (output) == "opacity" ? particles[0].alpha
                : std::string (output) == "maxlifetime" ? particles[0].lifetime
                : std::string (output) == "rotation" ? particles[0].rotation.z
                : std::string (output) == "angularspeed" ? particles[0].angularVelocity.z
                : particles[0].velocity.x;
            REQUIRE (std::isnan (result));
            if (std::string (output) == "speed")
                for (int axis = 0; axis < 3; ++axis) REQUIRE (std::isnan (particles[0].velocity[axis]));
        }
    }
    for (const float input : { nan, std::numeric_limits<float>::infinity (),
                              -std::numeric_limits<float>::infinity () }) {
        JSON definition = JSON::parse (R"({"id":1,"particle":{"initializer":[
            {"name":"remapinitialvalue","input":"size","output":"opacity","flags":0,
             "operation":"remap","transformfunction":"simplexnoise","transforminputscale":1}
        ]}})");
        const auto parsed = ObjectParser::parse (definition, project);
        const auto closure = createScalarRemapOperator (*parsed->as<Particle> ()->initializers[0]
            ->as<RemapInitialValueInitializer> ()->remap->as<ScalarRemapValueOperator> (), true, false, false, false);
        particles[0] = ParticleInstance {}; particles[0].alive = true; particles[0].size = input;
        closure (particles, 1, cps, 0.0f, MovementTime {});
        if (std::isnan (input)) REQUIRE (std::isnan (particles[0].alpha));
        else REQUIRE (particles[0].alpha == input);
    }
}
