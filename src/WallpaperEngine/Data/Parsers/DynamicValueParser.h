#pragma once

#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/PropertyAnimation.h"
#include "WallpaperEngine/Data/Model/Types.h"

#include <vector>

namespace WallpaperEngine::Data::Parsers {
using json = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class DynamicValueParser {
public:
    static Model::DynamicValueUniquePtr parse (const json& data, const Properties& properties,
                                               bool expectColor, bool expectString = false,
                                               bool floatColor = false);
    /** Reads "options" (fps, length, mode, name, startpaused); false when native rejects the timeline. */
    static bool parseAnimationOptions (const json& source, Model::PropertyAnimation& result);
    /** Appends the strictly increasing, well-formed keys of one authored frame array. */
    static void parseAnimationKeys (const json& authored, std::vector<Model::PropertyAnimation::Key>& keys);
};
}
