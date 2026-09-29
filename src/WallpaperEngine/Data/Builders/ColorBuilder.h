#pragma once

#include "WallpaperEngine/Data/Model/Color.h"

#include <glm/vec4.hpp>
#include <string>

namespace WallpaperEngine::Data::Builders {
class ColorBuilder {
public:
    /**
     * White color constant
     */
    static const Model::Color White;
    /**
     * Black color constant
     */
    static const Model::Color Black;

    static WallpaperEngine::Data::Model::Color parse (const std::string& value, float alpha = 1.0f);
    // Project color properties store normalized channels, including integer
    // spellings such as "1 1 1". Scene literals may also use byte channels.
    static WallpaperEngine::Data::Model::Color parseProperty (const std::string& value,
                                                               float alpha = 1.0f);
};
}
