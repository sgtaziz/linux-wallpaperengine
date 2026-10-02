#pragma once

#include <nlohmann/json.hpp>
#include <functional>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Project.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;

using namespace WallpaperEngine::Assets;
using namespace WallpaperEngine::Data::Model;

/**
 * Parses a project file and returns an object representing it
 */
class ProjectParser {
public:
    struct ResolvedProject {
        JSON data;
        // Most specific project first, matching asset lookup precedence.
        std::vector<std::filesystem::path> assetDirectories;
    };
    using DependencyLookup = std::function<std::filesystem::path (const std::string&)>;
    static ResolvedProject resolve (const std::filesystem::path& directory,
                                    const DependencyLookup& lookup = {});
    static ProjectUniquePtr parse (const JSON& data, AssetLocatorUniquePtr container);

private:
    static Project::Type parseType (const std::string& type);
    static Properties parseProperties (const std::optional<JSON>& data);
};
} // namespace WallpaperEngine::Data::Parsers
