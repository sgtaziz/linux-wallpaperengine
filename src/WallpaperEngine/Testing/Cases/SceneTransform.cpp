#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Render/Objects/ParticleBirthGeometry.h"
#include "WallpaperEngine/Render/WallpaperState.h"
#include "WallpaperEngine/Render/Camera.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <memory>
#include <vector>

using WallpaperEngine::Data::Builders::UserSettingBuilder;
using WallpaperEngine::Data::Model::Object;
using WallpaperEngine::Data::Model::ObjectData;
using WallpaperEngine::Render::Wallpapers::resolveSceneTransform;
using WallpaperEngine::Render::Wallpapers::inverseFiniteTransform;
using WallpaperEngine::Render::Wallpapers::projectWorldControlPoint;
using WallpaperEngine::Render::Wallpapers::sceneLayerParallaxOffset;
using WallpaperEngine::Render::Wallpapers::sceneParallaxDelayWeight;
using WallpaperEngine::Render::Wallpapers::sceneParallaxDepth;
using WallpaperEngine::Render::Wallpapers::sceneParallaxRoot;
using WallpaperEngine::Render::Wallpapers::sceneCompositionOwner;
using WallpaperEngine::Render::Wallpapers::sceneCompositionSuppressesParallax;
using WallpaperEngine::Render::Wallpapers::sceneCompositionChildProjection;
using WallpaperEngine::Render::Wallpapers::sceneCompositionChildScope;

namespace {
ObjectData node (int id, std::optional<int> parent, glm::vec3 origin, glm::vec3 scale,
                 float angle, bool visible = true) {
    ObjectData result {};
    result.id = id;
    result.parent = parent;
    result.origin = UserSettingBuilder::fromValue (origin);
    result.groupScale = UserSettingBuilder::fromValue (scale);
    result.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (0, 0, angle));
    result.groupVisible = UserSettingBuilder::fromValue (visible);
    return result;
}
}

TEST_CASE ("Output-sized scene projection preserves fit fill and stretch without a second crop",
           "[scene][presentation]") {
    using WallpaperEngine::Render::WallpaperState;
    using Mode = WallpaperState::TextureUVsScaling;
    using WallpaperEngine::Render::Wallpapers::scenePresentationClipTransform;
    for (const auto output : {glm::ivec2 (1280, 720), glm::ivec2 (720, 960)}) {
        for (const auto mode : {Mode::ZoomFitUVs, Mode::ZoomFillUVs, Mode::StretchUVs}) {
            CAPTURE (output.x, output.y, mode);
            WallpaperState state (mode, 0);
            state.updateState ({0, 0, output.x, output.y}, false, 320, 240);
            const auto uv = state.getTextureUVs ();
            const auto crop = scenePresentationClipTransform (
                {uv.ustart, uv.uend, uv.vstart, uv.vend});
            const auto authoredProjection = glm::ortho (-160.0f, 160.0f, -120.0f, 120.0f);
            const auto screen = [&] (const glm::vec2& point) {
                const glm::vec4 clip = crop * authoredProjection * glm::vec4 (point, 0.0f, 1.0f);
                return (glm::vec2 (clip) * 0.5f + 0.5f) * glm::vec2 (output);
            };
            const glm::vec2 center = screen ({0.0f, 0.0f});
            REQUIRE (center.x == Catch::Approx (output.x * 0.5f));
            REQUIRE (center.y == Catch::Approx (output.y * 0.5f));
            const float scaleX = output.x / 320.0f;
            const float scaleY = output.y / 240.0f;
            const float uniformScale = mode == Mode::ZoomFitUVs
                ? std::min (scaleX, scaleY) : std::max (scaleX, scaleY);
            const glm::vec2 expectedSize = mode == Mode::StretchUVs
                ? glm::vec2 (output) : glm::vec2 (320.0f, 240.0f) * uniformScale;
            const glm::vec2 displayedSize = screen ({160.0f, 120.0f}) - screen ({-160.0f, -120.0f});
            REQUIRE (displayedSize.x == Catch::Approx (expectedSize.x).margin (0.001f));
            REQUIRE (displayedSize.y == Catch::Approx (expectedSize.y).margin (0.001f));
            // Final root presentation is a one-to-one copy: cropping has
            // already happened in the projection, with depth untouched.
            REQUIRE ((crop * glm::vec4 (0, 0, 0.37f, 1)).z == Catch::Approx (0.37f));
        }
    }
}

TEST_CASE ("Native point-filter GIF keeps authored pixels at integer output scale",
           "[scene][presentation]") {
    using WallpaperEngine::Render::WallpaperState;
    using WallpaperEngine::Render::Wallpapers::scenePresentationClipTransform;
    // Actual 843530721 is a 500x291 GIF with TEXI flags7 (nearest),
    // centered at (250,145.5). Native1000x582 captures repeat each pixel
    // exactly2x2. The source sampler remains nearest; no sampler override
    // is required when the root is rendered at its output dimensions.
    WallpaperState state (WallpaperState::TextureUVsScaling::ZoomFillUVs, 0);
    state.updateState ({0, 0, 1000, 582}, false, 500, 291);
    const auto uv = state.getTextureUVs ();
    const auto projection = scenePresentationClipTransform (
        {uv.ustart, uv.uend, uv.vstart, uv.vend})
        * glm::ortho (-250.0f, 250.0f, -145.5f, 145.5f);
    const auto clip = [&] (float x) { return projection * glm::vec4 (x, 0, 0, 1); };
    REQUIRE ((clip (1).x - clip (0).x) * 500.0f == Catch::Approx (2.0f));
    REQUIRE (projection[1][1] * 291.0f == Catch::Approx (2.0f));
    // Invalid/minimized windows do not introduce a singular clip matrix.
    REQUIRE (scenePresentationClipTransform ({0, 0, 0, 0}) == glm::mat4 (1.0f));
}

TEST_CASE ("Scene parent matrices preserve rotated nonuniform scale and live visibility", "[scene][transform]") {
    Object grandparent (node (1, {}, {10, 20, 0}, {-2, 1, 1}, 0));
    Object parent (node (2, 1, {3, 4, 0}, {3, 1, 1}, glm::half_pi<float> ()));
    Object child (node (3, 2, {5, 0, 0}, {1, 3, 1}, glm::quarter_pi<float> ()));
    std::map<int, const Object*> nodes {{1, &grandparent}, {2, &parent}, {3, &child}};
    auto resolve = [&] {
        return resolveSceneTransform (child, [&] (int id) -> const Object* {
            const auto it = nodes.find (id);
            return it == nodes.end () ? nullptr : it->second;
        });
    };

    auto result = resolve ();
    REQUIRE (result.visible);
    const glm::vec4 origin = result.authoredMatrix * glm::vec4 (0, 0, 0, 1);
    const glm::vec4 basisX = result.authoredMatrix * glm::vec4 (1, 0, 0, 1);
    const glm::vec4 basisY = result.authoredMatrix * glm::vec4 (0, 1, 0, 1);
    const float rootTwo = std::sqrt (2.0f);
    // Hand-composed G*T(parent)*R90*S(3,1)*T(child)*R45*S(1,3).
    REQUIRE (origin.x == Catch::Approx (4.0f));
    REQUIRE (origin.y == Catch::Approx (39.0f));
    REQUIRE (basisX.x == Catch::Approx (4.0f + rootTwo));
    REQUIRE (basisX.y == Catch::Approx (39.0f + 1.5f * rootTwo));
    REQUIRE (basisY.x == Catch::Approx (4.0f + 3.0f * rootTwo));
    REQUIRE (basisY.y == Catch::Approx (39.0f - 4.5f * rootTwo));
    const glm::vec4 localPoint {2, -1, 0, 1};
    const glm::vec4 recovered = glm::inverse (result.authoredMatrix) * (result.authoredMatrix * localPoint);
    REQUIRE (recovered.x == Catch::Approx (localPoint.x));
    REQUIRE (recovered.y == Catch::Approx (localPoint.y));

    parent.groupVisible->value->update (false, WallpaperEngine::Data::Model::DynamicValue::Script);
    REQUIRE_FALSE (resolve ().visible);
    parent.groupVisible->value->update (true, WallpaperEngine::Data::Model::DynamicValue::Script);
    parent.origin->value->update (glm::vec3 (6, 4, 0), WallpaperEngine::Data::Model::DynamicValue::Script);
    const auto moved = resolve ();
    REQUIRE (moved.visible);
    REQUIRE (glm::distance (moved.origin, result.origin) > 1.0f);
}

TEST_CASE ("Image and text retain native XYZ basis and typed transform values", "[scene][transform][image][text]") {
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Render::Wallpapers::localSceneTransform;
    const glm::vec3 angles {glm::pi<float> () / 6, glm::pi<float> () / 4, glm::pi<float> () / 3};
    const glm::vec3 scale {-2.0f, 3.0f, 0.5f};
    ImageData imageData {};
    imageData.angles = UserSettingBuilder::fromValue (angles);
    imageData.scale = UserSettingBuilder::fromValue (scale);
    imageData.visible = UserSettingBuilder::fromValue (true);
    Image image (node (1, {}, {4, 5, 6}, {9, 9, 9}, 0), std::move (imageData));
    TextData textData {};
    textData.scale = UserSettingBuilder::fromValue (scale);
    textData.visible = UserSettingBuilder::fromValue (true);
    auto textBase = node (2, {}, {4, 5, 6}, {9, 9, 9}, 0);
    textBase.groupAngles = UserSettingBuilder::fromValue (angles);
    Text text (std::move (textBase), std::move (textData));
    // Native's coefficients at X=30,Y=45,Z=60 degrees,
    // 's negative/nonuniform column scaling. These values
    // distinguish all three rotations, their order and typed scale authority.
    for (const Object* object : {static_cast<Object*> (&image), static_cast<Object*> (&text)}) {
        const auto matrix = localSceneTransform (*object).authoredMatrix;
        REQUIRE (glm::distance (glm::vec3 (matrix[0]), {-0.70710678f, -1.22474487f, 1.41421356f}) < 1e-5f);
        REQUIRE (glm::distance (glm::vec3 (matrix[1]), {-1.71966991f, 2.21759676f, 1.06066017f}) < 1e-5f);
        REQUIRE (glm::distance (glm::vec3 (matrix[2]), {0.36959946f, 0.14016504f, 0.30618622f}) < 1e-5f);
        REQUIRE (glm::vec3 (matrix[3]) == glm::vec3 (4, 5, 6));
    }
    image.angles->value->update (glm::vec3 (0, 0, glm::quarter_pi<float> ()), DynamicValue::Script);
    text.groupAngles->value->update (glm::vec3 (0, 0, glm::quarter_pi<float> ()), DynamicValue::Script);
    const auto priorZOnly = glm::scale (glm::rotate (glm::translate (glm::mat4 (1), {4, 5, 6}),
        glm::quarter_pi<float> (), {0, 0, 1}), scale);
    REQUIRE (localSceneTransform (image).authoredMatrix == priorZOnly);
    REQUIRE (localSceneTransform (text).authoredMatrix == priorZOnly);
}

TEST_CASE ("XYZ text basis preserves parent composition and horizontal perspective datum", "[scene][transform][text][perspective]") {
    using namespace WallpaperEngine::Data::Model;
    using WallpaperEngine::Render::Wallpapers::localSceneTransform;
    Object parent (node (1, {}, {8, 5, -2}, {1, 2, 3}, 0));
    parent.groupAngles->value->update (glm::vec3 (glm::half_pi<float> (), 0, 0), DynamicValue::Script);
    TextData data {};
    data.scale = UserSettingBuilder::fromValue (glm::vec3 (2, -0.5f, 1));
    data.visible = UserSettingBuilder::fromValue (true);
    auto base = node (2, 1, {1, 2, 3}, {9, 9, 9}, 0);
    base.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (0, glm::half_pi<float> (), 0));
    Text child (std::move (base), std::move (data));
    const auto composed = resolveSceneTransform (child, [&] (int id) -> const Object* {
        return id == 1 ? &parent : nullptr;
    });
    REQUIRE (glm::distance (composed.origin, {9, -4, 2}) < 1e-5f);
    REQUIRE (glm::distance (glm::vec3 (composed.authoredMatrix[0]), {0, 6, 0}) < 1e-5f);
    REQUIRE (glm::distance (glm::vec3 (composed.authoredMatrix[1]), {0, 0, -1}) < 1e-5f);

    // The native original perspective datum has horizontal Date text. Its
    // controlled X/Y-zero derivative becomes diagonal. Project the authored
    // local baseline to guard that consequence rather than only matrix values.
    child.parent.reset ();
    child.origin->value->update (glm::vec3 (3, 3.4f, 3), DynamicValue::Script);
    child.scale->value->update (glm::vec3 (0.007f, 0.006f, 0.0085f), DynamicValue::Script);
    child.groupAngles->value->update (glm::vec3 (-glm::pi<float> () / 6, glm::quarter_pi<float> (), 0), DynamicValue::Script);
    const glm::vec3 eye (10.63064f);
    const auto vp = glm::perspective (glm::radians (45.0f), 1280.0f / 720.0f, 0.1f, 10000.0f)
        * glm::lookAt (eye, eye - glm::vec3 (2), glm::vec3 (0, 1, 0));
    const auto baselineDelta = [&] {
        const auto matrix = vp * localSceneTransform (child).authoredMatrix;
        const auto left = matrix * glm::vec4 (-100, 0, 0, 1);
        const auto right = matrix * glm::vec4 (100, 0, 0, 1);
        return glm::vec2 (right) / right.w - glm::vec2 (left) / left.w;
    };
    REQUIRE (std::abs (baselineDelta ().y) < 1e-6f);
    REQUIRE (baselineDelta ().x > 0.0f);
    child.groupAngles->value->update (glm::vec3 (0), DynamicValue::Script);
    REQUIRE (std::abs (baselineDelta ().y) > 0.05f);
}

namespace {
// Independent reference for native
// in the native canvas (+Y up on screen) with a
// top-origin cursor. Influence and amount are applied as native does.
glm::vec2 nativeCanvasParallaxOffset (glm::vec2 rootOrigin, glm::vec2 rootDepth, glm::vec2 eye,
                                      glm::vec2 size, glm::vec2 cursorTopOrigin,
                                      float amount, float influence) {
    const glm::vec2 cursor (cursorTopOrigin.x, 1.0f - cursorTopOrigin.y);
    const glm::vec2 parallaxPos = size * 0.5f * (1.0f - influence) + size * cursor * influence + eye;
    return (rootOrigin - parallaxPos) * amount * rootDepth;
}

// Linux CScene displacement for the same top-origin cursor.
glm::vec2 linuxDisplacement (glm::vec2 cursorTopOrigin, float amount, float influence) {
    return (cursorTopOrigin - glm::vec2 (0.5f)) * amount * influence;
}

// Screen position (canvas pixels from the left/top edge) of a layer centre
// drawn through Linux's centered camera space plus its parallax offset. The
// final presentation shows camera +Y toward the bottom edge.
glm::vec2 linuxScreenFromTop (glm::vec2 canvasOrigin, glm::vec3 offset, glm::vec2 size) {
    const glm::vec3 camera = WallpaperEngine::Render::Wallpapers::scenePointForCamera (
        glm::vec3 (canvasOrigin, 0.0f), size.x, size.y, true) + offset;
    return {camera.x + size.x * 0.5f, camera.y + size.y * 0.5f};
}
}

TEST_CASE ("Layer parallax reproduces the controlled native capture", "[scene][parallax]") {
    // parallax-default-v1: ortho 1920x1080, eye 0, amount 0.25, delay 0.
    // Native run1 (influence 0): A(480,540) depth 1,1 -> x 359.25;
    // C(960,270) depth 1,1 -> y-from-top 876.75; B depth 0,0 stays at 1440.
    const glm::vec2 size {1920.0f, 1080.0f};
    const auto offset = [&] (glm::vec2 origin, glm::vec2 depth, glm::vec2 cursor, float influence) {
        return sceneLayerParallaxOffset (glm::vec3 (origin, 0.0f), glm::vec3 (0.0f), depth,
                                         linuxDisplacement (cursor, 0.25f, influence),
                                         size.x, size.y, 0.25f, true, true);
    };
    const glm::vec2 centre {0.5f};
    const auto a = linuxScreenFromTop ({480, 540}, offset ({480, 540}, {1, 1}, centre, 0.0f), size);
    const auto b = linuxScreenFromTop ({1440, 540}, offset ({1440, 540}, {0, 0}, centre, 0.0f), size);
    const auto c = linuxScreenFromTop ({960, 270}, offset ({960, 270}, {1, 1}, centre, 0.0f), size);
    REQUIRE (a.x == Catch::Approx (359.25f).margin (2.0f));
    REQUIRE (a.y == Catch::Approx (539.25f).margin (2.0f));
    REQUIRE (b.x == Catch::Approx (1439.25f).margin (2.0f));
    REQUIRE (c.x == Catch::Approx (959.25f).margin (2.0f));
    // The static Y term moves C toward the bottom edge, as native does.
    REQUIRE (c.y == Catch::Approx (876.75f).margin (2.0f));

    // Native run2 (influence 1): cursor centre -> 0.8125 moves every depth-1
    // layer by -150 px and leaves depth 0 fixed.
    const glm::vec2 right {0.8125f, 0.5f};
    REQUIRE ((offset ({480, 540}, {1, 1}, right, 1.0f) - offset ({480, 540}, {1, 1}, centre, 1.0f)).x
             == Catch::Approx (-150.0f).margin (0.01f));
    REQUIRE ((offset ({960, 270}, {1, 1}, right, 1.0f) - offset ({960, 270}, {1, 1}, centre, 1.0f)).x
             == Catch::Approx (-150.0f).margin (0.01f));
    REQUIRE (offset ({1440, 540}, {0, 0}, right, 1.0f) == glm::vec3 (0.0f));
    // Native run2 stationary pointer (0.01, 0.01): a cursor near the top
    // moves C down the screen; native shows it clipped at the bottom edge.
    const auto top = linuxScreenFromTop ({960, 270}, offset ({960, 270}, {1, 1}, {0.01f, 0.01f}, 1.0f), size);
    REQUIRE (top.y - c.y == Catch::Approx (132.3f).margin (0.01f));
    REQUIRE (top.y + 80.0f > 1080.0f);
}

TEST_CASE ("Layer parallax matches the native canvas formula for both axes and terms", "[scene][parallax]") {
    // Linux camera space negates the canvas Y axis; X is shared. The cursor Y
    // term flips twice (top-origin pointer and camera axis) while the static
    // origin term flips once, so only a sign-exact conversion matches native.
    const glm::vec2 size {3840.0f, 2160.0f};
    for (const glm::vec2 origin : {glm::vec2 (480, 1700), glm::vec2 (3500, 120), glm::vec2 (1920, 1080)})
        for (const glm::vec2 depth : {glm::vec2 (1, 1), glm::vec2 (-1, 0.5f), glm::vec2 (0.1f, -0.2f)})
            for (const glm::vec2 eye : {glm::vec2 (0), glm::vec2 (-103.6f, 120.9f)})
                for (const glm::vec2 cursor : {glm::vec2 (0.5f), glm::vec2 (0.0f), glm::vec2 (1.0f, 0.25f)})
                    for (const float influence : {0.0f, 0.14f, 1.0f}) {
                        const glm::vec2 native = nativeCanvasParallaxOffset (
                            origin, depth, eye, size, cursor, 0.1f, influence);
                        const glm::vec3 camera = sceneLayerParallaxOffset (
                            glm::vec3 (origin, 0.0f), glm::vec3 (eye, 0.0f), depth,
                            linuxDisplacement (cursor, 0.1f, influence), size.x, size.y, 0.1f, true, true);
                        REQUIRE (camera.x == Catch::Approx (native.x).margin (1e-3f));
                        REQUIRE (camera.y == Catch::Approx (-native.y).margin (1e-3f));
                        REQUIRE (camera.z == 0.0f);
                    }
}

TEST_CASE ("Layer parallax applies only to orthographic camera-parallax scenes", "[scene][parallax]") {
    // Native requires scene flags & 0x108 == 0x108: cameraparallax
    // (0x100) and orthographic projection (0x8). Otherwise no object moves.
    const glm::vec3 origin {100.0f, 900.0f, 0.0f};
    const glm::vec2 displacement {0.1f, -0.1f};
    const auto offset = [&] (bool parallax, bool orthographic) {
        return sceneLayerParallaxOffset (origin, glm::vec3 (0.0f), {1, 1}, displacement,
                                         1920.0f, 1080.0f, 0.5f, parallax, orthographic);
    };
    REQUIRE (offset (true, true) != glm::vec3 (0.0f));
    REQUIRE (offset (false, true) == glm::vec3 (0.0f));
    REQUIRE (offset (true, false) == glm::vec3 (0.0f));
    REQUIRE (offset (false, false) == glm::vec3 (0.0f));
}

TEST_CASE ("Perspective parallax preserves world visibility with nonzero authored depth", "[scene][parallax][perspective]") {
    // A valid perspective object near the world origin would otherwise be
    // displaced hundreds of scene units by the output's pixel dimensions.
    const glm::vec3 origin {3.0f, 3.4f, 3.0f};
    const glm::vec3 eye {10.63064f};
    const auto view = glm::lookAt (eye, eye - glm::vec3 (2.0f), glm::vec3 (0, 1, 0));
    const auto projection = glm::perspective (glm::radians (45.0f), 1280.0f / 720.0f, 0.1f, 10000.0f);
    const auto visible = [&] (glm::vec3 offset) {
        const auto clip = projection * view * glm::vec4 (origin + offset, 1.0f);
        return clip.w > 0.0f && std::abs (clip.x) < clip.w && std::abs (clip.y) < clip.w;
    };
    REQUIRE (visible ({}));
    for (const auto displacement : {glm::vec2 (0.0f), glm::vec2 (0.1f, -0.1f)}) {
        const auto perspective = sceneLayerParallaxOffset (
            origin, eye, {1.0f, 1.0f}, displacement, 1280.0f, 720.0f, 0.5f, true, false);
        REQUIRE (perspective == glm::vec3 (0.0f));
        REQUIRE (visible (perspective));
        REQUIRE_FALSE (visible (sceneLayerParallaxOffset (
            origin, eye, {1.0f, 1.0f}, displacement, 1280.0f, 720.0f, 0.5f, true, true)));
    }
}

TEST_CASE ("Parented layers take parallax from the top-most ancestor", "[scene][parallax][parent]") {
    // Native walks to the root of any type and reads only
    // the root's origin and depth; the child's own values
    // are ignored. Absent depth stays default (1,1).
    Object group (node (1, {}, {300, 200, 0}, {1, 1, 1}, 0));
    group.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.5f, 2.0f));
    Object middle (node (2, 1, {50, 60, 0}, {2, 2, 1}, 0.3f));
    middle.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (-1.0f));
    Object child (node (3, 2, {1700, 1000, 0}, {1, 1, 1}, 0));
    child.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    const std::map<int, const Object*> objects {{1, &group}, {2, &middle}, {3, &child}};
    const auto find = [&] (int id) -> const Object* {
        const auto it = objects.find (id);
        return it == objects.end () ? nullptr : it->second;
    };
    const glm::vec2 cursor {0.75f, 0.4f};
    const auto offset = [&] (const Object& object) {
        const auto& root = sceneParallaxRoot (object, find, objects.size ());
        return sceneLayerParallaxOffset (root.origin->value->getVec3 (), glm::vec3 (0.0f),
                                         sceneParallaxDepth (root), linuxDisplacement (cursor, 0.2f, 1.0f),
                                         1920.0f, 1080.0f, 0.2f, true, true);
    };
    REQUIRE (&sceneParallaxRoot (child, find, objects.size ()) == &group);
    REQUIRE (&sceneParallaxRoot (middle, find, objects.size ()) == &group);
    const glm::vec2 native = nativeCanvasParallaxOffset (
        {300, 200}, {0.5f, 2.0f}, {0, 0}, {1920, 1080}, cursor, 0.2f, 1.0f);
    for (const Object* object : {&group, &middle, &child}) {
        const auto value = offset (*object);
        REQUIRE (value.x == Catch::Approx (native.x).margin (1e-3f));
        REQUIRE (value.y == Catch::Approx (-native.y).margin (1e-3f));
    }

    // A root without a depth key keeps (1,1), so a child authored "0 0"
    // still moves with that root.
    group.parallaxDepth.reset ();
    REQUIRE (sceneParallaxDepth (group) == glm::vec2 (1.0f));
    const glm::vec2 defaulted = nativeCanvasParallaxOffset (
        {300, 200}, {1, 1}, {0, 0}, {1920, 1080}, cursor, 0.2f, 1.0f);
    REQUIRE (offset (child).x == Catch::Approx (defaulted.x).margin (1e-3f));
    REQUIRE (offset (child).y == Catch::Approx (-defaulted.y).margin (1e-3f));
    REQUIRE (offset (child) != glm::vec3 (0.0f));

    // A missing parent ends the walk at the last resolvable object, and a
    // malformed cycle terminates.
    Object orphan (node (4, 99, {10, 10, 0}, {1, 1, 1}, 0));
    REQUIRE (&sceneParallaxRoot (orphan, find, objects.size ()) == &orphan);
    Object loopA (node (5, 6, {0, 0, 0}, {1, 1, 1}, 0));
    Object loopB (node (6, 5, {0, 0, 0}, {1, 1, 1}, 0));
    const auto loopFind = [&] (int id) -> const Object* {
        return id == 5 ? &loopA : id == 6 ? &loopB : nullptr;
    };
    const auto& loopRoot = sceneParallaxRoot (loopA, loopFind, 2);
    REQUIRE ((&loopRoot == &loopA || &loopRoot == &loopB));
}

TEST_CASE ("Scene parallax delay follows native per-frame smoothing", "[scene][parallax]") {
    // Native uses (1 - delay/3)*10*frameDelta for positive
    // authored delay. The original 2902406982 delay is 0.1.
    REQUIRE (sceneParallaxDelayWeight (0.0f, 0.0f) == Catch::Approx (1.0f));
    REQUIRE (sceneParallaxDelayWeight (0.1f, 1.0f / 60.0f)
             == Catch::Approx (0.16111111f));
    REQUIRE (sceneParallaxDelayWeight (0.1f, 1.0f / 30.0f)
             == Catch::Approx (0.32222222f));
    REQUIRE (sceneParallaxDelayWeight (0.1f, 0.0f) == Catch::Approx (0.0f));
    REQUIRE (sceneParallaxDelayWeight (0.1f, 1.0f) == Catch::Approx (1.0f));
    REQUIRE (sceneParallaxDelayWeight (3.0f, 1.0f / 60.0f) == Catch::Approx (0.0f));
    // The native implementation has an upper clamp only, even outside the
    // authored slider's 0..3 range.
    REQUIRE (sceneParallaxDelayWeight (4.0f, 1.0f / 60.0f)
             == Catch::Approx (-1.0f / 18.0f));
}

TEST_CASE ("Composed membership resolves the nearest composing ancestor", "[scene][parallax][composition]") {
    // One rule decides both the child draw scopes and the parallax exclusion,
    // so update-time particle geometry and the draw agree.
    Object outer (node (1, {}, {0, 0, 0}, {1, 1, 1}, 0));
    Object group (node (2, 1, {0, 0, 0}, {1, 1, 1}, 0));
    Object inner (node (3, 2, {0, 0, 0}, {1, 1, 1}, 0));
    Object leaf (node (4, 3, {0, 0, 0}, {1, 1, 1}, 0));
    Object orphan (node (5, 99, {0, 0, 0}, {1, 1, 1}, 0));
    const std::map<int, const Object*> objects {{1, &outer}, {2, &group}, {3, &inner}, {4, &leaf}, {5, &orphan}};
    const auto find = [&] (int id) -> const Object* {
        const auto it = objects.find (id);
        return it == objects.end () ? nullptr : it->second;
    };
    std::set<int> composing {1, 3};
    const auto composes = [&] (int id) { return composing.contains (id); };
    REQUIRE_FALSE (sceneCompositionOwner (outer, find, composes).has_value ());
    REQUIRE (sceneCompositionOwner (group, find, composes) == std::optional<int> (1));   // across nothing
    REQUIRE (sceneCompositionOwner (inner, find, composes) == std::optional<int> (1));   // across a transform-only group
    REQUIRE (sceneCompositionOwner (leaf, find, composes) == std::optional<int> (3));    // nearest scope wins
    REQUIRE_FALSE (sceneCompositionOwner (orphan, find, composes).has_value ());         // missing ancestor
    composing = {};
    REQUIRE_FALSE (sceneCompositionOwner (leaf, find, composes).has_value ());           // no scope: drawn directly
    Object loopA (node (6, 7, {0, 0, 0}, {1, 1, 1}, 0));
    Object loopB (node (7, 6, {0, 0, 0}, {1, 1, 1}, 0));
    const auto loopFind = [&] (int id) -> const Object* { return id == 6 ? &loopA : id == 7 ? &loopB : nullptr; };
    REQUIRE_FALSE (sceneCompositionOwner (loopA, loopFind, composes).has_value ());      // cycle terminates
}

TEST_CASE ("Update-time parallax exclusion agrees with the draw scope for every composer nesting", "[scene][parallax][composition]") {
    // refreshComposedObjects walks up from each object before particle updates;
    // the render pass folds sceneCompositionChildScope down while it draws.
    // Every chain of up to three plain composers, fullscreen composers and
    // transform-only groups must give the same answer in both directions.
    // Anchors: parallax-v2 run6a (plain composer: children move only through the
    // composite) and run6b (fullscreen composer: children move once themselves).
    enum Kind { Plain, Fullscreen, Group };
    size_t chains = 0;
    for (int length = 1; length <= 3; ++length) {
        int combinations = 1;
        for (int i = 0; i < length; ++i) combinations *= 3;
        for (int code = 0; code < combinations; ++code) {
            std::vector<Kind> kinds;
            for (int i = 0, c = code; i < length; ++i, c /= 3) kinds.push_back (static_cast<Kind> (c % 3));
            std::vector<std::unique_ptr<Object>> chain;
            for (int i = 0; i <= length; ++i)
                chain.push_back (std::make_unique<Object> (node (i + 1, i == 0 ? std::optional<int> {} : std::optional<int> (i),
                    {0, 0, 0}, {1, 1, 1}, 0)));
            const auto find = [&] (int id) -> const Object* {
                return id >= 1 && id <= static_cast<int> (chain.size ()) ? chain[id - 1].get () : nullptr;
            };
            const auto composes = [&] (int id) { return id <= length && kinds[id - 1] != Group; };
            const auto fullscreen = [&] (int id) { return kinds[id - 1] == Fullscreen; };
            WallpaperEngine::Render::Wallpapers::SceneCompositionScope scope;
            for (int depth = 0; depth <= length; ++depth) {
                CAPTURE (length, code, depth);
                REQUIRE (sceneCompositionSuppressesParallax (*chain[depth], find, composes, fullscreen)
                    == scope.localProjection);
                if (depth < length && kinds[depth] != Group)
                    scope = sceneCompositionChildScope (scope, kinds[depth] == Fullscreen);
            }
            ++chains;
        }
    }
    REQUIRE (chains == 39);
    using WallpaperEngine::Render::Wallpapers::SceneCompositionScope;
    REQUIRE (sceneCompositionChildScope ({}, false).localProjection);         // run6a
    REQUIRE_FALSE (sceneCompositionChildScope ({}, true).localProjection);    // run6b
    REQUIRE (sceneCompositionChildScope ({}, true).offscreenTarget);
}

TEST_CASE ("A fullscreen composer's children present exactly like root draws", "[scene][composition]") {
    // Native keeps the scene stacks for a fullscreen composer, so its
    // children take the root projection, winding and copy route; only a
    // non-fullscreen composer's local box changes them.
    using WallpaperEngine::Render::Wallpapers::SceneCompositionScope;
    using WallpaperEngine::Render::Wallpapers::modelPresentationReversesWinding;
    using WallpaperEngine::Render::Wallpapers::particlePresentationReversesWinding;
    using WallpaperEngine::Render::Wallpapers::sceneFullscreenCopyInClipSpace;
    const SceneCompositionScope root {};
    const auto fullscreen = sceneCompositionChildScope (root, true);
    const auto nestedFullscreen = sceneCompositionChildScope (fullscreen, true);
    const auto local = sceneCompositionChildScope (root, false);
    const auto fullscreenInLocal = sceneCompositionChildScope (local, true);
    for (const auto& scope : {fullscreen, nestedFullscreen}) {
        for (const bool orthographic : {true, false}) {
            REQUIRE (modelPresentationReversesWinding (orthographic, scope.localProjection)
                == modelPresentationReversesWinding (orthographic, root.localProjection));
            for (const bool presetWorld : {true, false})
                for (const bool perspectiveOverride : {true, false})
                    REQUIRE (particlePresentationReversesWinding (orthographic, presetWorld, scope.localProjection, perspectiveOverride)
                        == particlePresentationReversesWinding (orthographic, presetWorld, root.localProjection, perspectiveOverride));
        }
        REQUIRE (sceneFullscreenCopyInClipSpace (true, scope));
    }
    // A perspective local box no longer carries the root presentation reflection.
    for (const auto& scope : {local, fullscreenInLocal}) {
        REQUIRE_FALSE (modelPresentationReversesWinding (false, scope.localProjection));
        REQUIRE_FALSE (particlePresentationReversesWinding (false, false, scope.localProjection, false));
        REQUIRE (particlePresentationReversesWinding (false, false, scope.localProjection, true));
        REQUIRE_FALSE (sceneFullscreenCopyInClipSpace (true, scope));
    }
    REQUIRE (sceneFullscreenCopyInClipSpace (true, root));
    REQUIRE_FALSE (sceneFullscreenCopyInClipSpace (false, root));

    // parallax-v2 run6b, 1920x1080 canvas in a 1280x720 output: native draws K1
    // at x=660 and K2 at (1260,440), wherever the composer target size comes from.
    const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    const glm::mat4 scene = glm::ortho (-960.0f, 960.0f, -540.0f, 540.0f, -1000.0f, 1000.0f);
    const glm::mat4 parentWorld = glm::translate (glm::mat4 (1.0f), glm::vec3 (-960.0f, 540.0f, 0.0f))
        * flip * glm::translate (glm::mat4 (1.0f), glm::vec3 (960.0f, 540.0f, 0.0f));
    for (const glm::vec2 target : {glm::vec2 (1280.0f, 720.0f), glm::vec2 (1920.0f, 1080.0f)}) {
        const auto composed = sceneCompositionChildProjection (true, scene, parentWorld, flip, target);
        REQUIRE (composed.has_value ());
        const glm::vec4 k1 = *composed * glm::vec4 (-300.0f, 0.0f, 0.0f, 1.0f);
        const glm::vec4 k2 = *composed * glm::vec4 (300.0f, 100.0f, 0.0f, 1.0f);
        REQUIRE ((k1.x + 1.0f) * 960.0f == Catch::Approx (660.0f));
        REQUIRE ((k2.x + 1.0f) * 960.0f == Catch::Approx (1260.0f));
        REQUIRE ((1.0f - k2.y) * 540.0f == Catch::Approx (440.0f));
    }
    // A non-fullscreen composer maps its own authored box onto the whole target.
    const auto box = sceneCompositionChildProjection (false, scene, glm::mat4 (1.0f), flip, {1200.0f, 400.0f});
    REQUIRE (box.has_value ());
    for (const glm::vec2 corner : {glm::vec2 (-600, -200), glm::vec2 (600, 200)}) {
        const glm::vec4 ndc = *box * glm::vec4 (corner, 0.0f, 1.0f);
        REQUIRE (std::abs (ndc.x) == Catch::Approx (1.0f));
        REQUIRE (std::abs (ndc.y) == Catch::Approx (1.0f));
    }
    REQUIRE_FALSE (sceneCompositionChildProjection (false, scene, glm::mat4 (0.0f), flip, {1200.0f, 400.0f}).has_value ());
}

TEST_CASE ("g_ParallaxPosition follows the native smoothed parallax point", "[scene][parallax]") {
    // Native. Cursors are top-origin,
    // as CScene::m_mousePosition and native g_PointerPosition; native uses (x, 1 - y).
    using WallpaperEngine::Render::Wallpapers::advanceSceneParallaxPosition;
    using WallpaperEngine::Render::Wallpapers::initialSceneParallaxPosition;
    const glm::vec2 size (1920.0f, 1080.0f);
    auto state = initialSceneParallaxPosition (true, size, glm::vec2 (0.0f));
    REQUIRE (state.uniform == glm::vec2 (0.5f));
    REQUIRE (state.point == size * 0.5f);
    // Parallax off: the uniform keeps its load value whatever the cursor does.
    advanceSceneParallaxPosition (state, false, glm::vec2 (0.9f, 0.1f), 1.0f, size, glm::vec2 (0.0f), 1.0f);
    REQUIRE (state.uniform == glm::vec2 (0.5f));
    advanceSceneParallaxPosition (state, true, glm::vec2 (0.5f), 0.21f, size, glm::vec2 (0.0f), 1.0f);
    REQUIRE (state.uniform.x == Catch::Approx (0.5f));
    REQUIRE (state.uniform.y == Catch::Approx (0.5f));
    // 3212934821 offset cursor: window (0.8133, 0.25) from the top, influence 0.21.
    advanceSceneParallaxPosition (state, true, glm::vec2 (0.8133f, 0.25f), 0.21f, size, glm::vec2 (0.0f), 1.0f);
    REQUIRE (state.uniform.x == Catch::Approx (0.5658f).margin (1e-4));
    REQUIRE (state.uniform.y == Catch::Approx (0.5525f).margin (1e-4));
    // The cursor is clamped before scaling and the result after the eye offset.
    advanceSceneParallaxPosition (state, true, glm::vec2 (1.5f, 1.5f), 1.0f, size, glm::vec2 (0.0f), 1.0f);
    REQUIRE (state.uniform == glm::vec2 (1.0f, 0.0f));
    advanceSceneParallaxPosition (state, true, glm::vec2 (0.5f), 0.21f, size, glm::vec2 (5000.0f, -5000.0f), 1.0f);
    REQUIRE (state.uniform == glm::vec2 (1.0f, 0.0f));
    // Linux-side, not native-backed: native's perspective size is 0, so its
    // uniform is degenerate there (documented divergence); this pins only the
    // easing from the perspective start point 0 with the layer offset's delay
    // weight, at an arbitrary size.
    auto eased = initialSceneParallaxPosition (false, size, glm::vec2 (0.0f));
    REQUIRE (eased.point == glm::vec2 (0.0f));
    REQUIRE (eased.uniform == glm::vec2 (0.5f));
    const float weight = sceneParallaxDelayWeight (1.5f, 1.0f / 30.0f);
    glm::vec2 displacement (0.0f);
    for (int frame = 0; frame < 3; ++frame) {
        advanceSceneParallaxPosition (eased, true, glm::vec2 (1.0f, 0.0f), 1.0f, size, glm::vec2 (0.0f), weight);
        displacement = glm::mix (displacement, glm::vec2 (1.0f), weight);
    }
    REQUIRE (eased.uniform.x == Catch::Approx (displacement.x));
    REQUIRE (eased.uniform.y == Catch::Approx (displacement.y));
}

TEST_CASE ("g_ParallaxPosition and the layer offset agree on one top-origin cursor", "[scene][parallax]") {
    // CScene feeds m_parallaxCursor to both. Native derives both from the same
    // smoothed point: offset = (root - point) * amount * depth in the y-up
    // canvas and uniform = point / size. Recovering the
    // point from the uniform must reproduce the layer offset on both axes.
    using WallpaperEngine::Render::Wallpapers::advanceSceneParallaxPosition;
    using WallpaperEngine::Render::Wallpapers::initialSceneParallaxPosition;
    using WallpaperEngine::Render::Wallpapers::sceneParallaxDisplacementTarget;
    const glm::vec2 size (2560.0f, 1440.0f);
    const glm::vec3 root (700.0f, 1000.0f, 0.0f);
    const float amount = 0.25f, influence = 0.21f;
    for (const glm::vec2 cursor : {glm::vec2 (0.8133f, 0.25f), glm::vec2 (0.1f, 0.9f), glm::vec2 (0.5f)}) {
        CAPTURE (cursor.x, cursor.y);
        auto state = initialSceneParallaxPosition (true, size, glm::vec2 (0.0f));
        advanceSceneParallaxPosition (state, true, cursor, influence, size, glm::vec2 (0.0f), 1.0f);
        const glm::vec2 point = state.uniform * size;
        const glm::vec3 offset = sceneLayerParallaxOffset (root, glm::vec3 (0.0f), glm::vec2 (1.0f),
            sceneParallaxDisplacementTarget (cursor, amount, influence), size.x, size.y, amount, true, true);
        REQUIRE (offset.x == Catch::Approx ((root.x - point.x) * amount).margin (1e-3));
        REQUIRE (-offset.y == Catch::Approx ((root.y - point.y) * amount).margin (1e-3));   // camera Y is negated
    }
}

TEST_CASE ("Camera parallax sweeps the full canvas of a cover-cropped scene", "[scene][parallax]") {
    // Native scales the viewport-fraction cursor
    // by the full projection size. A 3440x1440 canvas shown
    // at 16:9 keeps only u 0.128..0.872, so a cursor taken from that band moves
    // the C-PLX F4H ember (amount 0.16, influence 0.18, depth 2) by 0.744 of
    // native's 97.09 canvas px between the corner and centre pointers.
    using WallpaperEngine::Render::WallpaperState;
    using WallpaperEngine::Render::Wallpapers::advanceSceneParallaxPosition;
    using WallpaperEngine::Render::Wallpapers::initialSceneParallaxPosition;
    using WallpaperEngine::Render::Wallpapers::sceneParallaxCursor;
    using WallpaperEngine::Render::Wallpapers::sceneParallaxDisplacementTarget;
    const float amount = 0.16f, influence = 0.18f;
    const glm::vec3 root (2000.0f, 720.0f, 0.0f);
    for (const bool vflip : {false, true}) {
        CAPTURE (vflip);
        WallpaperState state (WallpaperState::TextureUVsScaling::DefaultUVs, 0);
        state.updateState ({0, 0, 1280, 720}, vflip, 3440, 1440);
        const auto uv = state.getTextureUVs ();
        REQUIRE (uv.ustart == Catch::Approx (0.12791f).margin (1e-4));
        REQUIRE (uv.uend == Catch::Approx (0.87209f).margin (1e-4));
        const auto cursor = [&] (glm::vec2 viewport) {
            return sceneParallaxCursor (viewport, uv.ustart, uv.uend, uv.vstart, uv.vend);
        };
        const auto offset = [&] (glm::vec2 viewport) {
            return sceneLayerParallaxOffset (root, glm::vec3 (0.0f), glm::vec2 (2.0f),
                sceneParallaxDisplacementTarget (cursor (viewport), amount, influence),
                3440.0f, 1440.0f, amount, true, true);
        };
        // updateMouse's top-origin viewport fractions (mouseY = 1 - y-up) for
        // the top-left corner pointer (0.01, 0.01) and the centre.
        const glm::vec2 corner (0.01f), centre (0.5f);
        const glm::vec3 delta = offset (corner) - offset (centre);
        REQUIRE (delta.x == Catch::Approx (0.49f * 3440.0f * influence * amount * 2.0f).margin (1e-3));
        const float canvasY = 0.49f * 1440.0f * influence * amount * 2.0f;
        if (vflip) {
            // A top pointer raises the parallax point, so native moves the layer
            // down the y-up canvas; Linux camera Y is the negated canvas axis.
            REQUIRE (delta.y == Catch::Approx (canvasY).margin (1e-3));
        } else {
            // vflip=false (X11) reverses the band, and with it m_mousePosition's
            // y; that orientation is pre-existing and kept as is.
            REQUIRE (std::abs (delta.y) == Catch::Approx (canvasY).margin (1e-3));
        }
        auto parallax = initialSceneParallaxPosition (true, glm::vec2 (3440.0f, 1440.0f), glm::vec2 (0.0f));
        advanceSceneParallaxPosition (parallax, true, cursor (corner), influence,
            glm::vec2 (3440.0f, 1440.0f), glm::vec2 (0.0f), 1.0f);
        REQUIRE (parallax.uniform.x == Catch::Approx (0.5f - 0.49f * influence).margin (1e-5));
    }
    // An uncropped scene keeps the cursor m_mousePosition already forms.
    for (const bool vflip : {false, true}) {
        CAPTURE (vflip);
        WallpaperState state (WallpaperState::TextureUVsScaling::DefaultUVs, 0);
        state.updateState ({0, 0, 1280, 720}, vflip, 2560, 1440);
        const auto uv = state.getTextureUVs ();
        const glm::vec2 viewport (0.23f, 0.71f);
        const glm::vec2 scene (uv.ustart + viewport.x * (uv.uend - uv.ustart),
                               uv.vstart + viewport.y * (uv.vend - uv.vstart));
        const glm::vec2 parallax = sceneParallaxCursor (viewport, uv.ustart, uv.uend, uv.vstart, uv.vend);
        REQUIRE (parallax.x == Catch::Approx (scene.x));
        REQUIRE (parallax.y == Catch::Approx (scene.y));
    }
}

TEST_CASE ("Unparented scene parallax precedes rotated and mirrored object transforms", "[scene][parallax][particle]") {
    // The 2334035201 mouth smoke has Z angle ~pi and the left fog has a
    // negative X scale. Native moves the scene stack before either
    // object's renderer applies those local transforms. Its authored camera
    // eye (-47.67, 204.18) is reset to 0 for the root orthographic camera
    // as at runtime.
    const glm::vec3 eye {0.0f};
    const glm::vec2 center {0.0f};
    const glm::vec2 right {0.0144f, 0.0f}; // centered cursor .5 * amount .18 * influence .16
    const auto offset = [&] (const glm::vec3& origin, const glm::vec2& displacement) {
        return sceneLayerParallaxOffset (
            origin, eye, {1.0f, 1.0f}, displacement, 1920.0f, 1080.0f, 0.18f, true, true);
    };
    for (const auto& [origin, scale, angle] : {
             std::tuple {glm::vec3 (943.53278f, 637.84656f, 0.0f),
                         glm::vec3 (1.04606f, 1.48629f, 1.0f), 3.12857f},
             std::tuple {glm::vec3 (663.39062f, 591.50177f, 0.0f),
                         glm::vec3 (-1.47145f, 1.49194f, 1.0f), 0.0f}}) {
        const glm::mat4 local = glm::rotate (glm::mat4 (1.0f), angle, {0.0f, 0.0f, 1.0f})
            * glm::scale (glm::mat4 (1.0f), scale);
        const auto transformed = [&] (const glm::vec2& displacement) {
            const auto world = glm::translate (glm::mat4 (1.0f), offset (origin, displacement)) * local;
            return glm::vec3 (world * glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f));
        };
        const glm::vec3 delta = transformed (right) - transformed (center);
        REQUIRE (delta.x == Catch::Approx (-27.648f).margin (0.001f));
        REQUIRE (delta.y == Catch::Approx (0.0f).margin (0.001f));
    }
    // Static terms: canvas (origin - eye - size/2) * amount, with the canvas
    // Y negated into camera space.
    const auto smokeCenter = offset ({943.53278f, 637.84656f, 0.0f}, center);
    REQUIRE (smokeCenter.x == Catch::Approx (-2.96410f).margin (0.002f));
    REQUIRE (smokeCenter.y == Catch::Approx (-17.61238f).margin (0.002f));
    // The native cursor uses scene height on Y.
    const auto characterCenter = offset ({960.0f, 540.0f, 0.0f}, center);
    const auto characterTop = offset ({960.0f, 540.0f, 0.0f}, {0.0f, 0.0144f});
    REQUIRE (characterCenter.x == Catch::Approx (0.0f).margin (0.002f));
    REQUIRE (characterCenter.y == Catch::Approx (0.0f).margin (0.002f));
    REQUIRE ((characterTop - characterCenter).y == Catch::Approx (-15.552f).margin (0.001f));
    REQUIRE (sceneLayerParallaxOffset ({943.53278f, 637.84656f, 0.0f}, eye,
             {0.0f, 0.0f}, right, 1920.0f, 1080.0f, 0.18f, true, true) == glm::vec3 (0.0f));
}

TEST_CASE ("Named puppet attachments compose live bone and offset before child local", "[scene][transform][puppet]") {
    Object parent (node (1, {}, {100, 50, 0}, {1, 1, 1}, glm::half_pi<float> ()));
    Object child (node (2, 1, {3, 0, 0}, {1, 1, 1}, 0));
    child.attachment = "Hand";
    float boneX = 10.0f;
    int lookups = 0;
    auto resolve = [&] {
        return resolveSceneTransform (child,
            [&] (int id) -> const Object* { return id == 1 ? &parent : nullptr; },
            [&] (const Object& owner, const std::string& name) -> std::optional<glm::mat4> {
                REQUIRE (owner.id == 1);
                REQUIRE (name == "Hand");
                ++lookups;
                const glm::mat4 bone = glm::rotate (
                    glm::translate (glm::mat4 (1), glm::vec3 (boneX, 0, 0)),
                    glm::half_pi<float> (), glm::vec3 (0, 0, 1));
                const glm::mat4 offset = glm::translate (glm::mat4 (1), glm::vec3 (2, 0, 0));
                return bone * offset;
            });
    };
    const auto first = resolve ();
    REQUIRE (first.origin.x == Catch::Approx (95.0f));
    REQUIRE (first.origin.y == Catch::Approx (60.0f));
    boneX = 20.0f;
    const auto moved = resolve ();
    REQUIRE (moved.origin.x == Catch::Approx (95.0f));
    REQUIRE (moved.origin.y == Catch::Approx (70.0f));
    REQUIRE (lookups == 2);
    child.attachment.reset ();
    const auto ordinary = resolve ();
    REQUIRE (ordinary.origin.x == Catch::Approx (100.0f));
    REQUIRE (ordinary.origin.y == Catch::Approx (53.0f));
    REQUIRE (lookups == 2);
}

TEST_CASE ("Scene transform walks long valid chains and reports broken graphs", "[scene][transform]") {
    std::vector<std::unique_ptr<Object>> nodes;
    for (int id = 0; id < 40; ++id)
        nodes.push_back (std::make_unique<Object> (node (
            id, id ? std::optional<int> (id - 1) : std::nullopt,
            {1, 0, 0}, {1, 1, 1}, 0)));
    auto find = [&] (int id) -> const Object* {
        return id >= 0 && id < static_cast<int> (nodes.size ()) ? nodes[id].get () : nullptr;
    };
    REQUIRE (resolveSceneTransform (*nodes.back (), find).origin.x == Catch::Approx (40.0f));
    nodes[0]->parent = 39;
    REQUIRE_THROWS_AS (resolveSceneTransform (*nodes.back (), find), std::invalid_argument);
    nodes[0]->parent = 999;
    REQUIRE_THROWS_AS (resolveSceneTransform (*nodes.back (), find), std::invalid_argument);
}

TEST_CASE ("World control points use finite pre-parallax inverse across scale transitions", "[scene][transform]") {
    const glm::vec4 worldPoint {12.0f, 24.0f, 0.0f, 1.0f};
    const glm::mat4 worldFromLocal = glm::translate (glm::mat4 (1.0f), glm::vec3 (10, 20, 0))
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (0.001f));
    const auto inverse = inverseFiniteTransform (worldFromLocal);
    REQUIRE (inverse.has_value ());
    const glm::vec3 local = projectWorldControlPoint (*inverse, true, glm::vec3 (worldPoint), {});
    REQUIRE (local.x == Catch::Approx (2000.0f));
    REQUIRE (local.y == Catch::Approx (4000.0f));
    // A render-only displacement changes visible coordinates, but the same
    // world simulation point keeps its pre-parallax local coordinate.
    const glm::mat4 visuallyDisplaced = worldFromLocal * glm::translate (glm::mat4 (1.0f), glm::vec3 (5, 0, 0));
    REQUIRE ((inverseFiniteTransform (visuallyDisplaced).value () * worldPoint).x
             != Catch::Approx (local.x));
    const glm::mat4 collapsed = glm::scale (worldFromLocal, glm::vec3 (0, 1, 1));
    REQUIRE_FALSE (inverseFiniteTransform (collapsed).has_value ());
    const glm::vec3 held = projectWorldControlPoint (
        glm::mat4 (1.0f), false, glm::vec3 (worldPoint), local);
    REQUIRE (held == local);
    const glm::mat4 movedAndRestored = glm::translate (glm::mat4 (1.0f), glm::vec3 (11, 22, 0))
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (0.002f));
    const auto restoredInverse = inverseFiniteTransform (movedAndRestored);
    REQUIRE (restoredInverse.has_value ());
    const glm::vec3 restored = projectWorldControlPoint (
        *restoredInverse, true, glm::vec3 (worldPoint), held);
    REQUIRE (restored.x == Catch::Approx (500.0f));
    REQUIRE (restored.y == Catch::Approx (1000.0f));
    REQUIRE (restored != held);
    const glm::vec3 overflow = projectWorldControlPoint (
        *inverse, true, glm::vec3 (1e38f), local);
    REQUIRE (overflow == local);
}

TEST_CASE ("Parented control-point conversion retains world position through live parent edits", "[scene][transform]") {
    Object parent (node (1, {}, {10, 20, 0}, {2, 1, 1}, glm::half_pi<float> ()));
    Object child (node (2, 1, {3, 4, 0}, {0.001f, 0.001f, 1}, glm::quarter_pi<float> ()));
    auto model = [&] {
        const auto transform = resolveSceneTransform (child, [&] (int id) -> const Object* {
            return id == 1 ? &parent : nullptr;
        });
        const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1, -1, 1));
        return glm::translate (glm::mat4 (1.0f), glm::vec3 (-50, 50, 0))
            * flip * transform.authoredMatrix * flip;
    };
    const glm::mat4 initialModel = model ();
    const glm::vec3 fixedWorld = glm::vec3 (initialModel * glm::vec4 (2, -1, 0, 1));
    // Hand-composed world point: parent origin (6,26), child R45*S(.001)
    // transformed by parent R90*S(2,1), then particle's Y flip/centering.
    REQUIRE (fixedWorld.x == Catch::Approx (-44.00212132f).margin (1e-4f));
    REQUIRE (fixedWorld.y == Catch::Approx (23.99858579f).margin (1e-4f));
    const auto initialInverse = inverseFiniteTransform (initialModel);
    REQUIRE (initialInverse.has_value ());
    const glm::vec3 initialLocal = projectWorldControlPoint (*initialInverse, true, fixedWorld, {});
    REQUIRE (initialLocal.x == Catch::Approx (2.0f).margin (0.005f));
    REQUIRE (initialLocal.y == Catch::Approx (-1.0f).margin (0.005f));

    // This preserves the current simulation conversion without visual
    // parallax; native control-point/parallax space remains to be verified.
    const glm::mat4 parallaxModel = glm::translate (initialModel, glm::vec3 (30, -15, 0));
    REQUIRE (glm::distance (
        glm::vec3 (inverseFiniteTransform (parallaxModel).value () * glm::vec4 (fixedWorld, 1)),
        initialLocal) > 1.0f);
    REQUIRE (projectWorldControlPoint (*initialInverse, true, fixedWorld, {}) == initialLocal);

    parent.groupScale->value->update (glm::vec3 (0, 1, 1), WallpaperEngine::Data::Model::DynamicValue::Script);
    REQUIRE_FALSE (inverseFiniteTransform (model ()).has_value ());
    REQUIRE (projectWorldControlPoint (glm::mat4 (1.0f), false, fixedWorld, initialLocal) == initialLocal);

    parent.groupScale->value->update (glm::vec3 (2, 1, 1), WallpaperEngine::Data::Model::DynamicValue::Script);
    parent.origin->value->update (glm::vec3 (13, 17, 0), WallpaperEngine::Data::Model::DynamicValue::Script);
    const glm::mat4 movedModel = model ();
    const auto movedInverse = inverseFiniteTransform (movedModel);
    REQUIRE (movedInverse.has_value ());
    const glm::vec3 updatedLocal = projectWorldControlPoint (*movedInverse, true, fixedWorld, initialLocal);
    REQUIRE (glm::distance (updatedLocal, initialLocal) > 1.0f);
    const glm::vec3 roundTrip = glm::vec3 (movedModel * glm::vec4 (updatedLocal, 1));
    REQUIRE (roundTrip.x == Catch::Approx (fixedWorld.x).margin (1e-4f));
    REQUIRE (roundTrip.y == Catch::Approx (fixedWorld.y).margin (1e-4f));
}

TEST_CASE ("Absolute particle control points keep native canvas endpoints through sequence rendering",
           "[scene][particle][controlpoint][sequence]") {
    using WallpaperEngine::Render::Wallpapers::particleControlPointMatrix;
    using WallpaperEngine::Render::Wallpapers::scenePointForCamera;
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    const glm::vec2 canvas {1920.0f, 1080.0f};
    const glm::mat4 center = glm::translate (glm::mat4 (1.0f), {-960.0f, 540.0f, 0.0f});
    const glm::mat4 reflect = glm::scale (glm::mat4 (1.0f), {1.0f, -1.0f, 1.0f});
    // Installed native discharge layer 508 authors this origin and an
    // absolute world CP1. Rotated/mirrored nodes must reach that same canvas
    // endpoint, independently of their emitter-local transformation.
    const glm::vec3 origin {154.130f, 644.440f, 0.0f};
    const glm::vec3 endpoint {947.560f, 1081.670f, 0.0f};
    for (const auto& [scale, angle] : {
             std::pair {glm::vec3 (0.101f, 0.243f, 0.635f), 0.0f},
             std::pair {glm::vec3 (-1.5f, 0.7f, 1.0f), 1.2f}}) {
        const glm::mat4 node = center * reflect
            * glm::translate (glm::mat4 (1.0f), origin)
            * glm::rotate (glm::mat4 (1.0f), angle, {0.0f, 0.0f, 1.0f})
            * glm::scale (glm::mat4 (1.0f), scale) * reflect;
        const auto inverse = inverseFiniteTransform (node);
        REQUIRE (inverse.has_value ());
        const glm::mat4 cp0 = particleControlPointMatrix (
            glm::mat4 (1.0f), node, inverse, false, false, true, true, canvas);
        const glm::mat4 cp1 = particleControlPointMatrix (
            glm::translate (glm::mat4 (1.0f), glm::vec3 (endpoint.x, -endpoint.y, endpoint.z)),
            node, inverse, false, true, false, true, canvas);
        for (float phase : {0.0f, 0.5f, 1.0f}) {
            BetweenControlPointsState state {phase, 0.0f};
            const auto birth = betweenControlPointsBirth (
                glm::vec3 (cp0[3]), {}, 1.0f, glm::vec3 (cp0[3]), glm::vec3 (cp1[3]),
                state, {0.0f, 1.0f}, false, 0u, 0.0f, {}, 0.0f, false);
            const glm::vec3 rendered = glm::vec3 (node * glm::vec4 (birth.position, 1.0f));
            const glm::vec3 expected = scenePointForCamera (
                glm::mix (origin, endpoint, phase), canvas.x, canvas.y, true);
            for (int axis = 0; axis < 3; ++axis)
                REQUIRE (rendered[axis] == Catch::Approx (expected[axis]).margin (0.001f));
        }
        const glm::mat4 authored = glm::translate (glm::mat4 (1.0f), glm::vec3 (endpoint.x, -endpoint.y, endpoint.z));
        const auto directWorld = particleControlPointMatrix (
            authored, node, inverse, true, true, false, true, canvas);
        REQUIRE (glm::distance (glm::vec3 (directWorld[3]),
                 scenePointForCamera (endpoint, canvas.x, canvas.y, true)) < 0.001f);
        // Native CP0 remains stack-mapped even when both world flags are set.
        REQUIRE (particleControlPointMatrix (authored, node, inverse,
                 true, true, true, true, canvas) == node * authored);
        REQUIRE (particleControlPointMatrix (authored, node, inverse,
                 false, false, false, true, canvas) == authored);
    }
}

TEST_CASE ("Mouse control points replace offsets and reach native local and world sequence markers",
           "[scene][particle][controlpoint][mouse]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    const auto projection = glm::ortho (-160.0f, 160.0f, -120.0f, 120.0f, 0.0f, 10000.0f);
    const auto reflect = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const auto node = glm::translate (glm::mat4 (1), glm::vec3 (-160, 120, 0)) * reflect
        * glm::translate (glm::mat4 (1), glm::vec3 (60, 55, 0))
        * glm::rotate (glm::mat4 (1), 0.35f, glm::vec3 (0, 0, 1))
        * glm::scale (glm::mat4 (1), glm::vec3 (-0.7f, 0.8f, 1)) * reflect;
    const auto inverse = inverseFiniteTransform (node);
    REQUIRE (inverse);
    // Native complete offset/zero-offset fixtures both end at these cursor
    // positions, irrespective of authored CP offset(32,17) and node mirror.
    for (const auto uv : {glm::vec2 (0.5f), glm::vec2 (0.75f)}) {
        const auto world = particleMouseWorldPosition (
            uv, particleMouseProjection (projection, true), glm::mat4 (1));
        REQUIRE (world);
        const glm::vec2 end = uv.x == 0.5f ? glm::vec2 (640, 480) : glm::vec2 (960, 240);
        for (bool presetWorld : {false, true}) {
            const glm::vec3 cp0 = presetWorld ? glm::vec3 (node[3]) : glm::vec3 (0);
            const glm::vec3 cp1 = particleMouseControlPoint (*world, presetWorld, *inverse, true, {});
            for (float phase : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                BetweenControlPointsState state {phase, 0};
                const auto birth = betweenControlPointsBirth (
                    cp0, {}, 1, cp0, cp1, state, {0, 1}, false, 0u, 0, {}, 0, presetWorld);
                const glm::vec4 clip = projection * (presetWorld ? glm::mat4 (1) : node)
                    * glm::vec4 (birth.position, 1);
                const glm::vec2 screen = (glm::vec2 (clip) / clip.w * 0.5f + 0.5f)
                    * glm::vec2 (1280, 960);
                const auto expected = glm::mix (glm::vec2 (240, 740), end, phase);
                REQUIRE (screen.x == Catch::Approx (expected.x).margin (0.001f));
                REQUIRE (screen.y == Catch::Approx (expected.y).margin (0.001f));
            }
        }
    }
}

TEST_CASE ("Mouse unprojection consumes the authored fit fill crop once",
           "[scene][particle][controlpoint][mouse][presentation]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    using WallpaperEngine::Render::WallpaperState;
    using Mode = WallpaperState::TextureUVsScaling;
    const auto projection = glm::ortho (-160.0f, 160.0f, -120.0f, 120.0f, 0.0f, 10000.0f);
    for (const auto output : {glm::ivec2 (1280, 720), glm::ivec2 (720, 960)}) {
        for (const auto mode : {Mode::ZoomFitUVs, Mode::ZoomFillUVs, Mode::StretchUVs}) {
            for (bool vflip : {false, true}) {
                WallpaperState state (mode, 0);
                state.updateState ({0, 0, output.x, output.y}, vflip, 320, 240);
                const auto uv = state.getTextureUVs ();
                const auto crop = scenePresentationClipTransform ({uv.ustart, uv.uend, uv.vstart, uv.vend});
                for (const auto normalized : {glm::vec2 (0.5f), glm::vec2 (0.65f, 0.6f)}) {
                    const glm::vec2 authoredUv {
                        glm::mix (uv.ustart, uv.uend, normalized.x),
                        glm::mix (uv.vstart, uv.vend, normalized.y)};
                    const auto world = particleMouseWorldPosition (
                        authoredUv, particleMouseProjection (projection, true), glm::mat4 (1));
                    REQUIRE (world);
                    const glm::vec4 clip = crop * projection * glm::vec4 (*world, 1);
                    // CWallpaper's fixed reflected quad samples ascending V for
                    // GLFW/Wayland and descending V for X11. Model that final
                    // presentation, retaining the same physical cursor target.
                    const glm::vec2 outputClip (clip.x / clip.w, (vflip ? clip.y : -clip.y) / clip.w);
                    const glm::vec2 screen = (outputClip * 0.5f + 0.5f) * glm::vec2 (output);
                    REQUIRE (screen.x == Catch::Approx (normalized.x * output.x).margin (0.001f));
                    REQUIRE (screen.y == Catch::Approx ((1 - normalized.y) * output.y).margin (0.001f));
                }
            }
        }
    }
}

TEST_CASE ("Perspective mouse uses native reversed far depth and preserves both draw anchors",
           "[scene][particle][controlpoint][mouse][perspective]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto reflect = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const auto projection = reflect * glm::perspective (glm::radians (50.0f), 4.0f / 3.0f, 100.0f, 1000.0f);
    const auto view = glm::lookAt (glm::vec3 (0, 0, 500), glm::vec3 (0), glm::vec3 (0, 1, 0));
    const auto node = glm::translate (glm::mat4 (1), glm::vec3 (-60, -40, 0))
        * glm::rotate (glm::mat4 (1), 0.35f, glm::vec3 (0, 0, 1))
        * glm::scale (glm::mat4 (1), glm::vec3 (-0.7f, 0.8f, 1)) * reflect;
    const auto inverse = inverseFiniteTransform (node);
    REQUIRE (inverse);
    for (const auto uv : {glm::vec2 (0.5f), glm::vec2 (0.5625f, 0.5f), glm::vec2 (0.5f, 7.0f / 12.0f)}) {
        const auto world = particleMouseWorldPosition (
            uv, particleMouseProjection (projection, false), view);
        REQUIRE (world);
        const glm::vec2 expectedWorld {
            (uv.x * 2 - 1) * std::tan (glm::radians (25.0f)) * (4.0f / 3.0f) * 1000.0f,
            (uv.y * 2 - 1) * std::tan (glm::radians (25.0f)) * 1000.0f};
        REQUIRE (world->x == Catch::Approx (expectedWorld.x).margin (0.001f));
        REQUIRE (world->y == Catch::Approx (expectedWorld.y).margin (0.001f));
        REQUIRE (world->z == 0);
        for (bool presetWorld : {false, true}) {
            const auto vp = particlePerspectiveViewProjection (projection, view, presetWorld);
            const auto draw = presetWorld ? reflect : node;
            const auto endpoint = particleMouseControlPoint (*world, presetWorld, *inverse, true, {});
            const auto start = presetWorld ? glm::vec3 (node[3]) : glm::vec3 (0);
            for (float phase : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
                const glm::vec4 clip = vp * draw * glm::vec4 (glm::mix (start, endpoint, phase), 1);
                const auto screen = (glm::vec2 (clip) / clip.w * 0.5f + 0.5f) * glm::vec2 (1280, 960);
                const glm::vec2 expectedStart {516.475f, 562.350f};
                const glm::vec2 expectedEnd {640 + (uv.x - 0.5f) * 2560, 480 - (uv.y - 0.5f) * 1920};
                const auto expected = glm::mix (expectedStart, expectedEnd, phase);
                REQUIRE (screen.x == Catch::Approx (expected.x).margin (0.01f));
                REQUIRE (screen.y == Catch::Approx (expected.y).margin (0.01f));
            }
        }
    }
}

TEST_CASE ("Mouse depth is native before tilted view and local inverse; singular local points stay finite",
           "[scene][particle][controlpoint][mouse][perspective]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto reflect = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const auto glProjection = glm::perspective (glm::radians (50.0f), 4.0f / 3.0f, 100.0f, 1000.0f);
    const auto view = glm::lookAt (glm::vec3 (30, 40, 500), glm::vec3 (0), glm::vec3 (0.1f, 1, 0));
    // Literal coefficients recovered from native14009a360, independent of
    // the OpenGL-to-native depth adapter; native mouse uses this NDCz0.
    glm::mat4 nativeProjection (0);
    nativeProjection[0][0] = 1 / (std::tan (glm::radians (25.0f)) * (4.0f / 3.0f));
    nativeProjection[1][1] = 1 / std::tan (glm::radians (25.0f));
    nativeProjection[2][2] = 100.0f / 900.0f;
    nativeProjection[2][3] = -1;
    nativeProjection[3][2] = 100000.0f / 900.0f;
    const glm::vec2 uv {0.62f, 0.68f};
    const auto raw = glm::inverse (nativeProjection * view) * glm::vec4 (uv * 2.0f - 1.0f, 0, 1);
    const glm::vec3 expectedWorld (raw.x / raw.w, raw.y / raw.w, 0);
    const auto world = particleMouseWorldPosition (uv, particleMouseProjection (reflect * glProjection, false), view);
    REQUIRE (world);
    REQUIRE (glm::distance (*world, expectedWorld) < 0.001f);
    const auto node = glm::translate (glm::mat4 (1), glm::vec3 (15, 20, 30))
        * glm::rotate (glm::mat4 (1), 0.4f, glm::vec3 (1, 0, 0))
        * glm::scale (glm::mat4 (1), glm::vec3 (-0.7f, 0.8f, 1)) * reflect;
    const auto inverse = inverseFiniteTransform (node);
    REQUIRE (inverse);
    const auto local = particleMouseControlPoint (*world, false, *inverse, true, {});
    REQUIRE (glm::distance (glm::vec3 (node * glm::vec4 (local, 1)), expectedWorld) < 0.001f);
    REQUIRE (std::abs (local.z) > 1); // worldz0 is applied BEFORE the full local inverse.
    const glm::vec3 previous {7, 8, 9};
    REQUIRE (particleMouseControlPoint (*world, false, glm::mat4 (0), false, previous) == previous);
    REQUIRE (glm::distance (particleMouseControlPoint (
        *world, true, glm::mat4 (0), false, previous), expectedWorld) < 0.001f);
    REQUIRE_FALSE (particleMouseWorldPosition (uv, glm::mat4 (0), view));
    REQUIRE_FALSE (particleMouseWorldPosition (
        {std::numeric_limits<float>::quiet_NaN (), 0.5f}, nativeProjection, view));

    // Native root orthographic depth is fixed to +/-2000 rather than the
    // renderer's authored far10000. A tilted view couples that depth into XY.
    glm::mat4 nativeOrtho (1);
    nativeOrtho[0][0] = 1.0f / 160.0f;
    nativeOrtho[1][1] = 1.0f / 120.0f;
    nativeOrtho[2][2] = 1.0f / 4000.0f;
    nativeOrtho[3][2] = 0.5f;
    const auto expectedRaw = glm::inverse (nativeOrtho * view)
        * glm::vec4 (uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    const auto orthoWorld = particleMouseWorldPosition (
        uv, particleMouseProjection (
            glm::ortho (-160.0f, 160.0f, -120.0f, 120.0f, 0.0f, 10000.0f), true), view);
    REQUIRE (orthoWorld);
    REQUIRE (glm::distance (*orthoWorld,
        glm::vec3 (expectedRaw.x / expectedRaw.w, expectedRaw.y / expectedRaw.w, 0)) < 0.001f);
}

TEST_CASE ("Perspective particle presentation preserves culling facing and authored mirrors",
           "[scene][particle][perspective][culling]") {
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto flip = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const auto projection = glm::perspective (glm::radians (50.0f), 4.0f / 3.0f, 100.0f, 1000.0f);
    const auto view = glm::lookAt (glm::vec3 (30, 40, 500), glm::vec3 (0), glm::vec3 (0.1f, 1, 0));
    const auto signedArea = [] (const glm::mat4& matrix) {
        glm::vec2 points[3];
        const glm::vec3 vertices[] {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}};
        for (int i = 0; i < 3; ++i) {
            const auto clip = matrix * glm::vec4 (vertices[i], 1);
            points[i] = glm::vec2 (clip) / clip.w;
        }
        const auto a = points[1] - points[0], b = points[2] - points[0];
        return a.x * b.y - a.y * b.x;
    };
    for (bool world : {false, true}) {
        for (float authoredScaleX : {-0.7f, 0.7f}) {
            const auto node = glm::translate (glm::mat4 (1), glm::vec3 (-60, -40, 0))
                * glm::scale (glm::mat4 (1), glm::vec3 (authoredScaleX, 0.8f, 1)) * flip;
            const auto draw = world ? flip : node;
            const float oldArea = signedArea (projection * view * draw);
            const float correctedArea = signedArea (
                particlePerspectiveViewProjection (flip * projection, view, world) * draw);
            const bool reversed = particlePresentationReversesWinding (false, world, false, false);
            // Change only the presentation-facing convention. Mirrored nodes
            // retain their authored opposite facing instead of being repaired.
            REQUIRE (oldArea * correctedArea * (reversed ? -1.0f : 1.0f) > 0);
        }
    }
    REQUIRE_FALSE (particlePresentationReversesWinding (true, false, false, false));
    REQUIRE_FALSE (particlePresentationReversesWinding (true, true, false, false));
    REQUIRE_FALSE (particlePresentationReversesWinding (false, false, true, false));
    REQUIRE_FALSE (particlePresentationReversesWinding (false, true, true, false));
    REQUIRE (particlePresentationReversesWinding (false, false, true, true));
    REQUIRE_FALSE (particlePresentationReversesWinding (false, true, true, true));
    REQUIRE_FALSE (particlePresentationReversesWinding (true, false, true, true));
}

TEST_CASE ("Model canvas projection preserves native MVP screen positions", "[scene][model-canvas]") {
    using WallpaperEngine::Render::Wallpapers::sceneAuthoredToCamera;
    const glm::mat4 authoredWorld = glm::translate (glm::mat4 (1), glm::vec3 (384, 100, 0))
        * glm::scale (glm::mat4 (1), glm::vec3 (.2f));
    const auto projection = glm::ortho (-384.f, 384.f, -216.f, 216.f, -2000.f, 2000.f);
    const auto mvp = projection * sceneAuthoredToCamera (768, 432, true) * authoredWorld;
    // Original MODEL draw116/120 captured matrix rows. Native D3D maps clip
    // Y down; the Linux wallpaper's final presentation maps GL clip Y up.
    const glm::vec4 nativeRows[2] = {
        {.0005208333604969084f, 0, 0, 0},
        {0, .0009259259677492082f, 0, -.5370370149612427f},
    };
    for (const auto point : {glm::vec3 (0), glm::vec3 (-24, -24, 0), glm::vec3 (24, 24, 0)}) {
        const auto p = glm::vec4 (point, 1);
        const auto clip = mvp * p;
        const glm::vec2 nativePixel ((1 + glm::dot (nativeRows[0], p)) * 640,
                                    (1 - glm::dot (nativeRows[1], p)) * 360);
        REQUIRE ((1 + clip.x) * 640 == Catch::Approx (nativePixel.x).margin (.0001));
        REQUIRE ((1 + clip.y) * 360 == Catch::Approx (nativePixel.y).margin (.0001));
    }
    REQUIRE (authoredWorld[3] == glm::vec4 (384, 100, 0, 1));
    // Perspective shaders already use native world coordinates.
    REQUIRE (sceneAuthoredToCamera (768, 432, false) == glm::mat4 (1));
}

TEST_CASE ("Model canvas bridge composes with child scope projection and parent shear", "[scene][model-canvas]") {
    using WallpaperEngine::Render::Wallpapers::sceneAuthoredToCamera;
    const auto bridge = sceneAuthoredToCamera (768, 432, true);
    const auto parent = glm::translate (glm::mat4 (1), glm::vec3 (400, 150, 0))
        * glm::rotate (glm::mat4 (1), .35f, glm::vec3 (0, 0, 1))
        * glm::scale (glm::mat4 (1), glm::vec3 (1.4f, .7f, 1));
    const auto local = glm::translate (glm::mat4 (1), glm::vec3 (20, 15, 0))
        * glm::rotate (glm::mat4 (1), -.4f, glm::vec3 (0, 0, 1));
    const auto authored = parent * local;
    const auto localFlip = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const auto childScope = glm::ortho (-80.f, 80.f, -40.f, 40.f, -1000.f, 1000.f)
        * localFlip * glm::inverse (bridge * parent);
    const auto modelVP = childScope * bridge;
    const auto expectedLocal = glm::ortho (-80.f, 80.f, -40.f, 40.f, -1000.f, 1000.f)
        * localFlip * local;
    for (const auto point : {glm::vec4 (0, 0, 0, 1), glm::vec4 (12, -8, 0, 1)}) {
        const auto actual = modelVP * authored * point;
        const auto expected = expectedLocal * point;
        for (int axis = 0; axis < 4; ++axis)
            REQUIRE (actual[axis] == Catch::Approx (expected[axis]).margin (.00001));
    }
}

TEST_CASE ("Model presentation culling retains authored mirrored facing", "[scene][model-canvas]") {
    using WallpaperEngine::Render::Wallpapers::modelPresentationReversesWinding;
    using WallpaperEngine::Render::Wallpapers::sceneAuthoredToCamera;
    const glm::vec4 triangle[] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 1, 0, 1}};
    const auto area = [&] (const glm::mat4& m) {
        const auto a = glm::vec2 (m * triangle[1] - m * triangle[0]);
        const auto b = glm::vec2 (m * triangle[2] - m * triangle[0]);
        return a.x * b.y - a.y * b.x;
    };
    for (bool childScope : {false, true}) {
        for (float mirror : {-1.f, 1.f}) {
            const auto world = glm::scale (glm::mat4 (1), glm::vec3 (mirror, .7f, 1));
            const auto projection = childScope
                ? glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1))
                : sceneAuthoredToCamera (768, 432, true);
            const bool reverse = modelPresentationReversesWinding (true, childScope);
            const float authoredFacing = area (world);
            const float presentedFacing = area (projection * world) * (reverse ? -1 : 1);
            REQUIRE (authoredFacing * presentedFacing > 0);
        }
    }
    // Perspective child composition has no authored canvas/local Y reflection.
    REQUIRE_FALSE (modelPresentationReversesWinding (false, true));
    REQUIRE (modelPresentationReversesWinding (false, false));
}

TEST_CASE ("Secondary projection preserves canvas offsets and reflects the view input",
           "[render][secondary-reflection]") {
    using WallpaperEngine::Render::Wallpapers::secondarySceneProjection;
    // Native captured orthographic MODEL stage/main both have these clip
    // coordinates. Canvas translation must survive the two Y sign changes.
    auto canvas = glm::ortho (0.0f,768.0f,0.0f,432.0f,-2000.0f,2000.0f);
    const auto reflected = secondarySceneProjection (glm::mat4 (1), canvas, glm::mat4 (1));
    const auto point = reflected * glm::vec4 (384,100,0,1);
    REQUIRE (point.x == Catch::Approx (0));
    REQUIRE (point.y == Catch::Approx (-0.537037037f));
    // A translated view is not a global clip Y reversal: X translation
    // survives, the reflected output Y translation changes its sign.
    auto view = glm::translate (glm::mat4 (1), glm::vec3 (5,7,0));
    const auto posed = secondarySceneProjection (glm::mat4 (1), canvas, view) * glm::vec4 (384,100,0,1);
    REQUIRE (posed.x == Catch::Approx (10.0f/768));
    REQUIRE (posed.y == Catch::Approx (186.0f/432-1));
    // Source toggles a single element, not the whole Y column. Oblique terms
    // are retained; this guards accidental broad conjugation of projection.
    glm::mat4 oblique (1); oblique[1][0] = .25f; oblique[3][1] = .3f;
    const auto projected = secondarySceneProjection (glm::mat4 (1), oblique, glm::mat4 (1))
        * glm::vec4 (2,4,0,1);
    REQUIRE (projected.x == Catch::Approx (1));
    REQUIRE (projected.y == Catch::Approx (4.3f));
}

TEST_CASE ("Secondary projection uses the camera's normalized render basis",
           "[render][secondary-reflection]") {
    using WallpaperEngine::Render::Camera;
    using namespace WallpaperEngine::Render::Wallpapers;
    const auto flip = glm::scale (glm::mat4 (1), glm::vec3 (1,-1,1));
    // Exercise the same camera builders and authored-point adapter consumed
    // by CScene/CModel. Native camera-space values are normalized at both
    // ends of the orthographic view; perspective keeps the native view.
    for (bool orthographic : {false,true}) {
        const Camera::Transforms transforms {{{15,23,200},{3,-9,0},{.2f,1,.1f}},1.3f};
        const auto nativeView = glm::lookAt (transforms.pose.eye,
            transforms.pose.center, transforms.pose.up);
        const auto renderView = Camera::renderLookAtForTransforms (transforms.pose,orthographic,768,432);
        const auto projection = Camera::makeProjectionForTransforms (
            768,432,60,.1f,1000,transforms,orthographic,1.2f);
        const auto bridge = sceneAuthoredToCamera (768,432,orthographic);
        const auto nativeProjection = flip*projection*bridge;
        const auto point = glm::vec4 (384,100,17,1);
        const auto nativeInput = point;
        auto nativeReflectedProjection = nativeProjection;
        nativeReflectedProjection[1][1] *= -1;
        const auto nativeReflected = nativeReflectedProjection*nativeView*flip*nativeInput;
        const auto nativeToRenderClip = flip;
        const auto expected = nativeToRenderClip*nativeReflected;
        const auto actual = secondarySceneProjectionForCamera (glm::mat4 (1),projection,renderView,bridge)
            *sceneAuthoredToCamera (768,432,orthographic)*point;
        for (int i=0;i<4;++i)
            REQUIRE (actual[i] == Catch::Approx (expected[i]).margin (1e-5f));
    }
}

TEST_CASE ("Camera shake follows the native deterministic scene-clock vector", "[scene][camera][shake]") {
    using WallpaperEngine::Render::Wallpapers::cameraShakeOffset;
    // 2.8.42: (cos p, sin 1.333p, sin p) at p = speed^2 *,
    // scaled by amplitude/10; orthographic drops z and multiplies by height/10.
    // At p = 0 only x is non-zero, so the scale is visible directly.
    const auto origin = cameraShakeOffset (2.75f, 0.15f, 1.0f, 0.0f, true, 2160.0f);
    REQUIRE (origin.x == Catch::Approx (2160.0f * 0.01f * 0.15f));
    REQUIRE (origin.y == 0.0f);
    REQUIRE (origin.z == 0.0f);
    const auto perspective = cameraShakeOffset (1.0f, 0.5f, 1.0f, glm::half_pi<float> (), false, 2160.0f);
    REQUIRE (perspective.x == Catch::Approx (0.0f).margin (1e-7f));
    REQUIRE (perspective.y == Catch::Approx (0.05f * std::sin (1.333f * glm::half_pi<float> ())));
    REQUIRE (perspective.z == Catch::Approx (0.05f));
    const auto orthographic = cameraShakeOffset (1.0f, 0.5f, 1.0f, glm::half_pi<float> (), true, 1000.0f);
    REQUIRE (orthographic.z == 0.0f);
    REQUIRE (orthographic.y == Catch::Approx (perspective.y * 1000.0f * 0.1f));
    // Speed is squared before multiplying the clock; no random phase or seed.
    REQUIRE (cameraShakeOffset (2.0f, 0.5f, 1.0f, 0.3f, false, 0.0f)
             == cameraShakeOffset (1.0f, 0.5f, 1.0f, 1.2f, false, 0.0f));
}

TEST_CASE ("Camera shake roughness reshapes only the vector length", "[scene][camera][shake]") {
    using WallpaperEngine::Render::Wallpapers::cameraShakeOffset;
    const float time = 0.37f;
    const auto base = cameraShakeOffset (3.0f, 0.5f, 1.0f, time, false, 0.0f);
    // The cubed roughness must exceed 0.001: small and zero roughness keep the
    // raw vector instead of normalizing it.
    REQUIRE (cameraShakeOffset (3.0f, 0.5f, 0.0f, time, false, 0.0f) == base);
    REQUIRE (cameraShakeOffset (3.0f, 0.5f, 0.05f, time, false, 0.0f) == base);
    // powf(0.1f, 3) is exactly 0.001f, which the native gate (r > 0.001) also
    // skips (bundled ricepod uses 0.1). Negative roughness cubes negative.
    REQUIRE (std::pow (0.1f, 3.0f) == 0.001f);
    REQUIRE (cameraShakeOffset (3.0f, 0.5f, 0.1f, time, false, 0.0f) == base);
    REQUIRE (cameraShakeOffset (3.0f, 0.5f, -1.0f, time, false, 0.0f) == base);
    REQUIRE (cameraShakeOffset (3.0f, 0.5f, -2.0f, time, true, 1080.0f)
             == cameraShakeOffset (3.0f, 0.5f, 1.0f, time, true, 1080.0f));
    REQUIRE (glm::length (cameraShakeOffset (3.0f, 0.5f, 0.11f, time, false, 0.0f) - base) > 1e-4f);
    for (bool orthographic : {false, true}) {
        const auto raw = cameraShakeOffset (3.0f, 0.5f, 1.0f, time, orthographic, 1080.0f);
        const float scale = orthographic ? 1080.0f * 0.1f * 0.05f : 0.05f;
        for (float roughness : {0.5f, 2.0f}) {
            const auto shaped = cameraShakeOffset (3.0f, 0.5f, roughness, time, orthographic, 1080.0f);
            const float length = glm::length (raw) / scale;
            REQUIRE (glm::length (shaped) / scale
                     == Catch::Approx (std::pow (length, std::pow (roughness, 3.0f))));
            REQUIRE (glm::dot (glm::normalize (shaped), glm::normalize (raw)) == Catch::Approx (1.0f));
        }
    }
}

TEST_CASE ("Untransformed orthographic shake translates the native view", "[scene][camera][shake]") {
    using WallpaperEngine::Render::Camera;
    using WallpaperEngine::Render::Wallpapers::sceneAuthoredToCamera;
    const auto flip = glm::scale (glm::mat4 (1), glm::vec3 (1, -1, 1));
    const Camera::Pose root {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    const auto projection = Camera::makeOrthogonalProjectionForScene (768, 432, 0, 1000, root.eye);
    const auto bridge = sceneAuthoredToCamera (768, 432, true);
    // Without shake the root view keeps its previous exact value.
    REQUIRE (Camera::untransformedOrthogonalRenderLookAt (root, Camera::Shake {true, glm::vec3 (0)}, 768, 432)
             == glm::lookAt (root.eye, root.center, root.up));
    const glm::vec3 shake {5, 7, 0};
    const Camera::Pose shaken {root.eye + shake, root.center + shake, root.up};
    const auto view = Camera::untransformedOrthogonalRenderLookAt (shaken, Camera::Shake {true, shake}, 768, 432);
    // Native clip is P_native * lookAt(shaken eye, shaken center) on authored
    // canvas points; Linux presents the same clip through one Y flip.
    const auto point = glm::vec4 (384, 100, 0, 1);
    const auto expected = flip * (flip * projection * bridge) * glm::lookAt (shaken.eye, shaken.center, shaken.up) * point;
    const auto actual = projection * view * bridge * point;
    for (int i = 0; i < 4; ++i) REQUIRE (actual[i] == Catch::Approx (expected[i]).margin (1e-6f));
    // The untransformed and transformed paths agree once a pose is shaken.
    const auto transformed = Camera::renderLookAtForTransforms (shaken, true, 768, 432) * bridge * point;
    for (int i = 0; i < 4; ++i) REQUIRE ((view * bridge * point)[i] == Catch::Approx (transformed[i]).margin (1e-4f));
}

TEST_CASE ("Orthographic flag-4 particles keep the frame shake of the copied root view", "[scene][camera][shake][particle]") {
    using WallpaperEngine::Render::Camera;
    // Native copies * (lookAt of the shaken eye/center)
    // rewrites only translation z, so a z = 0 particle shifts on
    // screen exactly as the orthographic scene layer does.
    const float width = 1280, height = 720, eye = 450.0f;
    const auto view0 = Camera::flag4OrthogonalView (eye, Camera::Shake {}, width, height);
    REQUIRE (view0 == glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, -eye)));
    const Camera::Shake shake {true, {9, -4, 0}};
    const auto view = Camera::flag4OrthogonalView (eye, shake, width, height);
    REQUIRE (view[3][2] == -eye);
    const auto ortho = Camera::makeOrthogonalProjectionForScene (width, height, 0, 1000, glm::vec3 (0));
    const float fov = 2.0f * std::atan (1.0f / (eye * ortho[1][1]));
    const auto perspective = glm::perspective (fov, width / height, 0.1f, 5000.0f);
    const Camera::Pose root {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    const auto layerView = Camera::untransformedOrthogonalRenderLookAt (shake.apply (root), shake, width, height);
    for (const glm::vec4 point : {glm::vec4 (0, 0, 0, 1), glm::vec4 (-300, 170, 0, 1)}) {
        const auto layer0 = ortho * point, layer = ortho * layerView * point;
        const auto particle0 = perspective * view0 * point, particle = perspective * view * point;
        for (int axis = 0; axis < 2; ++axis)
            REQUIRE (particle[axis] / particle.w - particle0[axis] / particle0.w
                     == Catch::Approx (layer[axis] / layer.w - layer0[axis] / layer0.w).margin (1e-5f));
    }
}

TEST_CASE ("Orthographic cursor unprojection uses the drawn shaken view", "[scene][camera][shake][cursor]") {
    using WallpaperEngine::Render::Camera;
    using WallpaperEngine::Render::Wallpapers::particleMouseProjection;
    using WallpaperEngine::Render::Wallpapers::particleMouseWorldPosition;
    // Native unprojects with inverse,
    // the same shaken view it draws with, so a cursor-linked point stays under
    // the cursor while the scene shakes.
    const float width = 1280, height = 720;
    const auto projection = Camera::makeOrthogonalProjectionForScene (width, height, 0, 1000, glm::vec3 (0));
    const Camera::Pose root {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    const auto rootLookAt = glm::lookAt (root.eye, root.center, root.up);
    REQUIRE (Camera::cursorLookAtForShake (root, rootLookAt, Camera::Shake {}, true, width, height) == rootLookAt);
    const Camera::Shake shake {true, {6, 11, 0}};
    const auto pose = shake.apply (root);
    const auto drawn = Camera::untransformedOrthogonalRenderLookAt (pose, shake, width, height);
    const auto cursorView = Camera::cursorLookAtForShake (pose, glm::lookAt (pose.eye, pose.center, pose.up),
                                                          shake, true, width, height);
    REQUIRE (cursorView == drawn);
    const glm::vec2 uv {0.3f, 0.65f};
    const auto world = particleMouseWorldPosition (uv, particleMouseProjection (projection, true), cursorView);
    REQUIRE (world);
    const auto clip = projection * drawn * glm::vec4 (*world, 1.0f);
    REQUIRE (clip.x / clip.w == Catch::Approx (uv.x * 2 - 1).margin (1e-5f));
    REQUIRE (clip.y / clip.w == Catch::Approx (1 - uv.y * 2).margin (1e-5f));
    // The unrendered lookAt would leave the point 2*s.y away from the cursor.
    const auto stale = particleMouseWorldPosition (uv, particleMouseProjection (projection, true),
                                                   glm::lookAt (pose.eye, pose.center, pose.up));
    REQUIRE (stale->y - world->y == Catch::Approx (2 * shake.offset.y));
    REQUIRE (stale->x == Catch::Approx (world->x));
}
