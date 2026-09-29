#include "EngineTimers.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Utils::ScopeGuard;

namespace {
uint32_t nextInstanceId = 0;
std::map<uint32_t, EngineTimers*> instances;

EngineTimers* findInstance (JSContext* ctx, JSValueConst owner) {
    uint32_t id = 0;
    if (JS_ToUint32 (ctx, &id, owner) < 0) return nullptr;
    const auto it = instances.find (id);
    return it == instances.end () ? nullptr : it->second;
}

JSValue stopTimer (JSContext* ctx, JSValueConst, int, JSValueConst*, int magic, JSValueConst* data) {
    auto* timers = findInstance (ctx, data[0]);
    if (!timers) return JS_UNDEFINED;
    uint32_t id = 0;
    if (JS_ToUint32 (ctx, &id, data[1]) < 0) return JS_EXCEPTION;
    if (magic == 1) timers->clearInterval (id);
    else timers->clearTimeout (id);
    return JS_UNDEFINED;
}

JSValue setTimer (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic,
	JSValueConst* data) {
    auto* timers = findInstance (ctx, data[0]);
    if (!timers) return JS_ThrowTypeError (ctx, "timer owner no longer exists");
    if (argc < 1 || !JS_IsFunction (ctx, argv[0]))
	return JS_ThrowTypeError (ctx, "timer callback must be a function");

    int64_t delayMs = 0;
    if (argc > 1 && JS_ToInt64 (ctx, &delayMs, argv[1]) < 0) return JS_EXCEPTION;
    const bool interval = magic == 1;
    const uint32_t id = interval ? timers->setInterval (argv[0], delayMs)
				 : timers->setTimeout (argv[0], delayMs);
    JSValue cancelData[] = {JS_DupValue (ctx, data[0]), JS_NewUint32 (ctx, id)};
    JSValue cancel = JS_NewCFunctionData (ctx, stopTimer, 0, magic, 2, cancelData);
    JS_FreeValue (ctx, cancelData[0]);
    JS_FreeValue (ctx, cancelData[1]);
    if (JS_IsException (cancel)) {
	if (interval) timers->clearInterval (id);
	else timers->clearTimeout (id);
    }
    return cancel;
}
} // namespace

EngineTimers::EngineTimers (JSContext* context) :
    m_context (context), m_instanceId (++nextInstanceId) {
    if (m_instanceId == 0)
	throw std::overflow_error ("too many script engine instances");
    instances.emplace (m_instanceId, this);
}

EngineTimers::EngineTimers (JSContext* context, ScriptEngine& engine) : EngineTimers (context) {
    m_engine = &engine;
}

EngineTimers::~EngineTimers () {
    instances.erase (m_instanceId);
    for (const auto& [id, timer] : m_timeouts) {
        JS_FreeValue (m_context, timer.callback);
        JS_FreeValue (m_context, timer.layer);
        JS_FreeValue (m_context, timer.thisObject);
    }
    for (const auto& [id, timer] : m_intervals) {
        JS_FreeValue (m_context, timer.callback);
        JS_FreeValue (m_context, timer.layer);
        JS_FreeValue (m_context, timer.thisObject);
    }
}

void EngineTimers::install (JSValueConst engineObject) const {
    JSValue owner[] = {JS_NewUint32 (m_context, m_instanceId)};
    JS_DefinePropertyValueStr (m_context, engineObject, "setInterval",
	JS_NewCFunctionData (m_context, setTimer, 2, 1, 1, owner), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr (m_context, engineObject, "setTimeout",
	JS_NewCFunctionData (m_context, setTimer, 2, 0, 1, owner), JS_PROP_ENUMERABLE);
    JS_FreeValue (m_context, owner[0]);
}

uint32_t EngineTimers::reserve (std::map<uint32_t, Timer>& timers, uint32_t& nextId,
	JSValueConst callback, int64_t delayMs, Clock::time_point now) {
    // Negative delays are immediately due; bound conversion and time_point arithmetic.
    const auto maxDelay = std::chrono::duration_cast<std::chrono::milliseconds> (Clock::duration::max () / 4).count ();
    const auto bounded = std::clamp<int64_t> (delayMs, 0, maxDelay);
    const auto duration = std::chrono::milliseconds (bounded);
    do { ++nextId; } while (nextId == 0 || timers.contains (nextId));
    const auto* module = m_engine ? m_engine->getRunningModule () : nullptr;
    timers.emplace (nextId, Timer {
        JS_DupValue (m_context, callback),
        JS_DupValue (m_context, module ? module->layer : m_activeLayer),
        JS_DupValue (m_context, module ? module->thisObject : m_activeThisObject),
        module ? &module->object : m_activeOwner,
        duration, now + duration});
    return nextId;
}

uint32_t EngineTimers::setTimeout (JSValueConst callback, int64_t delayMs, Clock::time_point now) {
    return reserve (m_timeouts, m_nextTimeoutId, callback, delayMs, now);
}

uint32_t EngineTimers::setInterval (JSValueConst callback, int64_t delayMs, Clock::time_point now) {
    return reserve (m_intervals, m_nextIntervalId, callback, delayMs, now);
}

void EngineTimers::clear (std::map<uint32_t, Timer>& timers, uint32_t id) {
    const auto it = timers.find (id);
    if (it == timers.end ()) return;
    JS_FreeValue (m_context, it->second.callback);
    JS_FreeValue (m_context, it->second.layer);
    JS_FreeValue (m_context, it->second.thisObject);
    timers.erase (it);
}

void EngineTimers::clearTimeout (uint32_t id) { clear (m_timeouts, id); }
void EngineTimers::clearInterval (uint32_t id) { clear (m_intervals, id); }

void EngineTimers::cancelOwner (const ScriptableObject& object) {
    for (auto* timers : {&m_timeouts, &m_intervals}) {
        for (auto it = timers->begin (); it != timers->end ();) {
            if (it->second.owner != &object) { ++it; continue; }
            JS_FreeValue (m_context, it->second.callback);
            JS_FreeValue (m_context, it->second.layer);
            JS_FreeValue (m_context, it->second.thisObject);
            it = timers->erase (it);
        }
    }
}

void EngineTimers::call (JSValueConst callback, JSValueConst layer, JSValueConst thisObject,
                         const ScriptableObject* owner) {
    const auto* previousOwner = m_activeOwner;
    const JSValue previousLayer = m_activeLayer;
    const JSValue previousObject = m_activeThisObject;
    m_activeOwner = owner;
    m_activeLayer = layer;
    m_activeThisObject = thisObject;
    ScopeGuard activeGuard ([&] {
        m_activeOwner = previousOwner;
        m_activeLayer = previousLayer;
        m_activeThisObject = previousObject;
    });
    JSValue priorLayer = m_engine
        ? JS_GetPropertyStr (m_context, m_engine->getGlobalThis (), "thisLayer") : JS_UNDEFINED;
    JSValue priorObject = m_engine
        ? JS_GetPropertyStr (m_context, m_engine->getGlobalThis (), "thisObject") : JS_UNDEFINED;
    ScopeGuard restore ([&] {
        if (m_engine) {
            JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisLayer", priorLayer);
            JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisObject", priorObject);
        }
    });
    if (m_engine) {
        JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisLayer", JS_DupValue (m_context, layer));
        JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisObject", JS_DupValue (m_context, thisObject));
    }
    JSValue result = JS_Call (m_context, callback, JS_NULL, 0, nullptr);
    if (JS_IsException (result)) {
	JSValue exception = JS_GetException (m_context);
	const char* message = JS_ToCString (m_context, exception);
	sLog.error ("ScriptEngine timer callback: ", message ? message : "unknown exception");
	if (message) JS_FreeCString (m_context, message);
	JS_FreeValue (m_context, exception);
    }
    JS_FreeValue (m_context, result);
}

void EngineTimers::tick (Clock::time_point now) {
    // Snapshot due IDs. Callbacks may clear either map or install new timers; new timers wait
    // until the next tick, and a cleared timer is never called from this snapshot.
    std::vector<uint32_t> dueIntervals;
    std::vector<uint32_t> dueTimeouts;
    for (const auto& [id, timer] : m_intervals)
	if (timer.next <= now) dueIntervals.push_back (id);
    for (const auto& [id, timer] : m_timeouts)
	if (timer.next <= now) dueTimeouts.push_back (id);

    for (uint32_t id : dueIntervals) {
	auto it = m_intervals.find (id);
	if (it == m_intervals.end ()) continue;
	it->second.next = now + it->second.duration;
	JSValue callback = JS_DupValue (m_context, it->second.callback);
	JSValue layer = JS_DupValue (m_context, it->second.layer);
	JSValue thisObject = JS_DupValue (m_context, it->second.thisObject);
	const auto* owner = it->second.owner;
	call (callback, layer, thisObject, owner);
	JS_FreeValue (m_context, callback);
	JS_FreeValue (m_context, layer);
	JS_FreeValue (m_context, thisObject);
    }
    for (uint32_t id : dueTimeouts) {
	auto it = m_timeouts.find (id);
	if (it == m_timeouts.end ()) continue;
	JSValue callback = it->second.callback;
	JSValue layer = it->second.layer;
	JSValue thisObject = it->second.thisObject;
	const auto* owner = it->second.owner;
	m_timeouts.erase (it); // A one-shot is gone before its callback can cancel itself.
	call (callback, layer, thisObject, owner);
	JS_FreeValue (m_context, callback);
	JS_FreeValue (m_context, layer);
	JS_FreeValue (m_context, thisObject);
    }
}
