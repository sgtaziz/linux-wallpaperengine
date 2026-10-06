#pragma once

#include "PuppetMeshParser.h"
#include "PuppetSkinning.h"
#include "WallpaperEngine/Data/Model/Object.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render::Objects {

/**
 * Playback state of a scene MODEL's animation layers. Native builds
 * the same layer objects for MODEL as for image puppets; only
 * layers that resolve to a clip of the model contribute to the pose.
 */
struct ModelPuppetLayers {
    std::vector<PuppetPoseSample> reference;
    std::vector<PuppetPlaybackState> states;
    std::vector<uint64_t> clipIds;
    std::vector<bool> blendInActive;

    void reset (const PuppetSkeletonData& skeleton,
                const std::vector<std::unique_ptr<Data::Model::ImageAnimationLayer>>& layers) {
        reference = puppetReferencePoseSamples (skeleton);
        states.assign (layers.size (), {});
        clipIds.assign (layers.size (), UINT64_MAX);
        blendInActive.resize (layers.size ());
        for (size_t i = 0; i < layers.size (); ++i) blendInActive[i] = layers[i]->blendIn;
    }
};

/**
 * Advances every visible layer by delta seconds and returns the local bone
 * matrices (the unanimated bind pose when no layer resolves to a clip). Same
 * evaluation as CImage::updatePuppetAnimation without channel meshes.
 */
inline std::vector<glm::mat4> modelPuppetLocalMatrices (
    const PuppetSkeletonData& skeleton, const PuppetAnimationHeader& animation,
    const std::vector<std::unique_ptr<Data::Model::ImageAnimationLayer>>& layers,
    ModelPuppetLayers& state, float delta
) {
    auto pose = state.reference;
    bool contributing = false;
    for (size_t layerIndex = 0; layerIndex < layers.size (); ++layerIndex) {
        const auto& layer = *layers[layerIndex];
        if (!layer.visible->value->getBool ()) continue;
        const int selectedId = layer.animation->value->getInt ();
        const auto clip = std::find_if (animation.clips.begin (), animation.clips.end (),
                                        [selectedId] (const PuppetClipHeader& candidate) {
                                            return candidate.rawId == uint64_t (selectedId);
                                        });
        if (clip == animation.clips.end ()) continue;
        contributing = true;
        if (state.clipIds[layerIndex] != clip->rawId) {
            state.clipIds[layerIndex] = clip->rawId;
            state.states[layerIndex] = {};
            state.blendInActive[layerIndex] = layer.blendIn;
        }
        puppetAdvancePlayback (*clip, state.states[layerIndex], delta, layer.rate->value->getFloat ());
        const auto frames = puppetSelectFrames (*clip, state.states[layerIndex].time);
        bool blendInActive = state.blendInActive[layerIndex];
        const float weight = puppetEffectiveLayerWeight (
            *clip, state.states[layerIndex], layer.blend->value->getFloat (), layer.blendTime,
            blendInActive, layer.blendOut);
        state.blendInActive[layerIndex] = blendInActive;
        pose = puppetApplyClipLayer (skeleton, *clip, pose, state.reference, frames, weight, layer.additive);
    }
    return contributing ? puppetPoseMatrices (pose) : puppetUnanimatedLocalMatrices (skeleton);
}

} // namespace WallpaperEngine::Render::Objects
