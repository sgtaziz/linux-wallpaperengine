#pragma once

#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Render::Shaders {

inline void applySceneHdrCombo (Data::Model::ComboMap& combos, bool active) {
    // Native 1401a5c40 inserts HDR=1 after material/default combos when the
    // scene context's HDR bit is active. Inactive scenes do not insert HDR=0.
    if (active) combos.insert_or_assign ("HDR", 1);
}

} // namespace WallpaperEngine::Render::Shaders
