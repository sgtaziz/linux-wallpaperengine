#pragma once

#include "quickjs.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace WallpaperEngine::Data::Model {
class DynamicValue;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptableObject;
/**
 * Owns the callbacks and public timer bindings for one script engine context.
 *
 * Native keeps one timer list per script; setTimeout and
 * setInterval share it. Each frame walks the scripts in order and each
 * script's timers in creation order, subtracting the scene delta from the
 * remaining seconds and firing a timer once it is no longer positive.
 */
class EngineTimers {
public:
    /** The script that schedules a timer and owns its callbacks. */
    struct Origin {
        // The receiver of the script's timer callbacks: native
        // calls them with the script's module namespace object.
        JSValueConst receiver = JS_UNDEFINED;
        JSValueConst layer = JS_UNDEFINED;
        JSValueConst thisObject = JS_UNDEFINED;
        const ScriptableObject* owner = nullptr;
        // The scheduling script's own property. Timers are cancelled with their
        // owner, which also owns this value.
        Data::Model::DynamicValue* value = nullptr;
        // The script's position in the per-frame script order.
        size_t order = 0;
    };

    explicit EngineTimers (JSContext* context);
    EngineTimers (JSContext* context, ScriptEngine& engine);
    virtual ~EngineTimers ();

    EngineTimers (const EngineTimers&) = delete;
    EngineTimers& operator= (const EngineTimers&) = delete;

    uint32_t instanceId () const { return m_instanceId; }
    void install (JSValueConst engineObject) const;
    /** Schedules a callback after delay seconds; the caller validates native argument rules. */
    uint32_t setTimeout (JSValueConst callback, float delay);
    uint32_t setInterval (JSValueConst callback, float delay);
    /**
     * Native cancel function semantics for the timer setTimeout/setInterval
     * returned as id; see EngineTimers.cpp.
     */
    bool cancel (uint32_t id);
    /** Removes exactly this timer, for a binding that could not hand out its cancel function. */
    void discard (uint32_t id);
    void cancelOwner (const ScriptableObject& object);
    /** Advances every timer by the scene frame delta, in seconds. */
    void tick (float delta);
    /** The layer and script property that scheduled the callback now running, if any. */
    const ScriptableObject* activeOwner () const { return m_activeOwner; }
    Data::Model::DynamicValue* activeValue () const { return m_activeValue; }

    /** The module whose code is executing, if any. */
    virtual std::optional<Origin> runningScript () const;
    /** True while a module's top-level (global scope) code is evaluating. */
    virtual bool evaluatingModuleTopLevel () const;

private:
    struct Timer {
        JSValue callback;
        JSValue receiver;
        JSValue layer;
        JSValue thisObject;
        const ScriptableObject* owner;
        Data::Model::DynamicValue* value;
        size_t order;
        uint64_t sequence;
        bool interval;
        float duration;
        float remaining;
        // Native cancel data slot 0 still holds this timer (an External);
        // a fired timeout and a called cancel function clear it.
        bool cancellable = true;
    };

    uint32_t reserve (JSValueConst callback, float delay, bool interval);
    void release (const Timer& timer);
    void call (JSValueConst callback, const Origin& origin);

    JSContext* m_context;
    ScriptEngine* m_engine = nullptr;
    const ScriptableObject* m_activeOwner = nullptr;
    Data::Model::DynamicValue* m_activeValue = nullptr;
    JSValue m_activeReceiver = JS_UNDEFINED;
    JSValue m_activeLayer = JS_UNDEFINED;
    JSValue m_activeThisObject = JS_UNDEFINED;
    size_t m_activeOrder = 0;
    uint32_t m_instanceId;
    uint32_t m_nextId = 0;
    uint64_t m_nextSequence = 0;
    std::map<uint32_t, Timer> m_timers;
};
} // namespace WallpaperEngine::Scripting
