#pragma once

#include "ParticleCore.h"
#include "PuppetSkinning.h"

#include <limits>
#include <span>

namespace WallpaperEngine::Render::Objects {

// Native 1401d6d30 assigns sampled source pixels through projected bind
// bounds. It retains RGB and physical pixel coordinates, replacing bone only.
inline void assignPuppetEmissionBones (
    std::span<ParticleCore::ImageEmitterSample> samples,
    std::span<const PuppetSkeletonData::EmissionBound> bounds,
    std::span<const glm::mat4> bindGlobals
) {
    if (bounds.size () > bindGlobals.size () || bounds.size () > 255)
        throw std::runtime_error ("Invalid puppet emission bound count");
    std::vector<float> outsideDistance (samples.size (), 0.0f);
    std::vector<bool> inside (samples.size (), false), ambiguous (samples.size (), false);
    for (auto& sample : samples) sample.bone = 0xff;
    for (size_t bone = 0; bone < bounds.size (); ++bone) {
        const auto& bound = bounds[bone];
        const auto matrix = bindGlobals[bone] * glm::make_mat4 (bound.matrix.data ());
        const glm::vec2 a (ParticleCore::imageEmitterPoint (
            matrix, {bound.extent[0], bound.extent[1], 0}));
        const glm::vec2 b (ParticleCore::imageEmitterPoint (
            matrix, {-bound.extent[0], bound.extent[1], 0}));
        const glm::vec2 c (ParticleCore::imageEmitterPoint (
            matrix, {-bound.extent[0], -bound.extent[1], 0}));
        const glm::vec2 ab = b - a, bc = c - b;
        const float abLength = glm::dot (ab, ab), bcLength = glm::dot (bc, bc);
        for (size_t index = 0; index < samples.size (); ++index) {
            auto& sample = samples[index];
            const glm::vec2 point (float (sample.x), -float (sample.y));
            const float alongAB = glm::dot (point - a, ab);
            const float alongBC = glm::dot (point - b, bc);
            if (alongAB < 0 || abLength < alongAB || alongBC < 0 || bcLength < alongBC) {
                if (!inside[index]) {
                    const float distance = std::max ({0.0f, -alongAB, alongAB - abLength,
                                                     -alongBC, alongBC - bcLength});
                    if (outsideDistance[index] == 0 || distance < outsideDistance[index]) {
                        outsideDistance[index] = distance;
                        sample.bone = static_cast<uint8_t> (bone);
                    }
                }
            } else {
                if (sample.bone != 0xff && inside[index]) ambiguous[index] = true;
                sample.bone = static_cast<uint8_t> (bone);
                inside[index] = true;
            }
        }
    }
    for (size_t index = 0; index < samples.size (); ++index) {
        if (!ambiguous[index]) continue;
        float nearest = std::numeric_limits<float>::max ();
        uint8_t bone = 0xff;
        for (size_t candidate = 0; candidate < samples.size (); ++candidate) {
            if (ambiguous[candidate]) continue;
            const float dx = float (samples[candidate].x) - float (samples[index].x);
            const float dy = float (samples[candidate].y) - float (samples[index].y);
            const float distance = dy * dy + dx * dx;
            if (distance < nearest) {
                nearest = distance;
                bone = samples[candidate].bone;
            }
        }
        samples[index].bone = bone;
    }
}

} // namespace WallpaperEngine::Render::Objects
