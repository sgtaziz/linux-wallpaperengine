#pragma once

#include "ObjectAdapter.h"

#include "ObjectInstanceCache.h"

namespace WallpaperEngine::Scripting::Adapters {
class ScriptableObjectAdapter : public ObjectAdapter {
public:
    explicit ScriptableObjectAdapter (ScriptEngine& engine, std::string name);

    JSValue instantiate (ScriptableObject& object) override;
    JSValue instantiate (Data::Model::DynamicValue& value) override;
    /** Release retained wrapper identity while the QuickJS context is live. */
    void forgetInstance (const ScriptableObject& object);
    void releaseInstances ();
    /** Resolve a live ILayer wrapper without accepting arbitrary JS objects. */
    static ScriptableObject* resolve (JSValueConst value);

private:
    ObjectInstanceCache m_instances;
    JSClassExoticMethods m_exoticMethods;
    std::string m_name;
};
}
