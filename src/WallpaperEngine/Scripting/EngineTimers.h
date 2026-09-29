#pragma once

#include "quickjs.h"

#include <chrono>
#include <cstdint>
#include <map>

namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptableObject;
/** Owns the callbacks and public timer bindings for one script engine context. */
class EngineTimers {
public:
    using Clock = std::chrono::steady_clock;

    explicit EngineTimers (JSContext* context);
    EngineTimers (JSContext* context, ScriptEngine& engine);
    ~EngineTimers ();

    EngineTimers (const EngineTimers&) = delete;
    EngineTimers& operator= (const EngineTimers&) = delete;

    uint32_t instanceId () const { return m_instanceId; }
    void install (JSValueConst engineObject) const;
    uint32_t setTimeout (JSValueConst callback, int64_t delayMs, Clock::time_point now = Clock::now ());
    uint32_t setInterval (JSValueConst callback, int64_t delayMs, Clock::time_point now = Clock::now ());
    void clearTimeout (uint32_t id);
    void clearInterval (uint32_t id);
    void cancelOwner (const ScriptableObject& object);
    void tick (Clock::time_point now = Clock::now ());

private:
    struct Timer {
        JSValue callback;
        JSValue layer;
        JSValue thisObject;
        const ScriptableObject* owner;
	std::chrono::milliseconds duration;
	Clock::time_point next;
    };

    uint32_t reserve (std::map<uint32_t, Timer>& timers, uint32_t& nextId, JSValueConst callback,
		      int64_t delayMs, Clock::time_point now);
    void clear (std::map<uint32_t, Timer>& timers, uint32_t id);
    void call (JSValueConst callback, JSValueConst layer, JSValueConst thisObject,
               const ScriptableObject* owner);

    JSContext* m_context;
    ScriptEngine* m_engine = nullptr;
    const ScriptableObject* m_activeOwner = nullptr;
    JSValue m_activeLayer = JS_UNDEFINED;
    JSValue m_activeThisObject = JS_UNDEFINED;
    uint32_t m_instanceId;
    uint32_t m_nextTimeoutId = 0;
    uint32_t m_nextIntervalId = 0;
    std::map<uint32_t, Timer> m_timeouts;
    std::map<uint32_t, Timer> m_intervals;
};
} // namespace WallpaperEngine::Scripting
