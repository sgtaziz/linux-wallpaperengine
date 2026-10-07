#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "WallpaperEngine/Render/Objects/TextEffectQuad.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

using namespace WallpaperEngine::Render::Objects;
using Catch::Matchers::WithinAbs;

namespace {
glm::vec2 project (const glm::mat4& m, const std::array<float, 18>& quad, size_t vertex, float yOffsetPx = 0.0f) {
    const glm::vec4 p = m * glm::vec4 (quad[vertex * 3], quad[vertex * 3 + 1] + yOffsetPx, 0.0f, 1.0f);
    return {p.x / p.w, p.y / p.w};
}
} // namespace

TEST_CASE ("text effect final step draws the target-pixel box with the layer matrix", "[text][effect]") {
    const glm::vec2 size (134.0f, 114.0f);
    const glm::vec2 center (3.0f, -5.0f);
    const auto quad = textEffectFinalPositions (size);
    // Same triangles/UVs as the 2x2 clip quad: vertex 0 has v=1 (target bottom
    // row side) at local -Y, vertex 1 has v=0 (first row) at local +Y.
    CHECK (quad[0] == -67.0f);
    CHECK (quad[1] == -57.0f);
    CHECK (quad[4] == 57.0f);
    CHECK (quad[15] == 67.0f);
    CHECK (quad[16] == 57.0f);

    // Layer matrix: any scene projection, here 2 px per unit scale plus an offset.
    const glm::mat4 layer = glm::translate (glm::mat4 (1.0f), glm::vec3 (10.0f, 20.0f, 0.0f))
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (2.0f, 2.0f, 1.0f));
    const auto mvp = textEffectFinalMatrix (layer, center);
    const auto c0 = project (mvp, quad, 0);
    CHECK_THAT (c0.x, WithinAbs (10.0 + 2.0 * (3.0 - 67.0), 1e-4));
    CHECK_THAT (c0.y, WithinAbs (20.0 + 2.0 * (-5.0 - 57.0), 1e-4));

    // Skew MODE 1 adds 0.1 * g_Texture0Resolution.w pixels to a_Position.y: in
    // layer space this is the same 11.4 px (scaled only by the layer scale),
    // not 11.4 clip units as with the 2x2 quad.
    const float shift = 0.1f * size.y;
    const auto shifted = project (mvp, quad, 2, shift);
    const auto base = project (mvp, quad, 2);
    CHECK_THAT (shifted.y - base.y, WithinAbs (2.0f * shift, 1e-4));
    CHECK_THAT (shifted.x - base.x, WithinAbs (0.0, 1e-5));
}
