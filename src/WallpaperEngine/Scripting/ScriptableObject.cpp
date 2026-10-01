#include "ScriptableObject.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <ranges>

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;

ScriptableObject::ScriptableObject (Wallpapers::CScene& scene, const Object& object) :
    CObject (scene, object), m_solid (object.solid), m_disablePropagation (object.disablePropagation) {
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

bool ScriptableObject::isAngleProperty (const DynamicValue& value) const {
    const auto entry = m_properties.find ("angles");
    return entry != m_properties.end () && &entry->second.value == &value;
}

bool ScriptableObject::isRgbColorProperty (const DynamicValue& value) const {
    const auto& object = getObject ();
    if (object.is<Data::Model::Image> ())
        return object.as<Data::Model::Image> ()->color->value.get () == &value;
    if (object.is<Data::Model::Text> ()) {
        const auto* text = object.as<Data::Model::Text> ();
        return text->color->value.get () == &value
            || (text->backgroundColor && text->backgroundColor->value.get () == &value);
    }
    return false;
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
