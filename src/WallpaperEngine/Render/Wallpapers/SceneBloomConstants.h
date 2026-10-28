#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <vector>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Render::Wallpapers {
/**
 * Copies the live scene bloom settings into the non-HDR bloom passes.
 *
 * Native general setters for bloomstrength/bloomthreshold store
 * the float and run a change callback, which sets bit 31 of the scene
 * flags. After the SceneScript update, native sees that
 * bit and uploads "bloomstrength" and
 * "bloomthreshold" to the bloom material before clearing the bit. A
 * script or animation on these general fields therefore changes the bloom of
 * the frame being drawn. The same non-HDR branch also uploads
 * "bloomtint" as material value g_BloomTint, which the
 * shipped downsample_quarter_bloom.frag multiplies into the bloom colour.
 */
inline void applyBloomConstants (const std::vector<Data::Model::ImageEffectUniquePtr>& effects, float strength,
                                 float threshold, const glm::vec3& tint) {
    using Data::Model::DynamicValue;
    for (const auto& effect : effects) {
        for (const auto& pass : effect->passOverrides) {
            for (const auto& [name, value] : {std::pair {"bloomstrength", strength}, {"bloomthreshold", threshold}}) {
                const auto constant = pass->constants.find (name);
                if (constant == pass->constants.end () || !constant->second || !constant->second->value) continue;
                auto& current = *constant->second->value;
                if (current.getType () != DynamicValue::Float || current.getFloat () != value)
                    current.update (value, DynamicValue::Script);
            }
            const auto constant = pass->constants.find ("bloomtint");
            if (constant == pass->constants.end () || !constant->second || !constant->second->value) continue;
            auto& current = *constant->second->value;
            if (current.getType () != DynamicValue::Vec3 || current.getVec3 () != tint)
                current.update (tint, DynamicValue::Script);
        }
    }
}
} // namespace WallpaperEngine::Render::Wallpapers
