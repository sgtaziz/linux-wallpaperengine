#pragma once

#include <memory>
#include <string>

#include "Types.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Assets;
/**
 * Represents a wallpaper engine project
 */
struct Project {
    enum Type { Type_Scene = 0, Type_Web = 1, Type_Video = 2, Type_Unknown = 3 };

    /** Wallpapers title */
    std::string title;
    /** Wallpaper's type */
    Type type;
    /** Native scene context bit used by particle initializer defaults. */
    bool sceneOrthogonalProjection = false;
    /** Authored scene version used by native particle preset compilation. */
    int sceneVersion = 0;
    /** Workshop ID of the background or a negative id if not present */
    std::string workshopId;
    /** Display identity captured by SceneScript storage when this wallpaper is constructed. */
    std::string storageScreenKey = "default";
    /** Source directory/package path for storage identity when no Workshop ID exists. */
    std::string storageSourcePath;
    /** Indicates if the background uses audio processing or not */
    bool supportsAudioProcessing;
    /** All the available properties that the project defines for the user to change */
    Properties properties;
    /** The wallpaper this project defines */
    WallpaperUniquePtr wallpaper;
    /** Abstraction over asset loading to provide access to them */
    AssetLocatorUniquePtr assetLocator;
};
};
