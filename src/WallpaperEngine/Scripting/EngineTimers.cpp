#include "EngineTimers.h"

#include "ScriptEngine.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>
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

// The function returned by setTimeout and setInterval (2.8.42,
// 2.7.3); see EngineTimers::cancel.
JSValue cancelTimer (JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
    auto* timers = findInstance (ctx, data[0]);
    if (!timers) return JS_FALSE;
    if (timers->evaluatingModuleTopLevel ())
        return JS_ThrowSyntaxError (ctx, "timeout cannot be cleared from global scope.");
    uint32_t id = 0;
    if (JS_ToUint32 (ctx, &id, data[1]) < 0) return JS_EXCEPTION;
    return JS_NewBool (ctx, timers->cancel (id));
}

// Native engine.clearTimeout is the cancel function itself (2.7.3 installer
// binds; 2.8.42) without a timer in its data, so
// it never clears anything and returns false. Native has no engine.clearInterval.
JSValue clearTimeout (JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValueConst* data) {
    auto* timers = findInstance (ctx, data[0]);
    if (timers && timers->evaluatingModuleTopLevel ())
        return JS_ThrowSyntaxError (ctx, "timeout cannot be cleared from global scope.");
    return JS_FALSE;
}

JSValue setTimer (JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int magic,
	JSValueConst* data) {
    auto* timers = findInstance (ctx, data[0]);
    if (!timers) return JS_ThrowTypeError (ctx, "timer owner no longer exists");
    const bool interval = magic == 1;
    // setTimeout: 2.8.42, 2.7.3. setInterval: 2.8.42
    // 2.7.3. Both first refuse module global scope.
    if (timers->evaluatingModuleTopLevel ())
	return JS_ThrowSyntaxError (ctx, interval ? "setInterval cannot be called from global scope."
	                                         : "setTimeout cannot be called from global scope.");
    // Native rejects invalid arguments with null rather than throwing (2.7.3
    // argc < 1 or non-function; argc < 2). The delay is read
    // only from a JS number (IsNumber), as float (ms / 1000.0); a timeout accepts
    // a missing, non-numeric, zero, negative or NaN delay (anything but a number
    // counts as zero). An interval needs a numeric delay and rejects
    // delay <= 0; NaN is not <= 0 and fires on every frame.
    if (argc < (interval ? 2 : 1) || !JS_IsFunction (ctx, argv[0])) return JS_NULL;
    float delay = 0.0f;
    if (argc > 1 && JS_IsNumber (argv[1])) {
	double milliseconds = 0.0;
	if (JS_ToFloat64 (ctx, &milliseconds, argv[1]) < 0) return JS_EXCEPTION;
	delay = static_cast<float> (milliseconds / 1000.0);
    } else if (interval) {
	return JS_NULL;
    }
    if (interval && delay <= 0.0f) return JS_NULL;

    const uint32_t id = interval ? timers->setInterval (argv[0], delay) : timers->setTimeout (argv[0], delay);
    JSValue cancelData[] = {JS_DupValue (ctx, data[0]), JS_NewUint32 (ctx, id)};
    JSValue cancel = JS_NewCFunctionData (ctx, cancelTimer, 0, 0, 2, cancelData);
    JS_FreeValue (ctx, cancelData[0]);
    JS_FreeValue (ctx, cancelData[1]);
    if (JS_IsException (cancel)) timers->discard (id);
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
    for (const auto& [id, timer] : m_timers) release (timer);
}

void EngineTimers::install (JSValueConst engineObject) const {
    JSValue owner[] = {JS_NewUint32 (m_context, m_instanceId)};
    JS_DefinePropertyValueStr (m_context, engineObject, "setInterval",
	JS_NewCFunctionData (m_context, setTimer, 2, 1, 1, owner), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr (m_context, engineObject, "setTimeout",
	JS_NewCFunctionData (m_context, setTimer, 2, 0, 1, owner), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr (m_context, engineObject, "clearTimeout",
	JS_NewCFunctionData (m_context, clearTimeout, 1, 0, 1, owner), JS_PROP_ENUMERABLE);
    JS_FreeValue (m_context, owner[0]);
}

std::optional<EngineTimers::Origin> EngineTimers::runningScript () const {
    const auto* module = m_engine ? m_engine->getRunningModule () : nullptr;
    if (!module) return std::nullopt;
    return Origin {module->module, module->layer, module->thisObject, module->object, &module->value,
                   module->queueOrder};
}

bool EngineTimers::evaluatingModuleTopLevel () const {
    return m_engine && m_engine->isEvaluatingModuleTopLevel ();
}

uint32_t EngineTimers::reserve (JSValueConst callback, float delay, bool interval) {
    do { ++m_nextId; } while (m_nextId == 0 || m_timers.contains (m_nextId));
    // A timer belongs to the running module, or, from inside a timer callback,
    // to the script that scheduled that callback.
    const auto origin = runningScript ().value_or (Origin {
        m_activeReceiver, m_activeLayer, m_activeThisObject, m_activeOwner, m_activeValue, m_activeOrder});
    m_timers.emplace (m_nextId, Timer {
        JS_DupValue (m_context, callback),
        JS_DupValue (m_context, origin.receiver),
        JS_DupValue (m_context, origin.layer),
        JS_DupValue (m_context, origin.thisObject),
        origin.owner, origin.value, origin.order, m_nextSequence++, interval, delay, delay});
    return m_nextId;
}

uint32_t EngineTimers::setTimeout (JSValueConst callback, float delay) {
    return reserve (callback, delay, false);
}

uint32_t EngineTimers::setInterval (JSValueConst callback, float delay) {
    return reserve (callback, delay, true);
}

void EngineTimers::release (const Timer& timer) {
    JS_FreeValue (m_context, timer.callback);
    JS_FreeValue (m_context, timer.receiver);
    JS_FreeValue (m_context, timer.layer);
    JS_FreeValue (m_context, timer.thisObject);
}

bool EngineTimers::cancel (uint32_t id) {
    // Native cancel reads its
    // timer from cancel data slot 0, sets that slot to null and then erases the
    // FIRST timer of the timer's owning script, not the timer itself. It returns
    // true when that script had a timer to erase, which a live target always
    // ensures. With one pending timer that is the target: the debounce pattern.
    // A cleared slot (second call, or a timeout that fired) returns false.
    // Linux limitation: a cancel whose timer another cancel already erased reads
    // freed memory natively; here it returns false and erases nothing.
    const auto target = m_timers.find (id);
    if (target == m_timers.end () || !target->second.cancellable) return false;
    target->second.cancellable = false;
    const size_t script = target->second.order;
    auto first = m_timers.end ();
    for (auto it = m_timers.begin (); it != m_timers.end (); ++it)
        if (it->second.order == script && (first == m_timers.end () || it->second.sequence < first->second.sequence))
            first = it;
    release (first->second);
    m_timers.erase (first);
    return true;
}

void EngineTimers::discard (uint32_t id) {
    const auto it = m_timers.find (id);
    if (it == m_timers.end ()) return;
    release (it->second);
    m_timers.erase (it);
}

void EngineTimers::cancelOwner (const ScriptableObject& object) {
    for (auto it = m_timers.begin (); it != m_timers.end ();) {
        if (it->second.owner != &object) { ++it; continue; }
        release (it->second);
        it = m_timers.erase (it);
    }
}

void EngineTimers::call (JSValueConst callback, const Origin& origin) {
    const auto* previousOwner = m_activeOwner;
    auto* previousValue = m_activeValue;
    const JSValue previousReceiver = m_activeReceiver;
    const JSValue previousLayer = m_activeLayer;
    const JSValue previousObject = m_activeThisObject;
    const size_t previousOrder = m_activeOrder;
    m_activeOwner = origin.owner;
    m_activeValue = origin.value;
    m_activeReceiver = origin.receiver;
    m_activeLayer = origin.layer;
    m_activeThisObject = origin.thisObject;
    m_activeOrder = origin.order;
    ScopeGuard activeGuard ([&] {
        m_activeOwner = previousOwner;
        m_activeValue = previousValue;
        m_activeReceiver = previousReceiver;
        m_activeLayer = previousLayer;
        m_activeThisObject = previousObject;
        m_activeOrder = previousOrder;
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
        JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisLayer", JS_DupValue (m_context, origin.layer));
        JS_SetPropertyStr (m_context, m_engine->getGlobalThis (), "thisObject", JS_DupValue (m_context, origin.thisObject));
    }
    // Native tick (2.7.3; 2.8.42 loads it for the call
    // ) calls v8::Function::Call with the owning script's handle
    // as the receiver; the loader stores
    // Module::GetModuleNamespace there.
    JSValue result = JS_Call (m_context, callback, origin.receiver, 0, nullptr);
    if (JS_IsException (result)) {
	JSValue exception = JS_GetException (m_context);
	const char* message = JS_ToCString (m_context, exception);
	sLog.error ("ScriptEngine timer callback: ", message ? message : "unknown exception");
	if (message) JS_FreeCString (m_context, message);
	JS_FreeValue (m_context, exception);
    }
    JS_FreeValue (m_context, result);
}

void EngineTimers::tick (float delta) {
    // Native tick runs from the scene frame
    // after animation events and before update() (native 
    // engine). It walks the scripts (registration order here), then
    // each script's timers in creation order, timeouts and intervals alike, over
    // a copy of each list taken before its callbacks run: a timer scheduled by a
    // callback is neither advanced nor fired until the next frame, and a
    // cancelled one is skipped.
    std::vector<std::tuple<size_t, uint64_t, uint32_t>> scheduled;
    scheduled.reserve (m_timers.size ());
    for (const auto& [id, timer] : m_timers) scheduled.emplace_back (timer.order, timer.sequence, id);
    std::sort (scheduled.begin (), scheduled.end ());

    for (const auto& [order, sequence, id] : scheduled) {
	auto it = m_timers.find (id);
	if (it == m_timers.end ()) continue;
	auto& timer = it->second;
	timer.remaining -= delta;
	// Native fires when !(remaining > 0): NaN is due.
	if (timer.remaining > 0.0f) continue;
	const Origin origin {timer.receiver, timer.layer, timer.thisObject, timer.owner, timer.value, timer.order};
	if (timer.interval) {
	    // Re-arm to the full duration, dropping the overshoot: at most one call
	    // per frame.
	    timer.remaining = timer.duration;
	    JSValue callback = JS_DupValue (m_context, timer.callback);
	    JSValue receiver = JS_DupValue (m_context, timer.receiver);
	    JSValue layer = JS_DupValue (m_context, timer.layer);
	    JSValue thisObject = JS_DupValue (m_context, timer.thisObject);
	    call (callback, origin);
	    JS_FreeValue (m_context, callback);
	    JS_FreeValue (m_context, receiver);
	    JS_FreeValue (m_context, layer);
	    JS_FreeValue (m_context, thisObject);
	} else {
	    const Timer fired = timer;
	    // A one-shot is gone before its callback can cancel itself; native nulls
	    // its cancel function's timer slot and erases it.
	    m_timers.erase (it);
	    call (fired.callback, origin);
	    release (fired);
	}
    }
}
