#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
/**
 * Copies the live scene bloom settings into the non-HDR bloom passes.
 *
 * Native general setters for bloomstrength/bloomthreshold store
 * the float and run change callback, which sets bit 31 of the scene
 * flags. After the SceneScript update, sees that
 * bit and, which uploads "bloomstrength" and
 * "bloomthreshold" to the bloom material before clearing the bit. A
 * script or animation on these general fields therefore changes the bloom of
 * the frame being drawn. The same non-HDR branch also uploads
 * "bloomtint".
 */
inline void applyBloomConstants (const std::vector<Data::Model::ImageEffectUniquePtr>& effects, float strength,
                                 float threshold) {
    for (const auto& effect : effects) {
        for (const auto& pass : effect->passOverrides) {
            for (const auto& [name, value] : {std::pair {"bloomstrength", strength}, {"bloomthreshold", threshold}}) {
                const auto constant = pass->constants.find (name);
                if (constant == pass->constants.end () || !constant->second || !constant->second->value) continue;
                auto& current = *constant->second->value;
                if (current.getType () != Data::Model::DynamicValue::Float || current.getFloat () != value)
                    current.update (value, Data::Model::DynamicValue::Script);
            }
        }
    }
}
} // namespace WallpaperEngine::Render::Wallpapers
