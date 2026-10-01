#pragma once

#include "WallpaperEngine/Data/Model/Effect.h"

#include <span>
#include <stdexcept>
#include <vector>

namespace WallpaperEngine::Render::Objects {
struct ImageCompositeStepEnd {
    size_t draws;
    size_t swaps;
    bool operator== (const ImageCompositeStepEnd&) const = default;
};
inline size_t imageEffectMainStepCount (const Data::Model::Effect& effect) {
    size_t count = 1;
    for (const auto& pass : effect.passes) count += pass->compose;
    return count;
}
// Native 1401e7170 counts compose:true descriptors; 1401e6300 adds one
// terminal main step per visible effect, including explicit-target endings.
// A material can compile into several draws, and swap commands into none.
// Boundaries therefore name the draw count after each authored descriptor.
inline std::vector<ImageCompositeStepEnd> imageEffectCompositeStepEnds (
    const Data::Model::Effect& effect, std::span<const ImageCompositeStepEnd> descriptorEnds) {
    if (descriptorEnds.size () != effect.passes.size ())
        throw std::invalid_argument ("Effect composite boundaries require every descriptor");
    std::vector<ImageCompositeStepEnd> result;
    for (size_t index = 0; index < effect.passes.size (); ++index)
        if (effect.passes[index]->compose) result.push_back (descriptorEnds[index]);
    // Empty effects affect native initial parity, but the draw loop does not
    // execute a terminal boundary without a descriptor (1401e8aa0).
    if (!descriptorEnds.empty ()) result.push_back (descriptorEnds.back ());
    return result;
}
}
