#include "WallpaperEngine/Render/Objects/ParticleBirthGeometry.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <limits>

using namespace WallpaperEngine::Render::Objects::ParticleCore;

namespace {
struct NativeBirth { float phase, nextPhase, time; glm::vec3 position; };
// Windows 2.8.42 SHA256 40e2ce021e9352324fadb3b8f72b8ba2a7ee95b71cc571d5b9f84be75cd993b0.
// Retained initializer pairs from capture 20260926-164205-817470. Phase and
// nextPhase are decoded from opcode14 payload+8 before/after. Positions are
// SoA lanes +0x2b0/+0x2b8/+0x2c0. Offset time is the float32 clock reconstructed
// from the recorded tick arguments; exclude births after the 96-tick cap.
// These rows are native observations, independent of the Linux evaluator.
const std::array<NativeBirth, 53> sequenceBirths {{
    {0.0f, 0.03225806355f, 0.09591430007f, {-388.7520142f, -380.341095f, 0.0f}}, // pair 14584
    {0.03225806355f, 0.06451612711f, 0.1621318976f, {-364.3214111f, -356.5023804f, 0.0f}}, // pair 14591
    {0.06451612711f, 0.09677419066f, 0.2282321001f, {-339.8908081f, -332.6636658f, 0.0f}}, // pair 14598
    {0.09677419066f, 0.1290322542f, 0.295138001f, {-315.4602051f, -308.8249817f, 0.0f}}, // pair 14605
    {0.1290322542f, 0.1612903178f, 0.3609132017f, {-291.0296021f, -284.9862671f, 0.0f}}, // pair 14612
    {0.1612903178f, 0.1935483813f, 0.4269664022f, {-266.598999f, -261.1475525f, 0.0f}}, // pair 14619
    {0.1935483813f, 0.2258064449f, 0.4931728016f, {-242.1684265f, -237.3088531f, 0.0f}}, // pair 14626
    {0.2258064449f, 0.2580645084f, 0.5593211013f, {-217.7378082f, -213.4701385f, 0.0f}}, // pair 14633
    {0.2580645084f, 0.290322572f, 0.6259949016f, {-193.3072052f, -189.631424f, 0.0f}}, // pair 14640
    {0.290322572f, 0.3225806355f, 0.6933203037f, {-168.8766022f, -165.7927246f, 0.0f}}, // pair 14647
    {0.3225806355f, 0.3548386991f, 0.7606426063f, {-144.4460144f, -141.95401f, 0.0f}}, // pair 14654
    {0.3548386991f, 0.3870967627f, 0.8279217067f, {-120.0154114f, -118.1152954f, 0.0f}}, // pair 14661
    {0.3870967627f, 0.4193548262f, 0.8948524068f, {-95.58483887f, -94.27661133f, 0.0f}}, // pair 14668
    {0.4193548262f, 0.4516128898f, 0.9628608073f, {-71.15420532f, -70.43789673f, 0.0f}}, // pair 14675
    {0.4516128898f, 0.4838709533f, 1.030336808f, {-46.72360229f, -46.59918213f, 0.0f}}, // pair 14682
    {0.4838709533f, 0.5161290169f, 1.096165407f, {-22.29299927f, -22.76046753f, 0.0f}}, // pair 14689
    {0.5161290169f, 0.5483870506f, 1.162024807f, {2.13760376f, 1.07824707f, 0.0f}}, // pair 14696
    {0.5483870506f, 0.5806450844f, 1.227846007f, {26.56817627f, 24.91690063f, 0.0f}}, // pair 14703
    {0.5806450844f, 0.6129031181f, 1.293580606f, {50.99874878f, 48.75561523f, 0.0f}}, // pair 14710
    {0.6129031181f, 0.6451611519f, 1.360192708f, {75.42935181f, 72.59429932f, 0.0f}}, // pair 14717
    {0.6451611519f, 0.6774191856f, 1.427165307f, {99.8598938f, 96.4329834f, 0.0f}}, // pair 14724
    {0.6774191856f, 0.7096772194f, 1.493815608f, {124.2904663f, 120.2716675f, 0.0f}}, // pair 14731
    {0.7096772194f, 0.7419352531f, 1.560513511f, {148.7210693f, 144.1103821f, 0.0f}}, // pair 14738
    {0.7419352531f, 0.7741932869f, 1.627464212f, {173.1516113f, 167.9490051f, 0.0f}}, // pair 14745
    {0.7741932869f, 0.8064513206f, 1.694080412f, {197.5822144f, 191.7876892f, 0.0f}}, // pair 14752
    {0.8064513206f, 0.8387093544f, 1.76098641f, {222.0127563f, 215.6263733f, 0.0f}}, // pair 14759
    {0.8387093544f, 0.8709673882f, 1.82750101f, {246.4433594f, 239.4651184f, 0.0f}}, // pair 14766
    {0.8709673882f, 0.9032254219f, 1.893721111f, {270.8739624f, 263.3038025f, 0.0f}}, // pair 14773
    {0.9032254219f, 0.9354834557f, 1.96219331f, {295.3045044f, 287.1424866f, 0.0f}}, // pair 14780
    {0.9354834557f, 0.9677414894f, 2.030108407f, {319.7351074f, 310.9811707f, 0.0f}}, // pair 14787
    {0.9677414894f, 0.9999995232f, 2.09565071f, {344.1657104f, 334.8198547f, 0.0f}}, // pair 14794
    {0.9999995232f, 0.0f, 2.16221961f, {368.5962524f, 358.6585388f, 0.0f}}, // pair 14801
    {0.0f, 0.03225806355f, 2.227859206f, {-388.7520142f, -380.341095f, 0.0f}}, // pair 14808
    {0.03225806355f, 0.06451612711f, 2.293596204f, {-364.3214111f, -356.5023804f, 0.0f}}, // pair 14815
    {0.06451612711f, 0.09677419066f, 2.359707605f, {-339.8908081f, -332.6636658f, 0.0f}}, // pair 14822
    {0.09677419066f, 0.1290322542f, 2.425239004f, {-315.4602051f, -308.8249817f, 0.0f}}, // pair 14829
    {0.1290322542f, 0.1612903178f, 2.491310406f, {-291.0296021f, -284.9862671f, 0.0f}}, // pair 14836
    {0.1612903178f, 0.1935483813f, 2.558568503f, {-266.598999f, -261.1475525f, 0.0f}}, // pair 14843
    {0.1935483813f, 0.2258064449f, 2.625111803f, {-242.1684265f, -237.3088531f, 0.0f}}, // pair 14850
    {0.2258064449f, 0.2580645084f, 2.691459402f, {-217.7378082f, -213.4701385f, 0.0f}}, // pair 14857
    {0.2580645084f, 0.290322572f, 2.7578624f, {-193.3072052f, -189.631424f, 0.0f}}, // pair 14864
    {0.290322572f, 0.3225806355f, 2.825640797f, {-168.8766022f, -165.7927246f, 0.0f}}, // pair 14871
    {0.3225806355f, 0.3548386991f, 2.892701998f, {-144.4460144f, -141.95401f, 0.0f}}, // pair 14878
    {0.3548386991f, 0.3870967627f, 2.961013998f, {-120.0154114f, -118.1152954f, 0.0f}}, // pair 14885
    {0.3870967627f, 0.4193548262f, 3.028714899f, {-95.58483887f, -94.27661133f, 0.0f}}, // pair 14892
    {0.4193548262f, 0.4516128898f, 3.094319097f, {-71.15420532f, -70.43789673f, 0.0f}}, // pair 14899
    {0.4516128898f, 0.4838709533f, 3.160070296f, {-46.72360229f, -46.59918213f, 0.0f}}, // pair 14906
    {0.4838709533f, 0.5161290169f, 3.192783396f, {-22.29299927f, -22.76046753f, 0.0f}}, // pair 14911
    {0.5161290169f, 0.5483870506f, 3.192783396f, {2.13760376f, 1.07824707f, 0.0f}}, // pair 14912
    {0.5483870506f, 0.5806450844f, 3.192783396f, {26.56817627f, 24.91690063f, 0.0f}}, // pair 14913
    {0.5806450844f, 0.6129031181f, 3.192783396f, {50.99874878f, 48.75561523f, 0.0f}}, // pair 14914
    {0.6129031181f, 0.6451611519f, 3.192783396f, {75.42935181f, 72.59429932f, 0.0f}}, // pair 14915
    {0.6451611519f, 0.6774191856f, 3.192783396f, {99.8598938f, 96.4329834f, 0.0f}}, // pair 14916
}};
const std::array<NativeBirth, 47> offsetBirths {{
    {0.0f, 0.03225806355f, 0.06814190123f, {-474.4421387f, -326.8716736f, 0.0f}}, // pair 16324
    {0.03225806355f, 0.06451612711f, 0.1337475044f, {-436.5401917f, -372.6000366f, 0.0f}}, // pair 16331
    {0.06451612711f, 0.09677419066f, 0.2319464025f, {-403.4785156f, -341.5964966f, 0.0f}}, // pair 16341
    {0.09677419066f, 0.1290322542f, 0.2975358007f, {-352.1875916f, -312.1442566f, 0.0f}}, // pair 16348
    {0.1290322542f, 0.1612903178f, 0.3629877989f, {-273.7049561f, -256.8086243f, 0.0f}}, // pair 16355
    {0.1612903178f, 0.1935483813f, 0.4288600993f, {-236.1949158f, -221.2992706f, 0.0f}}, // pair 16362
    {0.1935483813f, 0.2258064449f, 0.4950717998f, {-191.6718445f, -197.4318542f, 0.0f}}, // pair 16369
    {0.2258064449f, 0.2580645084f, 0.5614177001f, {-199.6023254f, -187.8212891f, 0.0f}}, // pair 16376
    {0.2580645084f, 0.290322572f, 0.6281185982f, {-167.5104828f, -165.283371f, 0.0f}}, // pair 16383
    {0.290322572f, 0.3225806355f, 0.6941837979f, {-148.5450745f, -137.5620575f, 0.0f}}, // pair 16390
    {0.3225806355f, 0.3548386991f, 0.761544497f, {-182.7509766f, -119.0065155f, 0.0f}}, // pair 16397
    {0.3548386991f, 0.3870967627f, 0.8284543951f, {-207.5915985f, -114.7354431f, 0.0f}}, // pair 16404
    {0.3870967627f, 0.4193548262f, 0.8965330956f, {-196.0559845f, -85.75268555f, 0.0f}}, // pair 16411
    {0.4193548262f, 0.4516128898f, 0.9625837964f, {-168.833374f, -111.0077057f, 0.0f}}, // pair 16418
    {0.4516128898f, 0.4838709533f, 1.030175497f, {-130.9805908f, -78.14202881f, 0.0f}}, // pair 16425
    {0.4838709533f, 0.5161290169f, 1.096025498f, {-69.86296082f, -38.89233398f, 0.0f}}, // pair 16432
    {0.5161290169f, 0.5483870506f, 1.161533096f, {-17.73870087f, -5.930972099f, 0.0f}}, // pair 16439
    {0.5483870506f, 0.5806450844f, 1.227311896f, {60.04516602f, 5.716051102f, 0.0f}}, // pair 16446
    {0.5806450844f, 0.6129031181f, 1.293690694f, {102.4852524f, -14.22917557f, 0.0f}}, // pair 16453
    {0.6129031181f, 0.6451611519f, 1.360277397f, {157.1372986f, 9.973716736f, 0.0f}}, // pair 16460
    {0.6451611519f, 0.6774191856f, 1.426155799f, {186.7623138f, 87.25302124f, 0.0f}}, // pair 16467
    {0.6774191856f, 0.7096772194f, 1.492292499f, {226.3433228f, 163.9530945f, 0.0f}}, // pair 16474
    {0.7096772194f, 0.7419352531f, 1.559082796f, {218.174408f, 171.1724701f, 0.0f}}, // pair 16481
    {0.7419352531f, 0.7741932869f, 1.624890098f, {191.5157623f, 202.5581055f, 0.0f}}, // pair 16488
    {0.7741932869f, 0.8064513206f, 1.692148798f, {181.4862518f, 192.7396393f, 0.0f}}, // pair 16495
    {0.8064513206f, 0.8387093544f, 1.758968898f, {236.4983063f, 217.3890381f, 0.0f}}, // pair 16502
    {0.8387093544f, 0.8709673882f, 1.826922898f, {262.7132874f, 236.4939728f, 0.0f}}, // pair 16509
    {0.8709673882f, 0.9032254219f, 1.893678797f, {289.8609009f, 234.8851166f, 0.0f}}, // pair 16516
    {0.9032254219f, 0.9354834557f, 1.961927996f, {324.8905945f, 256.7917786f, 0.0f}}, // pair 16523
    {0.9354834557f, 0.9677414894f, 2.028523294f, {336.1785889f, 251.5792389f, 0.0f}}, // pair 16530
    {0.9677414894f, 0.9999995232f, 2.093677295f, {357.6152039f, 269.0094604f, 0.0f}}, // pair 16537
    {0.9999995232f, 0.0f, 2.159860694f, {410.1502075f, 306.2471008f, 0.0f}}, // pair 16544
    {0.0f, 0.03225806355f, 2.225837292f, {-374.5697937f, -364.6112366f, 0.0f}}, // pair 16551
    {0.03225806355f, 0.06451612711f, 2.292046593f, {-322.02771f, -320.3731079f, 0.0f}}, // pair 16558
    {0.06451612711f, 0.09677419066f, 2.358189193f, {-283.2702332f, -306.2039795f, 0.0f}}, // pair 16565
    {0.09677419066f, 0.1290322542f, 2.423849491f, {-266.5195007f, -274.13974f, 0.0f}}, // pair 16572
    {0.1290322542f, 0.1612903178f, 2.489786991f, {-236.0231018f, -240.9379425f, 0.0f}}, // pair 16579
    {0.1612903178f, 0.1935483813f, 2.556750291f, {-273.6655273f, -234.3225098f, 0.0f}}, // pair 16586
    {0.1935483813f, 0.2258064449f, 2.623427392f, {-302.3577881f, -246.6984863f, 0.0f}}, // pair 16593
    {0.2258064449f, 0.2580645084f, 2.690522191f, {-293.8121948f, -267.8489075f, 0.0f}}, // pair 16600
    {0.2580645084f, 0.290322572f, 2.757412494f, {-276.4840088f, -272.6116028f, 0.0f}}, // pair 16607
    {0.290322572f, 0.3225806355f, 2.825178192f, {-281.8667297f, -202.6114197f, 0.0f}}, // pair 16614
    {0.3225806355f, 0.3548386991f, 2.892514595f, {-212.7757263f, -164.7995911f, 0.0f}}, // pair 16621
    {0.3548386991f, 0.3870967627f, 2.959799496f, {-141.9837494f, -147.3960876f, 0.0f}}, // pair 16628
    {0.3870967627f, 0.4193548262f, 3.027827298f, {-103.5610657f, -85.12734222f, 0.0f}}, // pair 16635
    {0.4193548262f, 0.4516128898f, 3.094087401f, {-52.01393127f, -36.71849442f, 0.0f}}, // pair 16642
    {0.4516128898f, 0.4838709533f, 3.160253098f, {-2.356620789f, 10.43733215f, 0.0f}}, // pair 16649
}};
const glm::vec3 start (-388.75201f, -380.34109f, 0.0f);
const glm::vec3 end (368.59659f, 358.65891f, 0.0f);
void requirePosition (glm::vec3 actual, glm::vec3 expected, float tolerance) {
    for (int axis = 0; axis < 3; ++axis)
        REQUIRE (actual[axis] == Catch::Approx (expected[axis]).margin (tolerance));
}
} // namespace

TEST_CASE ("Sequence births replay current native positions and packed counter transitions",
           "[particle][birthgeometry][native]") {
    for (const auto& birth : sequenceBirths) {
        BetweenControlPointsState state {birth.phase, betweenControlPointsStep (32.0f)};
        const auto result = betweenControlPointsBirth ({}, {}, 1.0f, start, end,
            state, {0.0f, 1.0f}, false, 4u, 0.3f, {0.0f, 1.0f, 0.0f}, 0.9f, false);
        requirePosition (result.position, birth.position, 0.00004f);
        REQUIRE (state.phase == birth.nextPhase);
    }
}

TEST_CASE ("Position offsets replay retained native births before the clock capture cap",
           "[particle][birthgeometry][native][noise]") {
    for (const auto& birth : offsetBirths) {
        BetweenControlPointsState state {birth.phase, betweenControlPointsStep (32.0f)};
        const auto sequence = betweenControlPointsBirth ({}, {}, 1.0f, start, end,
            state, {0.0f, 1.0f}, false, 4u, 0.3f, {0.0f, 1.0f, 0.0f}, 0.9f, false);
        const auto result = positionOffsetBirth (sequence.position, birth.time,
            0.001f, 150.0f, 1.0f, 6, {1.0f, 1.0f, 0.0f}, {});
        REQUIRE (result.has_value ());
        requirePosition (*result, birth.position, 0.00004f);
    }
}

TEST_CASE ("Linear sequence retains inclusive endpoints and reflects mirror overshoot",
           "[particle][birthgeometry][sequence]") {
    BetweenControlPointsState state {0.0f, betweenControlPointsStep (3.0f)};
    for (float x : {0.0f, 5.0f, 10.0f, 5.0f, 0.0f, 5.0f}) {
        const auto result = betweenControlPointsBirth ({}, {}, 1.0f, {}, {10, 0, 0},
            state, {0, 1}, true, 0u, 0.3f, {0, 1, 0}, 0.9f, false);
        REQUIRE (result.position.x == x);
    }
    state = {0.9f, 0.2f};
    betweenControlPointsBirth ({}, {}, 1.0f, {}, {10, 0, 0},
        state, {0, 1}, false, 0u, 0.3f, {0, 1, 0}, 0.9f, false);
    REQUIRE (state.phase == 0.0f); // repeat discards rather than wraps overshoot
    state = {0.9f, 0.2f};
    betweenControlPointsBirth ({}, {}, 1.0f, {}, {10, 0, 0},
        state, {0, 1}, true, 0u, 0.3f, {0, 1, 0}, 0.9f, false);
    REQUIRE (state.phase == Catch::Approx (0.9f));
    REQUIRE (state.step == -0.2f);
    REQUIRE (betweenControlPointsStep (0.0f) == 10000.0f);
}

TEST_CASE ("Sequence easing uses unbounded phase and applies independent authored flags",
           "[particle][birthgeometry][sequence]") {
    BetweenControlPointsState state {0.25f, 0.0f};
    const auto result = betweenControlPointsBirth ({2, 4, 6}, {8, 4, 2}, 10.0f,
        {10, 20, 30}, {20, 20, 30}, state, {2, 4}, false, 15u,
        0.3f, {0, 0, 1}, 0.9f, false);
    // phase=.25 gives easing=.75, even though bounds map it to 2.5.
    requirePosition (result.position, {35, 23, 36.75f}, 0.00001f);
    requirePosition (result.velocity, {6, 3, 1.5f}, 0.00001f);
    REQUIRE (result.size == Catch::Approx (7.75f));
    state = {0.25f, 0.0f};
    const auto absolute = betweenControlPointsBirth ({12, 24, 36}, {}, 10,
        {10, 20, 30}, {20, 20, 30}, state, {2, 4}, false, 1u,
        0.3f, {0, 0, 1}, 0.9f, true);
    requirePosition (absolute.position, {35, 23, 34.5f}, 0.00001f);
}

TEST_CASE ("Degenerate endpoints keep finite native behavior and read live endpoints",
           "[particle][birthgeometry][sequence]") {
    BetweenControlPointsState state {0.5f, 0.0f};
    const auto degenerate = betweenControlPointsBirth ({3, 4, 5}, {1, 2, 3}, 4,
        {7, 8, 9}, {7, 8, 9}, state, {0, 1}, false, 0u,
        0.3f, {0, 1, 0}, 0.9f, true);
    requirePosition (degenerate.position, {3, 4, 5}, 0.0f);
    const auto moved = betweenControlPointsBirth ({}, {}, 1, {10, 20, 30},
        {10, 20, 50}, state, {0, 1}, false, 0u, 0.3f, {0, 1, 0}, 0.9f, false);
    requirePosition (moved.position, {10, 20, 40}, 0.0f);
}

TEST_CASE ("Position offset uses signed component blend and bounded octave/domain rules",
           "[particle][birthgeometry][noise]") {
    const glm::vec3 position (20, -30, 40);
    const auto base = positionOffsetBirth (position, 0.2f, 0.01f, 100,
        2.0f, 3, {1, 2, 3}, {});
    const auto signs = positionOffsetBirth (position, 0.2f, 0.01f, 100,
        2.0f, 3, {1, 2, 3}, {1, -1, 0.5f});
    REQUIRE (base.has_value ());
    REQUIRE (signs.has_value ());
    const glm::vec3 displacement = *base - position;
    requirePosition (*signs, position + glm::vec3 (std::abs (displacement.x),
        -std::abs (displacement.y), 0.5f * displacement.z + 0.5f * std::abs (displacement.z)), 0.00002f);
    REQUIRE (birthFractalNoise2 (0.2f, -0.3f, 0) == birthFractalNoise2 (0.2f, -0.3f, 1));
    REQUIRE (birthFractalNoise2 (0.2f, -0.3f, 100) == birthFractalNoise2 (0.2f, -0.3f, 8));
    REQUIRE_FALSE (birthSimplexNoise2 (std::numeric_limits<float>::infinity (), 0));
    REQUIRE_FALSE (birthSimplexNoise2 (0, std::numeric_limits<float>::quiet_NaN ()));
    const auto fixed = positionOffsetBirth (position, 99, 0.01f, 100, 0, 3, {1, 2, 3}, {});
    REQUIRE (fixed == positionOffsetBirth (position, 2, 0.01f, 100, 0, 3, {1, 2, 3}, {}));
}
