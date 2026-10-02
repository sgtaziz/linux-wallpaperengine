#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

namespace WallpaperEngine::Render::Objects::ParticleCore {

inline bool needsNativeSlotStreams (const Data::Model::Particle& particle) {
    using namespace Data::Model;
    return std::any_of (particle.operators.begin (), particle.operators.end (), [] (const auto& op) {
        return op && (op->template is<BoidsOperator> () || (op->template is<VectorRemapValueOperator> ()
            && op->template as<VectorRemapValueOperator> ()->output == VectorRemapValueOperator::Output::ControlPoint));
    });
}

// This capability boundary is shared by construction and parsed-model tests.
// A scene transform parent is independent of a particle-runtime parent.
inline std::optional<std::string> nativeSlotScopeError (
    const Data::Model::Particle& particle, bool particleRuntimeParent = false,
    bool animatedTexture = false) {
    using namespace Data::Model;
    if (!needsNativeSlotStreams (particle)) return std::nullopt;
    const bool controlPointOutput = std::any_of (particle.operators.begin (), particle.operators.end (), [] (const auto& op) {
        return op && op->template is<VectorRemapValueOperator> ()
            && op->template as<VectorRemapValueOperator> ()->output == VectorRemapValueOperator::Output::ControlPoint;
    });
    const bool boids = std::any_of (particle.operators.begin (), particle.operators.end (), [] (const auto& op) {
        return op && op->template is<BoidsOperator> ();
    });
    const std::string owner = boids ? "Native boids physical streams" : "Runtime control-point output";
    if (particleRuntimeParent || !particle.children.empty ())
        return owner + " requires a root particle node without event children";
    if (particle.hasUnsupportedComponents)
        return owner + " contains an unsupported authored component";
    if (animatedTexture)
        return owner + " animated textures are unsupported";
    for (const auto& renderer : particle.renderers)
        if (renderer.name != "sprite") return owner + " requires sprite renderers";
    for (const auto& emitter : particle.emitters)
        if (emitter.name != "boxrandom" && emitter.name != "sphererandom")
            return owner + " emitter is unsupported: " + emitter.name;
    for (const auto& op : particle.operators) {
        if (!op) continue;
        if (op->is<BoidsOperator> () && op->blendEnvelope)
            return owner + " boids blend envelope requires separate per-lane clock validation";
        if (op->is<MovementOperator> () || op->is<CapVelocityOperator> () || op->is<BoidsOperator> ()) continue;
        if (boids && !controlPointOutput && (op->is<AngularMovementOperator> () || op->is<CollisionOperator> ()
            || op->is<AlphaFadeOperator> () || op->is<SizeChangeOperator> ()
            || op->is<AlphaChangeOperator> () || op->is<ColorChangeOperator> ())) continue;
        if (op->is<ScalarRemapValueOperator> ()) {
            const auto& remap = *op->as<ScalarRemapValueOperator> ();
            if (remap.transform == ScalarRemapValueOperator::Transform::Identity
                && (remap.output == ScalarRemapValueOperator::Output::Size
                    || remap.output == ScalarRemapValueOperator::Output::Opacity
                    || remap.output == ScalarRemapValueOperator::Output::Speed)) continue;
        }
        if (op->is<VectorRemapValueOperator> ()
            && op->as<VectorRemapValueOperator> ()->transform == VectorRemapValueOperator::Transform::Identity) continue;
        return boids && !controlPointOutput
            ? owner + " operator lacks physical-slot validation (supported: boids, movement, angularmovement, capvelocity, primitive collisions, fades/size/color, identity remaps)"
            : owner + " supports boids, movement, capvelocity and identity remaps";
    }
    for (const auto& initializer : particle.initializers) {
        if (!initializer) continue;
        if (initializer->is<InheritInitialValueFromEventInitializer> ())
            return owner + " event inheritance is unsupported";
        if (!initializer->is<RemapInitialValueInitializer> ()) continue;
        const auto& remap = initializer->as<RemapInitialValueInitializer> ()->remap;
        if (!remap || !remap->is<ScalarRemapValueOperator> ()) continue;
        const auto& scalar = *remap->as<ScalarRemapValueOperator> ();
        if (controlPointOutput && scalar.output == ScalarRemapValueOperator::Output::MaxLifetime
            && (scalar.operation != ScalarRemapValueOperator::Operation::Set
                || scalar.transform != ScalarRemapValueOperator::Transform::Identity
                || (scalar.flags & 1) == 0 || !std::isfinite (scalar.outputMin)
                || !std::isfinite (scalar.outputMax) || scalar.outputMin <= 0.0f
                || scalar.outputMax <= 0.0f))
            return owner + " birth lifetime remap is not proven positive";
    }
    return std::nullopt;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
