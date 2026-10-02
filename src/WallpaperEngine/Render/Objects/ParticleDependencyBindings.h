#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <cstdint>
#include <span>

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Native advances a counter for each dependency TYPE while walking
// the shared compiled vector. Authored collisionmodel.index matches that
// counter, not the opcode's global vector ordinal. Its inner loop processes
// every matching authored record, so the last binding survives. No match
// leaves a null cache; opcode 1a skips collision rather than choosing a source.
inline const Data::Model::ObjectDependency* modelCollisionDependency (
    std::span<const Data::Model::ObjectDependency> dependencies, uint32_t bindingIndex
) {
    const Data::Model::ObjectDependency* result = nullptr;
    for (const auto& dependency : dependencies)
        if (dependency.type == "collisionmodel"
            && dependency.index == static_cast<int> (bindingIndex)) result = &dependency;
    return result;
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
