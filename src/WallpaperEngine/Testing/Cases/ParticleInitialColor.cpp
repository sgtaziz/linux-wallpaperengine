#include "WallpaperEngine/Render/Objects/ParticleInitialColor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

namespace {
void requireRgb (glm::vec3 actual, glm::vec3 expected, float margin = 5e-7f) {
    for (int axis = 0; axis < 3; ++axis)
        REQUIRE (actual[axis] == Catch::Approx (expected[axis]).margin (margin));
}
}

TEST_CASE ("HSV birth color keeps the CRT stream independent of particle MT",
           "[particle][initializer][color][random]") {
    uint32_t crtState = 1;
    REQUIRE (nativeCrtRandomWord (crtState) == 41u);
    REQUIRE (nativeCrtRandomWord (crtState) == 18467u);
    REQUIRE (nativeCrtRandomWord (crtState) == 6334u);
    std::mt19937 rng (5489), expected (5489);
    HsvRandomRange range;
    range.saturationMin = range.saturationMax = 1.0f;
    range.valueMin = range.valueMax = 1.0f;
    requireRgb (nativeHsvRandomSample (rng, 32767u, range), glm::vec3 (1, 0, 0));
    expected.discard (2);
    REQUIRE (rng () == expected ());
}

TEST_CASE ("HSV hue packing retains native inclusive indices and full-turn threshold",
           "[particle][initializer][color]") {
    HsvRandomRange range;
    REQUIRE (compileNativeHsvRandomRange (range).hueStep == Catch::Approx (1.0f / 6.0f));
    range.hueMin = 0.2f;
    range.hueMax = 0.8f;
    const auto partial = compileNativeHsvRandomRange (range);
    REQUIRE (partial.hueMaxIndex == 6);
    REQUIRE (partial.hueStep == Catch::Approx (0.12f));
    // The sixth inclusive index is beyond authored max; do not replace
    // native count semantics with an apparently nicer endpoint sampler.
    REQUIRE (partial.hueMin + 6 * partial.hueStep == Catch::Approx (0.92f));
    range.hueMax = range.hueMin;
    REQUIRE (compileNativeHsvRandomRange (range).hueStep == Catch::Approx (1.0f / 6.0f));
    range.hueSteps = 1;
    REQUIRE (compileNativeHsvRandomRange (range).hueStep == 0.0f);
    range.hueSteps = -3;
    REQUIRE (compileNativeHsvRandomRange (range).hueMaxIndex == 0);
}

TEST_CASE ("HSV saturation precedes value in the particle random stream",
           "[particle][initializer][color][random]") {
    HsvRandomRange range;
    range.hueSteps = 0;
    std::mt19937 rng (5489);
    const float saturation = static_cast<float> (3499211612u >> 8) * 0x1p-24f * 0.5f + 0.5f;
    const float value = static_cast<float> (581869302u >> 8) * 0x1p-24f * 0.5f + 0.5f;
    requireRgb (nativeHsvRandomSample (rng, 0u, range),
                {value, value * (1.0f - saturation), value * (1.0f - saturation)});
}

TEST_CASE ("HSV tint recenters ranges and preserves native negative-hue conversion",
           "[particle][initializer][color][instance]") {
    HsvRandomRange range;
    range.saturationMin = 0.2f;
    range.saturationMax = 0.8f;
    range.valueMin = 0.1f;
    range.valueMax = 0.5f;
    const auto packed = compileNativeHsvRandomRange (range, {1.0f, 0.0f, 0.0f}, true);
    REQUIRE (packed.hueMin == Catch::Approx (-0.5f));
    REQUIRE (packed.saturation.x == Catch::Approx (0.7f));
    REQUIRE (packed.saturation.y == Catch::Approx (0.3f));
    REQUIRE (packed.value.x == Catch::Approx (0.8f));
    REQUIRE (packed.value.y == Catch::Approx (0.2f));
    requireRgb (nativeInitialHsvToRgb ({-0.1f, 1.0f, 1.0f}), glm::vec3 (0.0f));
    requireRgb (nativeInitialHsvToRgb ({1.1f, 1.0f, 1.0f}), {1.0f, 0.6f, 0.0f});
    const auto unpatched = compileNativeHsvRandomRange (range, {1, 0, 0}, false);
    REQUIRE (unpatched.hueMin == 0.0f);
    REQUIRE (unpatched.saturation.x == 0.2f);
}

TEST_CASE ("Native early color-list births replay captured selection and MT tails",
           "[particle][initializer][color][native-capture]") {
    // 2.8.42 capture 20260926-164205-817470, pairs
    // 9841/9848/9852/9859/9863/9870/9874/9881, slots 0..7. The first
    // captured MT half still contains the seed and its expansion. The
    // initializer prologue and earlier records consume seven words before
    // opcode 5. Total native before/after delta is eleven words per birth.
    constexpr uint32_t seed = 0xddf73f18u;
    const ColorListRandomRange range {{
        {0.0f, 0.611764705882353f, 1.0f},
        {1.0f, 0.9607843137254902f, 0.0f},
        {0.9333333333333333f, 0.0f, 1.0f}}, glm::vec3 (0.0f)};
    const std::array<glm::vec3, 8> colors {{
        {1.0f, 0.9607843160629272f, 0.0f},
        {0.0f, 0.6117644309997559f, 1.0f},
        {0.0f, 0.6117644309997559f, 1.0f},
        {0.0f, 0.6117644309997559f, 1.0f},
        {0.9333333969116211f, 0.0f, 1.0f},
        {0.0f, 0.6117644309997559f, 1.0f},
        {0.0f, 0.6117644309997559f, 1.0f},
        {0.0f, 0.6117644309997559f, 1.0f}}};
    for (uint32_t birth = 0; birth < colors.size (); ++birth) {
        std::mt19937 rng (seed), tail (seed);
        const uint32_t before = 4u + birth * 15u;
        rng.discard (before + 7u);
        requireRgb (nativeColorListSample (rng, range), colors[birth]);
        tail.discard (before + 11u);
        REQUIRE (rng () == tail ());
    }
}

TEST_CASE ("Color-list noise clips HSV endpoints before tint and always draws H S V",
           "[particle][initializer][color][random]") {
    std::mt19937 rng (5489), tail (5489);
    const ColorListRandomRange range {{glm::vec3 (1.0f, 0.0f, 0.0f)}, {0.2f, 0.4f, 0.6f}};
    // Independent MT19937 published sequence: selection3499211612,
    // H581869302,S3890346734,V3586334585. Clipped ranges are
    // H[0,.2], S[.6,1], V[.4,1], not symmetric random signed offsets.
    const float h = static_cast<float> (581869302u >> 8) * 0x1p-24f * 0.2f;
    const float s = static_cast<float> (3890346734u >> 8) * 0x1p-24f * 0.4f + 0.6f;
    const float v = static_cast<float> (3586334585u >> 8) * 0x1p-24f * 0.6f + 0.4f;
    requireRgb (nativeColorListSample (rng, range),
                {v, v * (1.0f - s + s * h * 6.0f), v * (1.0f - s)});
    tail.discard (4);
    REQUIRE (rng () == tail ());
    ColorListRandomRange empty;
    empty.colorsRgb.clear ();
    requireRgb (nativeColorListSample (rng, empty), {1.0f, 0.0f, 0.0f});
    tail.discard (4);
    REQUIRE (rng () == tail ());
}

TEST_CASE ("Color-list instance tint shifts relative to first authored entry",
           "[particle][initializer][color][instance]") {
    const ColorListRandomRange colors {{ {1, 0, 0}, {0, 1, 0} }, glm::vec3 (0.0f)};
    std::mt19937 rng (5489); // first selection chooses second entry
    requireRgb (nativeColorListSample (rng, colors, {0, 0, 1}, true), {1, 0, 0});
    std::mt19937 unpatched (5489);
    requireRgb (nativeColorListSample (unpatched, colors, {0, 0, 1}, false), {0, 1, 0});
    // Initializers multiply the caller's existing color; arbitrary tint or
    // brightness gain therefore survives instead of becoming a replacement.
    const glm::vec3 birthGain (0.4f, 0.7f, 1.3f);
    std::mt19937 constant (5489);
    requireRgb (birthGain * nativeColorListSample (constant, {{ {0, 1, 0} }, {0, 0, 0}}),
                {0.0f, 0.7f, 0.0f});
}

TEST_CASE ("HSV native captured births constrain hue sectors and particle draw tails",
           "[particle][initializer][color][native-capture]") {
    // Capture pairs10886/10887/10891/10895/10896/10900/10904/10908.
    // CRT state is absent, so these are conditional sector replays, not a
    // claim to reconstruct the external CRT sequence. Before/after MT
    // deltas are five: prologue/lifetime/size/S/V, with no MT hue draw.
    const std::array<uint32_t, 8> indices {2, 5, 1, 5, 4, 0, 0, 3};
    const std::array<glm::vec3, 8> colors {{ {0, 1, 0}, {1, 0, 1}, {1, 1, 0}, {1, 0, 1},
                                            {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {0, 1, 1} }};
    HsvRandomRange range;
    range.saturationMin = range.saturationMax = 1.0f;
    range.valueMin = range.valueMax = 1.0f;
    for (uint32_t birth = 0; birth < colors.size (); ++birth) {
        std::mt19937 rng (0xddf796c1u), tail (0xddf796c1u);
        const uint32_t before = 4u + birth * 9u;
        rng.discard (before + 3u);
        const uint32_t representativeCrtWord = (indices[birth] * 32767u + 16383u) / 7u;
        requireRgb (nativeHsvRandomSample (rng, representativeCrtWord, range), colors[birth]);
        tail.discard (before + 5u);
        REQUIRE (rng () == tail ());
    }
}

TEST_CASE ("Birth remap replays native control-point distance colors before operators",
           "[particle][initializer][remap][native-capture]") {
    // Capture pairs17051/17052/17056/17060/17064/17068/17072/17076,
    // slots0..7; complete fixture instance controlpoint1=(2.70951,-11.91028,0).
    // Captured pre-initializer authored position X, post-initializer red.
    constexpr std::array<float, 8> positions {-292.72979736328125f, -7.128927230834961f,
        263.5909729003906f, 35.736572265625f, 168.9049072265625f, -470.5722351074219f,
        278.82342529296875f, -251.4094696044922f};
    constexpr std::array<float, 8> red {0.9855976104736328f, 0.0514942966401577f,
        0.8705106377601624f, 0.11702997982501984f, 0.5554054379463196f, 1.0f,
        0.9212355613708496f, 0.8479931354522705f};
    const glm::vec3 cp (2.70951f, -11.91028f, 0.0f);
    for (size_t birth = 0; birth < positions.size (); ++birth) {
        const float distance = remapControlPointDistance ({positions[birth], 0, 0}, cp);
        const auto color = remapVectorValue (glm::vec3 (1.0f), glm::vec3 (distance), 0.0f,
            RemapOperation::Set, glm::vec3 (0.0f), glm::vec3 (300.0f),
            {0, 0, 1}, {1, 0, 0}, 1, std::nullopt, 0);
        requireRgb (color, {red[birth], 0.0f, 1.0f - red[birth]});
    }
}
