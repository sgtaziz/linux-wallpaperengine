#pragma once

#include "quickjs.h"
#include <map>

namespace WallpaperEngine::Scripting::Adapters {
/** Stable JS identity without owning the native objects used as keys. */
class ObjectInstanceCache {
public:
    explicit ObjectInstanceCache (JSContext* context) : m_context (context) { }
    ~ObjectInstanceCache () { clear (); }
    ObjectInstanceCache (const ObjectInstanceCache&) = delete;
    ObjectInstanceCache& operator= (const ObjectInstanceCache&) = delete;

    JSValue find (const void* key) const {
        const auto found = m_instances.find (key);
        return found == m_instances.end () ? JS_UNDEFINED : JS_DupValue (m_context, found->second);
    }
    void retain (const void* key, JSValueConst instance) {
        forget (key);
        m_instances.emplace (key, JS_DupValue (m_context, instance));
    }
    void forget (const void* key) {
        const auto found = m_instances.find (key);
        if (found == m_instances.end ()) return;
        JS_FreeValue (m_context, found->second);
        m_instances.erase (found);
    }
    /** Must be called before destroying the context, even if wrappers survive in JS cycles. */
    void clear () {
        for (const auto& [key, instance] : m_instances) JS_FreeValue (m_context, instance);
        m_instances.clear ();
    }
private:
    JSContext* m_context;
    std::map<const void*, JSValue> m_instances;
};
}
