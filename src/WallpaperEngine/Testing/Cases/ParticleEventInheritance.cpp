#include "WallpaperEngine/Render/Objects/ParticleEventInheritance.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

namespace {
void requireVector (glm::vec3 actual, glm::vec3 expected, float margin = 1e-6f) {
    for (int axis = 0; axis < 3; ++axis)
        REQUIRE (actual[axis] == Catch::Approx (expected[axis]).margin (margin));
}
EventParticleValues parentValues () {
    EventParticleValues p;
    p.color = {0.2f, 0.4f, 0.8f}; p.alpha = 0.3f; p.size = 8;
    p.velocity = {2, -3, 5}; p.rotation = {0.5f, 0.75f, 1};
    p.angularVelocity = {-1, 2, 3};
    p.valid = p.alive = p.rotationValid = p.angularVelocityValid = true;
    return p;
}
}

TEST_CASE ("Event inheritance selectors preserve each native channel operation",
           "[particle][event][initializer][operator]") {
    const auto p = parentValues ();
    const std::array<EventInheritanceMode, 14> modes {
        EventInheritanceMode::SetColor, EventInheritanceMode::MultiplyColor,
        EventInheritanceMode::SetOpacity, EventInheritanceMode::MultiplyOpacity,
        EventInheritanceMode::SetColorOpacity, EventInheritanceMode::MultiplyColorOpacity,
        EventInheritanceMode::SetVelocity, EventInheritanceMode::AddVelocity,
        EventInheritanceMode::SetSize, EventInheritanceMode::MultiplySize,
        EventInheritanceMode::SetRotation, EventInheritanceMode::AddRotation,
        EventInheritanceMode::SetAngularVelocity, EventInheritanceMode::AddAngularVelocity
    };
    for (size_t i = 0; i < modes.size (); ++i) {
        EventParticleValues c;
        c.color = glm::vec3 (0.5f); c.alpha = 0.5f; c.size = 2;
        c.velocity = glm::vec3 (1); c.rotation = glm::vec3 (2); c.angularVelocity = glm::vec3 (3);
        c.rotationValid = c.angularVelocityValid = true;
        REQUIRE (applyEventInheritance (c, p, modes[i]));
        requireVector (c.color, i == 0 || i == 4 ? p.color
            : i == 1 || i == 5 ? p.color * 0.5f : glm::vec3 (0.5f));
        REQUIRE (c.alpha == Catch::Approx (i == 2 || i == 4 ? 0.3f : i == 3 || i == 5 ? 0.15f : 0.5f));
        REQUIRE (c.size == Catch::Approx (i == 8 ? 8 : i == 9 ? 16 : 2));
        requireVector (c.velocity, i == 6 ? p.velocity : i == 7 ? p.velocity + 1.0f : glm::vec3 (1));
        requireVector (c.rotation, i == 10 ? p.rotation : i == 11 ? p.rotation + 2.0f : glm::vec3 (2));
        requireVector (c.angularVelocity, i == 12 ? p.angularVelocity : i == 13 ? p.angularVelocity + 3.0f : glm::vec3 (3));
    }
}

TEST_CASE ("Event initializer accepts expired parents while operators require a live slot",
           "[particle][event][lifecycle]") {
    auto p = parentValues ();
    p.alive = false;
    EventParticleValues c;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetColor));
    requireVector (c.color, p.color);
    p.color = glm::vec3 (0.9f);
    REQUIRE_FALSE (applyEventInheritance (c, p, EventInheritanceMode::SetColor, 1, true));
    requireVector (c.color, {0.2f, 0.4f, 0.8f});
    // A native event link names a pool slot, so a newly live value in that
    // slot resumes inheritance instead of pinning the old event snapshot.
    p.alive = true;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetColor, 1, true));
    requireVector (c.color, glm::vec3 (0.9f));
    p.valid = false;
    REQUIRE_FALSE (applyEventInheritance (c, p, EventInheritanceMode::SetOpacity));
    p.valid = true;
    REQUIRE_FALSE (applyEventInheritance (c, p, EventInheritanceMode::Noop));
    REQUIRE_FALSE (applyEventInheritance (c, p, static_cast<EventInheritanceMode> (15)));
}

TEST_CASE ("Event orientation streams require both native allocation flags",
           "[particle][event][rotation]") {
    for (bool parentFlag : {false, true}) for (bool childFlag : {false, true}) {
        auto p = parentValues ();
        EventParticleValues c;
        p.rotationValid = parentFlag; c.rotationValid = childFlag;
        REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetRotation) == (parentFlag && childFlag));
        requireVector (c.rotation, parentFlag && childFlag ? p.rotation : glm::vec3 (0));
        p.angularVelocityValid = parentFlag; c.angularVelocityValid = childFlag;
        REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::AddAngularVelocity) == (parentFlag && childFlag));
        requireVector (c.angularVelocity, parentFlag && childFlag ? p.angularVelocity : glm::vec3 (0));
    }
}

TEST_CASE ("Event envelopes blend set, multiplicative and additive selectors differently",
           "[particle][event][envelope]") {
    auto p = parentValues ();
    EventParticleValues c;
    c.velocity = glm::vec3 (4);
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetColorOpacity, 0.25f, true));
    requireVector (c.color, {0.8f, 0.85f, 0.95f});
    REQUIRE (c.alpha == Catch::Approx (0.825f));
    c.color = glm::vec3 (0.5f); c.alpha = 0.5f;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::MultiplyColorOpacity, 0.25f, true));
    requireVector (c.color, {0.4f, 0.425f, 0.475f});
    REQUIRE (c.alpha == Catch::Approx (0.4125f));
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::AddVelocity, 0.25f, true));
    requireVector (c.velocity, {4.5f, 3.25f, 5.25f});
    c.size = 9;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetSize, 0, true));
    REQUIRE (c.size == 9);
    // Native opcode20 copies directly; opcode40 retains float subtraction
    // even at envelope1. Do not merge the two paths based only on q's value.
    p.size = 1; c.size = 1e30f;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetSize, 1, true));
    REQUIRE (c.size == 1);
    c.size = 1e30f;
    REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetSize, 1, true, true));
    REQUIRE (c.size == 0);
}

TEST_CASE ("Captured death event initializes child color from expired parent current stream",
           "[particle][event][native-capture]") {
    // 2.8.42 trace 20260926-164205-817470, parent0x321ea90 after pair11816;
    // child0x304ce20 pairs11821..11825, opcode16 selector0. Parent lifetime
    // marker+260 is zero. Source+2f8/+300/+308 equals child baseline+318/+320/+328.
    EventParticleValues p;
    p.valid = true; p.alive = false;
    p.color = {0.17440247535705566f, 0.17440247535705566f, 0.5600521564483643f};
    for (unsigned pair : {11821, 11822, 11823, 11824, 11825}) {
        INFO ("native pair " << pair);
        EventParticleValues c;
        REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetColor));
        requireVector (c.color, {0.17440247535705566f, 0.17440247535705566f, 0.5600521564483643f});
        REQUIRE (c.alpha == 1);
    }
}

TEST_CASE ("Captured event operator follows parent current opacity rather than birth opacity",
           "[particle][event][operator][native-capture]") {
    // Unambiguous parent pool: highwater1. Native parent0x322d780 and child
    //0x2f4efd0, pairs12151,12157,12163,12169,12175,12181,12187. Child opcode20
    // selector4 copies parent current+310; parent birth+330 remains1 throughout.
    constexpr std::array<float, 7> alpha {0, 0.19207358360290527f,
        0.3863420784473419f, 0.5800284147262573f, 0.7717266082763672f,
        0.9643570184707642f, 1};
    EventParticleValues c, p;
    p.valid = p.alive = true;
    p.color = {0.7814501523971558f, 0.7814501523971558f, 0.35099372267723083f};
    for (float currentAlpha : alpha) {
        p.alpha = currentAlpha;
        REQUIRE (applyEventInheritance (c, p, EventInheritanceMode::SetColorOpacity, 1, true));
        requireVector (c.color, {0.7814501523971558f, 0.7814501523971558f, 0.35099372267723083f});
        REQUIRE (c.alpha == currentAlpha);
    }
}

TEST_CASE ("CP velocity inherits translation rate with native transform order",
           "[particle][initializer][controlpoint]") {
    const glm::mat3 inverse (glm::vec3 (0, -0.5f, 0), glm::vec3 (0.25f, 0, 0), glm::vec3 (0, 0, 2));
    const glm::mat3 birth (glm::vec3 (2, 0, 0), glm::vec3 (0, 3, 0), glm::vec3 (0, 0, 4));
    for (bool world : {false, true}) for (bool cpWorld : {false, true}) {
        std::mt19937 rng (5489), expected (5489);
        const auto actual = inheritedControlPointVelocity (rng, {4, 8, 12}, {2, 4, 6}, 0.5f,
            0.25f, 0.25f, birth, inverse, world, cpWorld);
        requireVector (actual, world && !cpWorld ? glm::vec3 (1, -1.5f, 24) : glm::vec3 (2, 6, 12));
        expected.discard (1);
        REQUIRE (rng () == expected ());
    }
}

TEST_CASE ("Captured CP velocity twin births share rate and retain exact MT factor draws",
           "[particle][initializer][controlpoint][native-capture]") {
    // The collector did not retain CP matrices/root+150. Absolute rate is
    // intentionally not asserted. Each same-frame pair holds that unknown
    // rate constant: the second captured velocity is independently predicted
    // from the first and their recovered MT factors. Initializer consumes five
    // total words (prologue,lifetime,size,color,CP); emitter uses four between.
    // Native seed0xddf7b404, pairs11276/77,11281/82,11286/87,11291/92.
    struct Pair { unsigned offset; glm::vec3 first, second; };
    const std::array<Pair, 4> pairs {{
        {31, {-91.9408493f, 302.067993f, 0}, {-66.8104858f, 219.503189f, 0}},
        {49, {-329.788818f, 361.327057f, 0}, {-270.941132f, 296.851685f, 0}},
        {67, {-144.749435f, 85.9530487f, 0}, {-397.173950f, 235.844177f, 0}},
        {85, {-256.940369f, 65.4916458f, 0}, {-201.411484f, 51.3378677f, 0}}
    }};
    for (const auto& pair : pairs) {
        std::mt19937 first (0xddf7b404u), second (0xddf7b404u);
        first.discard (pair.offset + 4); second.discard (pair.offset + 9 + 4);
        const float factor = nativeRandomUnit (first) * 0.7f + 0.3f;
        const glm::vec3 rate = pair.first / factor;
        requireVector (inheritedControlPointVelocity (second, rate, glm::vec3 (0), 1, 0.3f, 1.0f),
            pair.second, 1e-4f);
        std::mt19937 tail (0xddf7b404u);
        tail.discard (pair.offset + 9 + 5);
        REQUIRE (second () == tail ());
    }
    std::mt19937 stationary (0xddf7b404u), tail (0xddf7b404u);
    stationary.discard (8); tail.discard (9);
    requireVector (inheritedControlPointVelocity (stationary, {8, 4, 0}, {8, 4, 0}, 0.033f, 0.3f, 1), glm::vec3 (0));
    REQUIRE (stationary () == tail ());
}
