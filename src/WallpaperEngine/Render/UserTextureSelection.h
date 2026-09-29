#pragma once

#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Render/TextureProvider.h"

#include <memory>
#include <string>
#include <utility>

namespace WallpaperEngine::Render {

// An empty or unavailable scene-texture selection keeps the authored texture for the slot.
// The cached selection is refreshed when the project property changes.
template <typename Load, typename Fallback>
std::shared_ptr<const TextureProvider> resolveUserTextureSelection (
    const Data::Model::PropertySceneTexture& property, std::string& cachedValue,
    std::shared_ptr<const TextureProvider>& cachedTexture, Load&& load, Fallback&& fallback
) {
    const auto& selected = property.getString ();
    if (selected.empty ()) {
        cachedValue.clear ();
        cachedTexture.reset ();
        return std::forward<Fallback> (fallback) ();
    }
    if (selected != cachedValue) {
        cachedValue = selected;
        cachedTexture = std::forward<Load> (load) (selected);
    }
    return cachedTexture == nullptr ? std::forward<Fallback> (fallback) () : cachedTexture;
}

} // namespace WallpaperEngine::Render
