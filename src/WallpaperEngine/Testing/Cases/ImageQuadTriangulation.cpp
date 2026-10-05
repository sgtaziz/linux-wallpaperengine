#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Objects/ImagePrelighting.h"
#include "WallpaperEngine/Render/Objects/ImageQuadUV.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Objects;

namespace {

// A vertex as the rasterizer sees it: column and memory row of the render
// target (row 0 is the first row, which both renderers present as the top).
struct TargetVertex {
    glm::vec2 cell;
    glm::vec2 uv;
};

using QuadDraw = std::array<TargetVertex, 6>;

enum class Diagonal { FirstRowFirstColumn, FirstRowLastColumn };

// GL with the default lower-left clip origin: window y == memory row.
glm::vec2 glCell (const glm::mat4& mvp, const glm::vec3& position, const glm::vec2& target) {
    const glm::vec4 clip = mvp * glm::vec4 (position, 1.0f);
    const glm::vec2 ndc = glm::vec2 (clip) / clip.w;
    return {(ndc.x * 0.5f + 0.5f) * target.x, (ndc.y * 0.5f + 0.5f) * target.y};
}

// Native D3D (DXVK viewport height -H): clip +Y lands on memory row 0.
glm::vec2 d3dCell (const glm::vec4& clip, const glm::vec2& target) {
    const glm::vec2 ndc = glm::vec2 (clip) / clip.w;
    return {(ndc.x * 0.5f + 0.5f) * target.x, (0.5f - ndc.y * 0.5f) * target.y};
}

QuadDraw linuxDraw (const std::array<float, 18>& positions, const std::array<float, 12>& uvs,
                    const glm::mat4& mvp, const glm::vec2& target) {
    QuadDraw draw {};
    for (size_t vertex = 0; vertex < 6; ++vertex) {
        const glm::vec3 position {positions[vertex * 3], positions[vertex * 3 + 1], positions[vertex * 3 + 2]};
        draw[vertex] = {glCell (mvp, position, target), {uvs[vertex * 2], uvs[vertex * 2 + 1]}};
    }
    return draw;
}

// The two vertices both triangles of a 6-vertex triangle list contain.
std::array<TargetVertex, 2> sharedEdge (const QuadDraw& draw) {
    std::vector<TargetVertex> shared;
    for (size_t first = 0; first < 3; ++first)
        for (size_t second = 3; second < 6; ++second)
            if (draw[first].cell == draw[second].cell && draw[first].uv == draw[second].uv) {
                if (std::none_of (shared.begin (), shared.end (), [&] (const TargetVertex& known) {
                        return known.cell == draw[first].cell;
                    }))
                    shared.push_back (draw[first]);
            }
    REQUIRE (shared.size () == 2);
    return {shared[0], shared[1]};
}

Diagonal classify (const std::array<TargetVertex, 2>& edge, const QuadDraw& draw) {
    float minimumColumn = draw[0].cell.x;
    float minimumRow = draw[0].cell.y;
    for (const auto& vertex : draw) {
        minimumColumn = std::min (minimumColumn, vertex.cell.x);
        minimumRow = std::min (minimumRow, vertex.cell.y);
    }
    // Both endpoints are opposite corners; one suffices.
    const bool firstColumn = edge[0].cell.x == minimumColumn;
    const bool firstRow = edge[0].cell.y == minimumRow;
    return firstColumn == firstRow ? Diagonal::FirstRowFirstColumn : Diagonal::FirstRowLastColumn;
}

float windowArea (const TargetVertex& a, const TargetVertex& b, const TargetVertex& c) {
    const glm::vec2 ab = b.cell - a.cell;
    const glm::vec2 ac = c.cell - a.cell;
    return ab.x * ac.y - ab.y * ac.x;
}

// Native reference: vertices v0..v3 with its 16-bit index list
// 0x20000, 0x10001, 0x30002, at the post-VS clip positions RenderDoc
// recorded (w06-res-native-quads-v1.json, events 55 and 100).
Diagonal nativeDiagonal (const std::array<glm::vec4, 4>& postVs, const glm::vec2& target) {
    constexpr std::array<size_t, 6> indices {0, 2, 1, 1, 2, 3};
    QuadDraw draw {};
    for (size_t vertex = 0; vertex < 6; ++vertex)
        draw[vertex] = {d3dCell (postVs[indices[vertex]], target), glm::vec2 (static_cast<float> (indices[vertex]))};
    return classify (sharedEdge (draw), draw);
}

const std::array<glm::vec4, 4> nativeFboEvent55 {
    glm::vec4 (-1, 1, 0.5f, 1), glm::vec4 (1, 1, 0.5f, 1), glm::vec4 (-1, -1, 0.5f, 1), glm::vec4 (1, -1, 0.5f, 1)};
const std::array<glm::vec4, 4> nativeSceneEvent100 {
    glm::vec4 (-1, 1.7777778f, 0.5f, 1), glm::vec4 (1, 1.7777778f, 0.5f, 1),
    glm::vec4 (-1, -1.7777778f, 0.5f, 1), glm::vec4 (1, -1.7777778f, 0.5f, 1)};

struct ExpectedUV {
    float left;
    float firstRow; // V of the source row sampled on the target's first row.
    float right;
    float lastRow;
};

// Every Linux image quad must split like native in target memory, keep its
// corner UVs, keep the image's content diagonal native's TR-BL when V is not
// presentation-reflected, and stay front-facing (counter-clockwise) in GL.
void requireNativeSplit (const std::string& name, const QuadDraw& draw, const ExpectedUV& uv,
                         Diagonal native, bool reflectedV) {
    INFO (name);
    const auto edge = sharedEdge (draw);
    REQUIRE (classify (edge, draw) == native);

    float minimumColumn = draw[0].cell.x, maximumColumn = draw[0].cell.x;
    float minimumRow = draw[0].cell.y, maximumRow = draw[0].cell.y;
    for (const auto& vertex : draw) {
        minimumColumn = std::min (minimumColumn, vertex.cell.x);
        maximumColumn = std::max (maximumColumn, vertex.cell.x);
        minimumRow = std::min (minimumRow, vertex.cell.y);
        maximumRow = std::max (maximumRow, vertex.cell.y);
    }
    REQUIRE (minimumColumn < maximumColumn);
    REQUIRE (minimumRow < maximumRow);
    for (const auto& vertex : draw) {
        REQUIRE ((vertex.cell.x == minimumColumn || vertex.cell.x == maximumColumn));
        REQUIRE ((vertex.cell.y == minimumRow || vertex.cell.y == maximumRow));
        REQUIRE (vertex.uv.x == (vertex.cell.x == minimumColumn ? uv.left : uv.right));
        REQUIRE (vertex.uv.y == (vertex.cell.y == minimumRow ? uv.firstRow : uv.lastRow));
    }

    if (!reflectedV) {
        // Content corners: native TR is (right, first source row), BL is
        // (left, last source row) whatever the transform.
        const float topV = std::min (uv.firstRow, uv.lastRow);
        const float bottomV = std::max (uv.firstRow, uv.lastRow);
        const glm::vec2 contentTopRight {uv.right, topV};
        const glm::vec2 contentBottomLeft {uv.left, bottomV};
        REQUIRE (((edge[0].uv == contentTopRight && edge[1].uv == contentBottomLeft)
                  || (edge[1].uv == contentTopRight && edge[0].uv == contentBottomLeft)));
    }

    REQUIRE (windowArea (draw[0], draw[1], draw[2]) > 0.0f);
    REQUIRE (windowArea (draw[3], draw[4], draw[5]) > 0.0f);
}

std::array<float, 18> sceneQuad (const glm::mat4& authored, const glm::vec2& size, const glm::vec2& scene,
                                 bool orthographic) {
    const glm::vec2 half = size * 0.5f;
    const glm::vec2 local[4] {{-half.x, -half.y}, {-half.x, half.y}, {half.x, -half.y}, {half.x, half.y}};
    glm::vec3 corners[4];
    for (int corner = 0; corner < 4; ++corner)
        corners[corner] = Wallpapers::scenePointForCamera (
            glm::vec3 (authored * glm::vec4 (local[corner], 0.0f, 1.0f)), scene.x, scene.y, orthographic);
    return imageLocalQuadPositions (corners);
}

} // namespace

TEST_CASE ("Native image quads split along the BL-TR memory diagonal", "[render][image-quad-split]") {
    const glm::vec2 fbo {256.0f, 256.0f};
    const glm::vec2 screen {1280.0f, 720.0f};
    const Diagonal nativeFbo = nativeDiagonal (nativeFboEvent55, fbo);
    const Diagonal nativeScene = nativeDiagonal (nativeSceneEvent100, screen);
    REQUIRE (nativeFbo == Diagonal::FirstRowLastColumn);
    REQUIRE (nativeScene == Diagonal::FirstRowLastColumn);

    const glm::vec2 image {256.0f, 128.0f};
    const glm::vec2 allocation {256.0f, 128.0f};
    const auto direct = directImageQuadUV (1, 1, 256, 128, false, false, false, false);
    const auto composite = compositeImageQuadUV (256, 128, false, false, false);
    const auto ortho = Camera::makeOrthogonalProjectionForScene (screen.x, screen.y, 0.0f, 10000.0f, glm::vec3 (0.0f))
        * glm::lookAt (glm::vec3 (0, 0, 1), glm::vec3 (0), glm::vec3 (0, 1, 0));
    const glm::mat4 placed = glm::translate (glm::mat4 (1.0f), glm::vec3 (500.0f, 300.0f, 0.0f));

    // Direct and composite image quads into the orthographic scene target.
    const auto scenePositions = sceneQuad (placed, image, screen, true);
    requireNativeSplit ("direct scene quad", linuxDraw (scenePositions, direct.triangles (), ortho, screen),
                        {direct.left, direct.bottom, direct.right, direct.top}, nativeScene, false);
    requireNativeSplit ("composite scene quad", linuxDraw (scenePositions, composite.triangles (), ortho, screen),
                        {composite.left, composite.bottom, composite.right, composite.top}, nativeScene, false);

    // First (copy) pass into the layer's own target, and an effect pass.
    const auto copyUV = imageQuadTexcoords (0.0f, 0.0f, 31.0f / 32.0f, 19.0f / 32.0f);
    requireNativeSplit ("copy into image target",
                        linuxDraw (imageTargetQuadPositions (0.0f, 0.0f, allocation.x, allocation.y), copyUV,
                                   imageTargetProjection (allocation), allocation),
                        {0.0f, 0.0f, 31.0f / 32.0f, 19.0f / 32.0f}, nativeFbo, false);
    requireNativeSplit ("effect pass", linuxDraw (imagePassQuadPositions (), imageQuadTexcoords (0, 0, 1, 1),
                                                  glm::mat4 (1.0f), fbo),
                        {0, 0, 1, 1}, nativeFbo, false);
    requireNativeSplit ("fullscreen clip-space copy",
                        linuxDraw (imageTargetQuadPositions (-1.0f, -1.0f, 1.0f, 1.0f),
                                   imageQuadTexcoords (0, 0, 1, 1), glm::mat4 (1.0f), screen),
                        {0, 0, 1, 1}, nativeScene, false);

    // Passthrough copies sample the scene through scene-space corners.
    requireNativeSplit ("passthrough copy", linuxDraw (scenePositions, imageQuadTexcoords (0, 0, 1, 1), ortho, screen),
                        {0, 0, 1, 1}, nativeScene, false);

    // Prelighting rasterizes local source pixels into the image target.
    const auto prelighting = imagePrelightingBasis (placed, image, allocation, false);
    requireNativeSplit ("prelighting local quad",
                        linuxDraw (imageLocalQuadPositions (allocation * 0.5f), copyUV,
                                   prelighting.targetProjection, allocation),
                        {0.0f, 0.0f, 31.0f / 32.0f, 19.0f / 32.0f}, nativeFbo, false);

    // Lighting finals draw local corners through the authored model.
    const auto lighting = ortho * Wallpapers::sceneAuthoredToCamera (screen.x, screen.y, true) * placed;
    requireNativeSplit ("lighting local quad",
                        linuxDraw (imageLocalQuadPositions (image * 0.5f), direct.triangles (), lighting, screen),
                        {direct.left, direct.bottom, direct.right, direct.top}, nativeScene, false);
}

TEST_CASE ("Perspective presentation keeps native's memory split", "[render][image-quad-split]") {
    // The perspective scene projection reflects clip Y for the GL present,
    // and the final intermediate-texture quad reflects V to match. Corners
    // still land where native's do, so the memory split must not invert.
    const glm::vec2 screen {1280.0f, 720.0f};
    const Diagonal nativeScene = nativeDiagonal (nativeSceneEvent100, screen);
    const auto perspective = Camera::makePerspectiveProjectionForScene (screen.x, screen.y, 50.0f, 0.01f, 10000.0f)
        * glm::lookAt (glm::vec3 (0, 0, 1000), glm::vec3 (0), glm::vec3 (0, 1, 0));
    const auto positions = sceneQuad (glm::mat4 (1.0f), {256.0f, 128.0f}, screen, false);

    const auto composite = compositeImageQuadUV (256, 128, false, false, false);
    auto presented = composite.triangles ();
    for (size_t index = 1; index < presented.size (); index += 2) presented[index] = 1.0f - presented[index];
    requireNativeSplit ("presented composite", linuxDraw (positions, presented, perspective, screen),
                        {composite.left, 1.0f - composite.bottom, composite.right, 1.0f - composite.top},
                        nativeScene, true);
    requireNativeSplit ("presented pass", linuxDraw (positions, imageQuadTexcoords (0, 1, 1, 0), perspective, screen),
                        {0, 1, 1, 0}, nativeScene, true);
    // Unreflected direct quads under perspective keep the content split too.
    const auto direct = directImageQuadUV (1, 1, 256, 128, false, false, false, false);
    requireNativeSplit ("perspective direct", linuxDraw (positions, direct.triangles (), perspective, screen),
                        {direct.left, direct.bottom, direct.right, direct.top}, nativeScene, false);
}

TEST_CASE ("Mirrored and rotated layers split on native's local diagonal", "[render][image-quad-split]") {
    // Native indexes local corners, so a transform moves the diagonal with
    // the content: it always joins content TR and BL, never a fixed screen pair.
    const glm::vec2 screen {1280.0f, 720.0f};
    const auto ortho = Camera::makeOrthogonalProjectionForScene (screen.x, screen.y, 0.0f, 10000.0f, glm::vec3 (0.0f))
        * glm::lookAt (glm::vec3 (0, 0, 1), glm::vec3 (0), glm::vec3 (0, 1, 0));
    const auto direct = directImageQuadUV (1, 1, 64, 64, false, false, false, false);
    const auto uv = direct.triangles ();
    const glm::vec2 contentTopRight {direct.right, direct.bottom};
    const glm::vec2 contentBottomLeft {direct.left, direct.top};

    for (const auto& [name, transform] : std::array<std::pair<const char*, glm::mat4>, 3> {{
             {"mirrored x", glm::scale (glm::translate (glm::mat4 (1), glm::vec3 (640, 360, 0)), glm::vec3 (-1, 1, 1))},
             {"rotated 90", glm::rotate (glm::translate (glm::mat4 (1), glm::vec3 (640, 360, 0)),
                                         glm::radians (90.0f), glm::vec3 (0, 0, 1))},
             {"rotated 180", glm::rotate (glm::translate (glm::mat4 (1), glm::vec3 (640, 360, 0)),
                                          glm::radians (180.0f), glm::vec3 (0, 0, 1))},
         }}) {
        INFO (name);
        const auto draw = linuxDraw (sceneQuad (transform, {64.0f, 64.0f}, screen, true), uv, ortho, screen);
        const auto edge = sharedEdge (draw);
        REQUIRE (((edge[0].uv == contentTopRight && edge[1].uv == contentBottomLeft)
                  || (edge[1].uv == contentTopRight && edge[0].uv == contentBottomLeft)));
    }
}
