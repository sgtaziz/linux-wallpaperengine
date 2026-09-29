#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) : CObject (scene, object) {
    // Concrete objects register the values their render/simulation paths read.
    // Generic group values can differ from typed values with the same name.
}

ScriptableObject::~ScriptableObject () {
    m_lifetime->object = nullptr;
    this->getScene ().getScriptEngine ().unregisterObject (*this);
}

DynamicValue& ScriptableObject::getProperty (const std::string& name) {
    const auto it = this->m_properties.find (name);

    if (it == this->m_properties.end ()) {
	sLog.exception ("Property '" + name + "' not found on object '" + this->getObject ().name + "'");
    }

    return it->second.value;
}

const std::map<std::string, ScriptableObject::PropertyEntry>& ScriptableObject::getProperties () const {
    return this->m_properties;
}

void ScriptableObject::registerProperty (const std::string& name, DynamicValue& value) {
    const auto existing = this->m_properties.find (name);
    if (existing != this->m_properties.end ()) {
	if (&existing->second.value != &value)
	    sLog.exception ("Conflicting script property '", name, "' on object '", this->getObject ().name, "'");
	return;
    }
    auto inserted = this->m_properties.emplace (
	name, PropertyEntry { .key = name + "_" + std::to_string (this->getId ()), .value = value }
    );
    this->getScene ().getScriptEngine ().queueScript (inserted.first->second.key, inserted.first->second.value, *this);
}
