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

// Displacement already includes camera amount and mouse influence.
// Zero authored depth is fixed on screen.
inline glm::vec3 sceneParallaxOffset (
    const glm::vec2& depth, const glm::vec2& displacement, float sceneWidth
) {
    return {-depth.x * displacement.x * sceneWidth,
            -depth.y * displacement.y * sceneWidth, 0.0f};
}

// Native 14018aac0 forms a particle/object offset from its authored origin
// and the smoothed camera cursor, then adds it through the object's current
// matrix basis. The scene displacement stores only the cursor's deviation
// from center and already includes camera amount and mouse influence.
inline glm::vec3 sceneParticleParallaxOffset (
    const glm::vec3& origin, const glm::vec3& cameraEye,
    const glm::vec2& depth, const glm::vec2& displacement,
    float sceneWidth, float sceneHeight, float cameraAmount
) {
    return {
        ((origin.x - cameraEye.x - sceneWidth * 0.5f) * cameraAmount
            - displacement.x * sceneWidth) * depth.x,
        ((origin.y - cameraEye.y - sceneHeight * 0.5f) * cameraAmount
            - displacement.y * sceneHeight) * depth.y,
        0.0f,
    };
}

// Native 1401891a0 maps the authored 0..3 delay slider to a per-frame
// interpolation weight. Zero/negative delay bypasses smoothing; positive
// values clamp only the upper bound, exactly as the native update does.
inline float sceneParallaxDelayWeight (float delay, float frameDelta) {
    return delay > 0.0f ? std::min (1.0f, (1.0f - delay / 3.0f) * 10.0f * frameDelta) : 1.0f;
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
    glm::vec3 angles {0.0f, 0.0f, result.angle};
    if (object.is<Data::Model::Particle> ())
        angles = object.as<Data::Model::Particle> ()->angles->value->getVec3 ();
    else if (object.is<Data::Model::SceneModel> () ||
             (!object.is<Data::Model::Image> () && !object.is<Data::Model::Text> ()))
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
