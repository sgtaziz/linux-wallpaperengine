#pragma once

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"
#include <exception>
#include <string>

namespace WallpaperEngine::Render::Shaders::Variables {
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Utils;

class ShaderVariable : public DynamicValue, public TypeCaster {
public:
    using DynamicValue::DynamicValue;

    [[nodiscard]] const std::string& getIdentifierName () const;
    [[nodiscard]] const std::string& getName () const;
    [[nodiscard]] bool isPosition () const { return m_position; }

    void setIdentifierName (std::string identifierName);
    void setName (const std::string& name);
    void setPosition (bool position) { m_position = position; }

private:
    std::string m_identifierName;
    std::string m_name;
    bool m_position = false;
};
} // namespace WallpaperEngine::Render::Shaders::Variables
