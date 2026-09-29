#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

using WallpaperEngine::Data::Builders::UserSettingBuilder;
using WallpaperEngine::Data::Model::Object;
using WallpaperEngine::Data::Model::ObjectData;
using WallpaperEngine::Render::Wallpapers::resolveSceneTransform;
using WallpaperEngine::Render::Wallpapers::inverseFiniteTransform;
using WallpaperEngine::Render::Wallpapers::projectWorldControlPoint;
using WallpaperEngine::Render::Wallpapers::sceneParallaxOffset;
using WallpaperEngine::Render::Wallpapers::sceneParallaxDelayWeight;
using WallpaperEngine::Render::Wallpapers::sceneParticleParallaxOffset;

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

TEST_CASE ("Scene layer parallax keeps zero depth fixed and signed depths symmetric", "[scene][parallax]") {
    // 2902406982 authors both depth-zero triangle/text pairs and a
    // negative-X-depth triangle/text pair. Camera amount is already included
    // in the displacement passed to this helper.
    const glm::vec2 displacement {0.125f, -0.0625f};
    REQUIRE (sceneParallaxOffset ({0.0f, 0.0f}, displacement, 3840.0f)
             == glm::vec3 (0.0f));
    const glm::vec3 positive = sceneParallaxOffset ({0.1f, 0.1f}, displacement, 3840.0f);
    const glm::vec3 negative = sceneParallaxOffset ({-0.1f, 0.0f}, displacement, 3840.0f);
    REQUIRE (positive.x == Catch::Approx (-48.0f));
    REQUIRE (positive.y == Catch::Approx (24.0f));
    REQUIRE (negative.x == Catch::Approx (48.0f));
    REQUIRE (negative.y == Catch::Approx (0.0f));
}

TEST_CASE ("Scene parallax delay follows native per-frame smoothing", "[scene][parallax]") {
    // Native 1401891a0 uses (1 - delay/3)*10*frameDelta for positive
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

TEST_CASE ("Particle parallax uses authored depth and camera-relative origin", "[scene][parallax][particle]") {
    const glm::vec3 origin {1802.36865f, 917.86761f, 0.0f};
    const glm::vec3 eye {-103.60931f, 120.87753f, 0.0f};
    const glm::vec2 displacement {0.125f, -0.0625f};
    const auto offset = [&] (glm::vec2 depth) {
        return sceneParticleParallaxOffset (origin, eye, depth, displacement,
                                            3840.0f, 2160.0f, 0.5f);
    };
    // The original 2902406982 particle authors zero depth on both axes.
    REQUIRE (offset ({0.0f, 0.0f}) == glm::vec3 (0.0f));
    const auto centered = sceneParticleParallaxOffset (origin, eye, {0.1f, 0.1f},
                                                       {0.0f, 0.0f}, 3840.0f, 2160.0f, 0.5f);
    REQUIRE (centered.x == Catch::Approx (-0.701102f));
    REQUIRE (centered.y == Catch::Approx (-14.150496f));
    const auto positive = offset ({0.1f, 0.1f});
    const auto negative = offset ({-0.1f, -0.1f});
    REQUIRE (positive.x == Catch::Approx ((1802.36865f - 1816.39069f) * 0.05f - 48.0f));
    REQUIRE (positive.y == Catch::Approx ((917.86761f - 1200.87753f) * 0.05f + 13.5f));
    REQUIRE (negative == -positive);
    REQUIRE (offset ({0.2f, 0.2f}) == positive * 2.0f);
}

TEST_CASE ("Unparented scene parallax precedes rotated and mirrored object transforms", "[scene][parallax][particle]") {
    // The 2334035201 mouth smoke has Z angle ~pi and the left fog has a
    // negative X scale. Native 14018aac0 moves the scene stack before either
    // object's renderer applies those local transforms.
    const glm::vec3 eye {-47.67039f, 204.18417f, 0.0f};
    const glm::vec2 center {0.0f};
    const glm::vec2 right {0.0144f, 0.0f}; // centered cursor .5 * amount .18 * influence .16
    const auto offset = [&] (const glm::vec3& origin, const glm::vec2& displacement) {
        return sceneParticleParallaxOffset (
            origin, eye, {1.0f, 1.0f}, displacement, 1920.0f, 1080.0f, 0.18f);
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
    const auto smokeCenter = offset ({943.53278f, 637.84656f, 0.0f}, center);
    REQUIRE (smokeCenter.x == Catch::Approx (5.61657f).margin (0.002f));
    REQUIRE (smokeCenter.y == Catch::Approx (-19.14077f).margin (0.002f));
    // The native cursor uses scene height on Y; the old image/text helper
    // used scene width and separated them from particles on a 16:9 canvas.
    const auto characterCenter = offset ({960.0f, 540.0f, 0.0f}, center);
    const auto characterTop = offset ({960.0f, 540.0f, 0.0f}, {0.0f, 0.0144f});
    REQUIRE (characterCenter.x == Catch::Approx (8.58067f).margin (0.002f));
    REQUIRE (characterCenter.y == Catch::Approx (-36.75315f).margin (0.002f));
    REQUIRE ((characterTop - characterCenter).y == Catch::Approx (-15.552f).margin (0.001f));
    REQUIRE (sceneParticleParallaxOffset ({943.53278f, 637.84656f, 0.0f}, eye,
             {0.0f, 0.0f}, right, 1920.0f, 1080.0f, 0.18f) == glm::vec3 (0.0f));
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
