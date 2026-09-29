#pragma once

#include "ObjectAdapter.h"

namespace WallpaperEngine::Scripting::Adapters {
class ScriptableObjectAdapter : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);

    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& value) override;
    /** Resolve a live ILayer wrapper without accepting arbitrary JS objects. */
    static ScriptableObject* resolve (JSValueConst value);

private:
    JSClassExoticMethods m_exoticMethods;
    std::string m_name;
};
}
