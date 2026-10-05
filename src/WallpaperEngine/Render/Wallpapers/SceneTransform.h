#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Render::Wallpapers {

// Native allocates the root target at output dimensions, while
// applies fit/fill to the authored orthographic projection. Move
// the presentation's sampled UV window into clip space so that the final
// root copy does not resample an authored-resolution scene image.
inline glm::mat4 scenePresentationClipTransform (const glm::vec4& uvWindow) {
    const float left = std::min (uvWindow.x, uvWindow.y);
    const float right = std::max (uvWindow.x, uvWindow.y);
    const float bottom = std::min (uvWindow.z, uvWindow.w);
    const float top = std::max (uvWindow.z, uvWindow.w);
    const float width = right - left;
    const float height = top - bottom;
    if (!(width > 0.0f && height > 0.0f)
        || !std::isfinite (width) || !std::isfinite (height)) return glm::mat4 (1.0f);
    glm::mat4 result (1.0f);
    result[0][0] = 1.0f / width;
    result[1][1] = 1.0f / height;
    result[3][0] = (1.0f - left - right) / width;
    result[3][1] = (1.0f - bottom - top) / height;
    return result;
}

// Scene authored coordinates are Y-down and angles are radians. Multiplying
// local matrices through the parent chain retains affine shear; native
// attachment/bone adjustments and perspective still need separate handling.
struct ResolvedSceneTransform {
    glm::vec3 origin {0.0f};
    glm::vec3 scale {1.0f};
    float angle = 0.0f;
    bool visible = true;
    glm::mat4 authoredMatrix {1.0f};
};

// Native scene object matrices retain authored XYZ. Orthographic image/text
// geometry uses a Y-down authored canvas, which Linux centers and flips for
// its GL projection; the native perspective branch projects raw world XYZ.
inline glm::vec3 scenePointForCamera (
    const glm::vec3& authored, float sceneWidth, float sceneHeight, bool orthographic
) {
    if (!orthographic) return authored;
    return {authored.x - sceneWidth * 0.5f,
            sceneHeight * 0.5f - authored.y, authored.z};
}

// Model shaders consume their authored world matrix separately. Move the
// orthographic canvas-to-GL conversion into VP, leaving model/normal uniforms
// in the native authored basis (including parent shear).
inline glm::mat4 sceneAuthoredToCamera (float width, float height, bool orthographic) {
    if (!orthographic) return glm::mat4 (1.0f);
    return glm::translate (glm::mat4 (1.0f), glm::vec3 (-width * 0.5f, height * 0.5f, 0))
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (1, -1, 1));
}

// Native reflects the view's input Y and reverses only projection
// element [1][1] for the D3D output convention, retaining canvas translation.
inline glm::mat4 secondarySceneProjection (
    const glm::mat4& clip, glm::mat4 projection, const glm::mat4& view
) {
    projection[1][1] = -projection[1][1];
    return clip * projection * view
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (1, -1, 1));
}

// Reconstruct the native canvas projection/view before its single-element
// projection toggle. The centered GL view includes canvas translation,
// which must not change the authored pivot of the reflected stage.
inline glm::mat4 secondarySceneProjectionForCamera (
    const glm::mat4& clip, const glm::mat4& projection,
    const glm::mat4& view, const glm::mat4& authoredToCamera
) {
    const auto flip = glm::scale (glm::mat4 (1), glm::vec3 (1,-1,1));
    const auto inverseBridge = glm::inverse (authoredToCamera);
    auto nativeProjection = flip * projection * authoredToCamera;
    nativeProjection[1][1] *= -1;
    const auto nativeView = inverseBridge * view * authoredToCamera;
    return clip * flip * nativeProjection * nativeView * flip * inverseBridge;
}

inline bool modelPresentationReversesWinding (bool orthographic, bool localProjection) {
    return orthographic || !localProjection;
}

// Native translates the scene view stack before
// each object draw by
//   (root.origin.xy - parallaxPos) * cameraparallaxamount * root.parallaxDepth
// in the native canvas (origin bottom-left, +Y up on screen), and only when
// the scene has both camera parallax and an orthographic projection
// (flags & 0x108). forms
//   parallaxPos = size*0.5*(1-influence) + size*(x, 1-y)*influence + eye
// from a top-origin cursor. Linux keeps the cursor in its top-origin pointer
// convention and stores only (cursor - 0.5) * amount * influence as the
// displacement, so the canvas cursor Y term is +displacement.y * height.
// The result is returned in Linux's centered orthographic camera space, whose
// Y axis is the negated canvas axis (scenePointForCamera); X is unchanged.
inline glm::vec3 sceneLayerParallaxOffset (
    const glm::vec3& rootOrigin, const glm::vec3& cameraEye,
    const glm::vec2& rootDepth, const glm::vec2& displacement,
    float sceneWidth, float sceneHeight, float cameraAmount,
    bool cameraParallax, bool orthographic
) {
    if (!cameraParallax || !orthographic) return {};
    const float canvasX = ((rootOrigin.x - cameraEye.x - sceneWidth * 0.5f) * cameraAmount
        - displacement.x * sceneWidth) * rootDepth.x;
    const float canvasY = ((rootOrigin.y - cameraEye.y - sceneHeight * 0.5f) * cameraAmount
        + displacement.y * sceneHeight) * rootDepth.y;
    return {canvasX, -canvasY, 0.0f};
}

// Native walks parent links to the top-most ancestor of any
// object type (groups included); a missing parent ends the walk. A malformed
// cycle stops after maxHops links.
template <typename FindParent>
const Data::Model::Object& sceneParallaxRoot (
    const Data::Model::Object& object, FindParent&& findParent, size_t maxHops
) {
    const Data::Model::Object* root = &object;
    for (size_t hops = 0; root->parent.has_value () && hops < maxHops; ++hops) {
        const Data::Model::Object* parent = findParent (*root->parent);
        if (parent == nullptr) break;
        root = parent;
    }
    return *root;
}

// The nearest ancestor that owns an offscreen child draw scope (a composing
// image), across ordinary transform-only ancestors; nested scopes resolve to
// the nearest one. A missing ancestor or a malformed cycle ends the walk.
template <typename FindParent, typename Composes>
std::optional<int> sceneCompositionOwner (
    const Data::Model::Object& object, FindParent&& findParent, Composes&& composes
) {
    std::unordered_set<int> seen;
    auto parent = object.parent;
    while (parent && seen.insert (*parent).second) {
        const Data::Model::Object* ancestor = findParent (*parent);
        if (ancestor == nullptr) return std::nullopt;
        if (composes (*parent)) return parent;
        parent = ancestor->parent;
    }
    return std::nullopt;
}

// The draw scope inside composing images. Every composer draws its children
// into its own offscreen target, but only a non-fullscreen one replaces the
// scene stacks (native: inverse own transform, identity view,
// +-size/2 ortho). A fullscreen composer (& 0x2, set from the model's
// "fullscreen") keeps them, so its children otherwise
// draw exactly as root objects: same projection, root clip, winding and
// presentation. A local projection, once entered, persists through nesting.
struct SceneCompositionScope {
    bool offscreenTarget = false;
    bool localProjection = false;
};

inline SceneCompositionScope sceneCompositionChildScope (SceneCompositionScope outer, bool fullscreenComposer) {
    return {true, outer.localProjection || !fullscreenComposer};
}

// A fullscreen passthrough's copy quad is clip space. It copies the current
// target 1:1 wherever the
// scene stacks are in effect. The local-projection exception (scene MVP) has
// no native counterpart: native gates its passthrough identity copy on 0x20
// alone; it carries over the c337199 behaviour, which no installed scene hits.
inline bool sceneFullscreenCopyInClipSpace (bool fullscreenPassthrough, SceneCompositionScope scope) {
    return fullscreenPassthrough && !scope.localProjection;
}

// Native draws a non-fullscreen composer's children in its
// local space with an identity view, so they take no camera parallax there and
// reach the screen through the composer's composite, which carries the shared
// root offset once. A fullscreen composer keeps the scene stacks unchanged: its
// children keep the scene view's root translation and its composite does not
// A child is therefore excluded from parallax iff
// any composer above it is not fullscreen.
template <typename FindParent, typename Composes, typename Fullscreen>
bool sceneCompositionSuppressesParallax (
    const Data::Model::Object& object, FindParent&& findParent, Composes&& composes, Fullscreen&& fullscreen
) {
    std::unordered_set<int> seen;
    const Data::Model::Object* current = &object;
    while (auto owner = sceneCompositionOwner (*current, findParent, composes)) {
        if (!seen.insert (*owner).second) return false;
        if (!fullscreen (*owner)) return true;
        current = findParent (*owner);
        if (current == nullptr) return false;
    }
    return false;
}

// Native initializes every object's depth to (1,1).
inline glm::vec2 sceneParallaxDepth (const Data::Model::Object& object) {
    return object.parallaxDepth ? object.parallaxDepth->value->getVec2 () : glm::vec2 (1.0f);
}

// Native maps the authored 0..3 delay slider to a per-frame
// interpolation weight. Zero/negative delay bypasses smoothing; positive
// values clamp only the upper bound, exactly as the native update does.
// The displacement sceneLayerParallaxOffset consumes, from the same
// top-origin cursor that advanceSceneParallaxPosition receives.
inline glm::vec2 sceneParallaxDisplacementTarget (glm::vec2 cursor, float amount, float influence) {
    return (cursor - glm::vec2 (0.5f)) * amount * influence;
}

// The cursor both parallax consumers receive. Native reads it as a
// fraction of the viewport (, the g_PointerPosition
// field) and scales it by the full projection size, so
// a cover-cropped canvas still sweeps its whole width. The scene cursor is
// instead remapped into the visible UV band; keep only that band's axis
// orientation, which leaves an uncropped scene unchanged.
inline glm::vec2 sceneParallaxCursor (glm::vec2 viewportCursor, float ustart, float uend, float vstart, float vend) {
    return {uend >= ustart ? viewportCursor.x : 1.0f - viewportCursor.x,
            vend >= vstart ? viewportCursor.y : 1.0f - viewportCursor.y};
}

// The eye term of native's eased parallax point (eases
// size*0.5*(1-infl) + size*cursor*infl + eye as one point, which is linear in
// each term). Written as prev + (eye - prev) * weight so an unchanged eye stays
// bit-exact; sceneLayerParallaxOffset takes it in place of the live eye.
inline glm::vec2 easeSceneParallaxEye (glm::vec2 previous, glm::vec2 eye, float weight) {
    return previous + (eye - previous) * weight;
}

inline float sceneParallaxDelayWeight (float delay, float frameDelta) {
    return delay > 0.0f ? std::min (1.0f, (1.0f - delay / 3.0f) * 10.0f * frameDelta) : 1.0f;
}

// Native translates the frame eye and center by one deterministic
// vector of the scene clock. Roughness is cubed into an
// exponent on the vector length; 1.0 leaves the direction and length as-is.
// Orthographic scenes drop z and scale by a tenth of the canvas height.
inline glm::vec3 cameraShakeOffset (
    float speed, float amplitude, float roughness, float sceneTime,
    bool orthographic, float orthographicHeight
) {
    const float exponent = std::pow (roughness, 3.0f);
    float scale = amplitude * 0.1f;
    const float phase = speed * speed * sceneTime;
    float x = std::cos (phase);
    float y = std::sin (phase * 1.333f);
    float z = std::sin (phase);
    if (orthographic) {
        z = 0.0f;
        scale = orthographicHeight * 0.1f * scale;
    }
    if (exponent > 0.001f && exponent != 1.0f) {
        const float length = std::sqrt (x * x + y * y + z * z);
        const float power = std::pow (length, exponent);
        const float inverse = 1.0f / length;
        x = inverse * x * power;
        y = inverse * y * power;
        z = inverse * z * power;
    }
    return {x * scale, y * scale, z * scale};
}

// Native g_ParallaxPosition (shader global 0x6b, bound by
// ). writes it only while cameraparallax is on,
// without an orthographic gate: the smoothed parallax point (the
// clamped top-origin cursor as (x, 1 - y), scaled by the mouse influence about
// the canvas centre, plus the eye, eased by the delay weight) divided by the
// projection size and clamped to [0,1]. The cursor is the same top-origin one
// sceneLayerParallaxOffset receives as a displacement. 
// starts the uniform at (0.5,0.5) and the point at eye + size/2 for an
// orthographic scene, 0 otherwise. Known divergence: native's size
// is 0 for a perspective scene, so its uniform is degenerate
// there; Linux divides by the scene size. Not modelled: the & 0x800
// mirror, which native applies twice (cursor x at :295, uniform x at :333), and
// the zero influence under & 0x200200; Linux has neither state.
struct SceneParallaxPosition {
    glm::vec2 point {0.0f};
    glm::vec2 uniform {0.5f};
};

inline SceneParallaxPosition initialSceneParallaxPosition (bool orthographic, glm::vec2 size, glm::vec2 eye) {
    return {orthographic ? eye + size * 0.5f : glm::vec2 (0.0f), glm::vec2 (0.5f)};
}

inline void advanceSceneParallaxPosition (
    SceneParallaxPosition& state, bool cameraParallax, glm::vec2 cursor, float influence,
    glm::vec2 size, glm::vec2 eye, float weight
) {
    if (!cameraParallax) return;
    const glm::vec2 clamped = glm::clamp (glm::vec2 (cursor.x, 1.0f - cursor.y), glm::vec2 (0.0f), glm::vec2 (1.0f));
    const glm::vec2 target = size * 0.5f * (1.0f - influence) + size * clamped * influence + eye;
    state.point += (target - state.point) * weight;
    if (size.x > 0.0f && size.y > 0.0f)
        state.uniform = glm::clamp (state.point / size, glm::vec2 (0.0f), glm::vec2 (1.0f));
}

inline std::optional<glm::mat4> inverseFiniteTransform (const glm::mat4& matrix) {
    // Float matrix multiplication can give a mathematically singular affine
    // hierarchy a tiny nonzero determinant after rotated zero-scale axes mix.
    // Compare the linear volume with its own axis lengths, so a uniformly
    // small but well-conditioned scale remains invertible.
    const bool affine = std::abs (matrix[0][3]) < 1e-6f && std::abs (matrix[1][3]) < 1e-6f
        && std::abs (matrix[2][3]) < 1e-6f && std::abs (matrix[3][3] - 1.0f) < 1e-6f;
    if (affine) {
        const glm::dvec3 x (matrix[0]);
        const glm::dvec3 y (matrix[1]);
        const glm::dvec3 z (matrix[2]);
        const double axisVolume = glm::length (x) * glm::length (y) * glm::length (z);
        const double actualVolume = glm::dot (x, glm::cross (y, z));
        if (!std::isfinite (axisVolume) || axisVolume == 0.0 ||
            !std::isfinite (actualVolume) || std::abs (actualVolume) <= axisVolume * 1e-6)
            return std::nullopt;
    }
    const float determinant = glm::determinant (matrix);
    if (!std::isfinite (determinant) || determinant == 0.0f) return std::nullopt;
    const glm::mat4 inverse = glm::inverse (matrix);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite (inverse[column][row])) return std::nullopt;
    return inverse;
}

// Projection for a composer's children. A fullscreen
// composer keeps the current scene projection, so children land 1:1 in screen
// space. Otherwise the children are drawn in the composer's local space: an
// orthographic box of the composer's authored size over
// the inverse of its world transform.
inline std::optional<glm::mat4> sceneCompositionChildProjection (
    bool fullscreenComposer, const glm::mat4& currentProjection,
    const glm::mat4& composerWorld, const glm::mat4& localFlip, glm::vec2 targetSize
) {
    if (fullscreenComposer) return currentProjection;
    const auto inverse = inverseFiniteTransform (composerWorld);
    if (!inverse) return std::nullopt;
    return glm::ortho (-targetSize.x * 0.5f, targetSize.x * 0.5f,
        -targetSize.y * 0.5f, targetSize.y * 0.5f, -1000.0f, 1000.0f) * localFlip * *inverse;
}

// Native CP matrices use the authored canvas, whereas orthographic Linux
// node stacks use centered GL world coordinates. Preserve the native CP0
// world/world exception before converting absolute world points.
inline glm::mat4 particleControlPointMatrix (
    const glm::mat4& authoredSimulation, const glm::mat4& simulationModel,
    const std::optional<glm::mat4>& simulationInverse,
    bool presetWorld, bool pointWorld, bool pointZero,
    bool orthographic, glm::vec2 canvasSize
) {
    if (presetWorld && (!pointWorld || pointZero))
        return simulationModel * authoredSimulation;
    if (!pointWorld) return authoredSimulation;
    const glm::mat4 world = orthographic
        ? glm::translate (glm::mat4 (1.0f), glm::vec3 (
            -canvasSize.x / 2.0f, canvasSize.y / 2.0f, 0.0f)) * authoredSimulation
        : authoredSimulation;
    return !presetWorld && simulationInverse ? *simulationInverse * world : world;
}

// A singular authored transform cannot map a world control point into
// simulation-local coordinates. Retain the last finite local value until the
// transform becomes invertible again, then reproject on the next tick.
inline glm::vec3 projectWorldControlPoint (
    const glm::mat4& preParallaxInverse, bool invertible,
    const glm::vec3& worldPoint, const glm::vec3& previousLocal
) {
    if (!invertible) return previousLocal;
    const glm::vec4 local = preParallaxInverse * glm::vec4 (worldPoint, 1.0f);
    if (!std::isfinite (local.x) || !std::isfinite (local.y) || !std::isfinite (local.z))
        return previousLocal;
    return glm::vec3 (local);
}

// Native use reversed Direct3D depth
// unprojects NDC Z=0 before replacing world Z with zero. Keep this mouse
// projection separate from the renderer's OpenGL depth convention.
inline glm::mat4 particleMouseProjection (const glm::mat4& cameraProjection, bool orthographic) {
    if (orthographic) {
        glm::mat4 result = cameraProjection;
        // uses fixed near=-2000, far=2000 for the root 2D view.
        result[0][2] = result[1][2] = 0.0f;
        result[2][2] = 1.0f / 4000.0f;
        result[3][2] = 0.5f;
        return result;
    }
    glm::mat4 reverseDepth (1.0f);
    reverseDepth[2][2] = -0.5f;
    reverseDepth[3][2] = 0.5f;
    return reverseDepth * cameraProjection;
}

inline std::optional<glm::vec3> particleMouseWorldPosition (
    glm::vec2 visibleUv, const glm::mat4& mouseProjection, const glm::mat4& view
) {
    if (!std::isfinite (visibleUv.x) || !std::isfinite (visibleUv.y))
        return std::nullopt;
    const auto inverse = inverseFiniteTransform (mouseProjection * view);
    if (!inverse) return std::nullopt;
    // Mouse UV already includes the authored Fit/Fill crop and the output's
    // V direction. The final scene quad reflects Y relative to that UV, so
    // invert it once before unprojecting the root view.
    const glm::vec4 projected = *inverse * glm::vec4 (
        visibleUv.x * 2.0f - 1.0f, 1.0f - visibleUv.y * 2.0f, 0.0f, 1.0f);
    if (!std::isfinite (projected.w) || projected.w == 0.0f) return std::nullopt;
    const glm::vec3 world = glm::vec3 (projected) / projected.w;
    if (!std::isfinite (world.x) || !std::isfinite (world.y) || !std::isfinite (world.z))
        return std::nullopt;
    return glm::vec3 (world.x, world.y, 0.0f);
}

inline glm::vec3 particleMouseControlPoint (
    const glm::vec3& world, bool presetWorld, const glm::mat4& nodeInverse,
    bool nodeInvertible, const glm::vec3& previous
) {
    // Authored CP offsets are replaced, not added. Native inverses only the
    // local preset's node, even when the mouse descriptor has its world bit.
    return presetWorld ? world : projectWorldControlPoint (nodeInverse, nodeInvertible, world, previous);
}

inline glm::mat4 particlePerspectiveViewProjection (
    const glm::mat4& presentationProjection, const glm::mat4& view, bool presetWorld
) {
    const glm::mat4 worldBasis = presetWorld
        ? glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)) : glm::mat4 (1.0f);
    // Native replaces the local node stack with the root world stack
    // for world presets. Their existing draw Y reflection needs a matching
    // view basis; local nodes retain their authored model and presentation VP.
    return presentationProjection * view * worldBasis;
}

inline bool particlePresentationReversesWinding (
    bool orthographic, bool presetWorld, bool localProjection, bool perspectiveOverride
) {
    // Preset bit4 uses the root perspective camera even under a local
    // projection; ordinary presets use the composer's local orthographic box.
    return !orthographic && !presetWorld && (!localProjection || perspectiveOverride);
}

inline ResolvedSceneTransform localSceneTransform (const Data::Model::Object& object) {
    ResolvedSceneTransform result;
    result.origin = object.origin->value->getVec3 ();
    if (object.is<Data::Model::Image> ()) {
        const auto& image = *object.as<Data::Model::Image> ();
        result.scale = image.scale->value->getVec3 ();
        result.angle = image.angles->value->getVec3 ().z;
        result.visible = image.visible->value->getBool ();
    } else if (object.is<Data::Model::Particle> ()) {
        const auto& particle = *object.as<Data::Model::Particle> ();
        result.scale = particle.scale->value->getVec3 ();
        result.angle = particle.angles->value->getVec3 ().z;
        result.visible = particle.visible->value->getBool ();
    } else if (object.is<Data::Model::Text> ()) {
        const auto& text = *object.as<Data::Model::Text> ();
        result.scale = text.scale->value->getVec3 ();
        result.angle = object.groupAngles->value->getVec3 ().z;
        result.visible = text.visible->value->getBool ();
    } else {
        result.scale = object.groupScale->value->getVec3 ();
        result.angle = object.groupAngles->value->getVec3 ().z;
        result.visible = object.groupVisible->value->getBool ();
    }
    // Native stores the full Rz*Ry*Rx basis for every layer.
    // scales those columns before parent composition; text's
    // uses this same basis for the measured layout offset.
    glm::vec3 angles;
    if (object.is<Data::Model::Image> ())
        angles = object.as<Data::Model::Image> ()->angles->value->getVec3 ();
    else if (object.is<Data::Model::Particle> ())
        angles = object.as<Data::Model::Particle> ()->angles->value->getVec3 ();
    else
        angles = object.groupAngles->value->getVec3 ();
    result.authoredMatrix = glm::translate (glm::mat4 (1.0f), result.origin);
    result.authoredMatrix = glm::rotate (result.authoredMatrix, angles.z, glm::vec3 (0, 0, 1));
    result.authoredMatrix = glm::rotate (result.authoredMatrix, angles.y, glm::vec3 (0, 1, 0));
    result.authoredMatrix = glm::rotate (result.authoredMatrix, angles.x, glm::vec3 (1, 0, 0));
    result.authoredMatrix = glm::scale (result.authoredMatrix, result.scale);
    return result;
}

template <typename FindParent, typename FindAttachment>
ResolvedSceneTransform resolveSceneTransform (
    const Data::Model::Object& object, FindParent&& findParent, FindAttachment&& findAttachment
) {
    std::vector<const Data::Model::Object*> chain {&object};
    std::unordered_set<const Data::Model::Object*> visited {&object};
    const auto* current = &object;
    while (current->parent) {
        const auto* parent = std::invoke (findParent, *current->parent);
        if (!parent) throw std::invalid_argument ("Missing parent in scene transform");
        if (!visited.insert (parent).second) throw std::invalid_argument ("Cycle in scene transform");
        chain.push_back (parent);
        current = parent;
    }
    auto result = localSceneTransform (*chain.back ());
    for (int i = static_cast<int> (chain.size ()) - 2; i >= 0; --i) {
        const auto local = localSceneTransform (*chain[i]);
        result.scale *= local.scale;
        result.angle += local.angle;
        result.visible &= local.visible;
        // Matrix multiplication retains the shear produced by nonuniform
        // parent scale followed by a rotated child.
        // Native child composition applies the parent's current global bone
        // and MDAT attachment offset before the child's authored local matrix.
        if (chain[i]->attachment && !chain[i]->attachment->empty ()) {
            const auto attachment = std::invoke (findAttachment, *chain[i + 1], *chain[i]->attachment);
            if (attachment) result.authoredMatrix *= *attachment;
        }
        result.authoredMatrix *= local.authoredMatrix;
        result.origin = glm::vec3 (result.authoredMatrix[3]);
    }
    return result;
}

template <typename FindParent>
ResolvedSceneTransform resolveSceneTransform (const Data::Model::Object& object, FindParent&& findParent) {
    return resolveSceneTransform (object, std::forward<FindParent> (findParent),
                                  [] (const Data::Model::Object&, const std::string&) {
                                      return std::optional<glm::mat4> {};
                                  });
}

} // namespace WallpaperEngine::Render::Wallpapers
