#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Scripting/Adapters/VectorSubtraction.h"

using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Scripting::Adapters::subtractVectorValue;

TEST_CASE ("SceneScript subtraction matches captured native scalar and vector results",
           "[script][vector]") {
    // Literal results from the native 2.8 numeric readout fixture. Asymmetric
    // components and negative scalar inputs expose operand reversal.
    const glm::vec2 two (10, 20);
    const glm::vec3 three (10, 20, 30);
    const glm::vec4 four (10, 20, 30, 40);
    REQUIRE (subtractVectorValue (two, glm::vec2 (2, 5)) == glm::vec2 (8, 15));
    REQUIRE (subtractVectorValue (two, glm::vec2 (2)) == glm::vec2 (8, 18));
    REQUIRE (subtractVectorValue (three, glm::vec3 (2, 5, 7)) == glm::vec3 (8, 15, 23));
    REQUIRE (subtractVectorValue (three, glm::vec3 (2)) == glm::vec3 (8, 18, 28));
    REQUIRE (subtractVectorValue (three, glm::vec3 (-3)) == glm::vec3 (13, 23, 33));
    REQUIRE (subtractVectorValue (four, glm::vec4 (2, 5, 7, 9)) == glm::vec4 (8, 15, 23, 31));
    REQUIRE (subtractVectorValue (four, glm::vec4 (2)) == glm::vec4 (8, 18, 28, 38));
}

TEST_CASE ("SceneScript subtraction returns a detached result and preserves a Vec3 Vec2 z",
           "[script][vector][lifetime]") {
    DynamicValue live (glm::vec3 (10, 20, 30));
    const glm::vec3 snapshot = live.getVec3 ();
    // The actual Vec2 argument bridge supplies zero for the untouched z.
    DynamicValue result (subtractVectorValue (snapshot, glm::vec3 (2, 5, 0)));
    REQUIRE (result.getVec3 () == glm::vec3 (8, 15, 30));
    live.update (glm::vec3 (100, 200, 300), DynamicValue::UpdateSource::Script);
    REQUIRE (snapshot == glm::vec3 (10, 20, 30));
    REQUIRE (result.getVec3 () == glm::vec3 (8, 15, 30));
    result.update (glm::vec3 (0), DynamicValue::UpdateSource::Script);
    REQUIRE (live.getVec3 () == glm::vec3 (100, 200, 300));
}
