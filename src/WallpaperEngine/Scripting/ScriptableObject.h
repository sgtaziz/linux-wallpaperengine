#pragma once
#include "WallpaperEngine/Data/Model/Types.h"
#include "WallpaperEngine/Render/CObject.h"

#include <memory>
#include <string>
#include <utility>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Scripting {
class ScriptableObject : virtual public CObject {
public:
    struct Lifetime {
	ScriptableObject* object;
    };
    struct PropertyEntry {
	std::string key;
	DynamicValue& value;
    };

    ScriptableObject (Wallpapers::CScene& scene, const Object& object);
    ~ScriptableObject () override;

    DynamicValue& getProperty (const std::string& name);

    const std::map<std::string, PropertyEntry>& getProperties () const;
    bool isAngleProperty (const DynamicValue& value) const;
    bool isRgbColorProperty (const DynamicValue& value) const;
    std::shared_ptr<Lifetime> getLifetime () const { return m_lifetime; }
    bool isSolid () const { return m_solid; }
    bool disablesCursorPropagation () const { return m_disablePropagation; }
    void setSolid (bool solid) { m_solid = solid; }
    void setDisablePropagation (bool disabled) { m_disablePropagation = disabled; }
    // The layer `name` host property (2.8.42: String, 
    // setter) stores script writes in the field that the scene's
    // name lookup compares. It starts as the
    // authored name.
    const std::string& getScriptName () const { return m_scriptName; }
    void setScriptName (std::string name) { m_scriptName = std::move (name); }

protected:
    void registerProperty (const std::string& name, DynamicValue& value);

private:
    bool m_solid;
    bool m_disablePropagation;
    std::string m_scriptName;
    std::shared_ptr<Lifetime> m_lifetime = std::make_shared<Lifetime> (Lifetime {this});
    std::map<std::string, PropertyEntry> m_properties;
};
}
