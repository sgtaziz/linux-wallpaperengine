#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Types.h"

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class EffectParser {
public:
    static EffectUniquePtr load (const Project& project, const std::string& filename);
    static EffectUniquePtr parse (const JSON& it, const Project& project);

private:
    static std::vector<std::string> parseDependencies (const JSON& it);
    static std::vector<EffectPassUniquePtr> parseEffectPasses (const JSON& it, const Project& project);
    static std::map<int, std::string> parseBinds (const JSON& it);
    static std::vector<FBOUniquePtr> parseFBOs (const JSON& it);
    static std::map<std::string, std::vector<std::string>> parseClearFunctions (
        const JSON& it, const std::vector<FBOUniquePtr>& fbos);
};
} // namespace WallpaperEngine::Data::Parsers
