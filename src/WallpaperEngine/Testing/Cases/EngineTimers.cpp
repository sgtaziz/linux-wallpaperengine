#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Scripting/EngineObject.h"
#include "WallpaperEngine/Scripting/EngineTimers.h"
#include "WallpaperEngine/Scripting/SceneScriptClassRegistration.h"
#include "WallpaperEngine/Application/RenderFrameClock.h"
#include "WallpaperEngine/Render/Wallpapers/ParticleSceneClock.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <memory>
#include <optional>
#include <string>

using WallpaperEngine::Scripting::EngineTimers;
using WallpaperEngine::Scripting::registerSceneScriptClass;

TEST_CASE ("Loading time does not advance the first live particle schedule", "[particle][clock][particle-stages]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    float current = 0.0f;
    float previous = 0.0f;
    EmitterScheduleConfig schedule {};
    schedule.rate = 2.0f;
    auto state = initialState (schedule);
    const auto random = [] (float low, float) { return low; };

    // The host discards seven seconds of setup. Absolute shader time remains
    // intact, while a warmed particle still consumes the next frame.
    WallpaperEngine::Application::resumeRenderFrameClock (7.0f, current, previous);
    float warmedAge = 0.1f;
    uint32_t births = 0;
    for (int frame = 1; frame <= 31; ++frame) {
        WallpaperEngine::Application::sampleRenderFrameClock (
            7.0f + frame * 0.016f, current, previous);
        const float dt = current - previous;
        warmedAge += dt;
        births += advanceEmitter (schedule, state, dt, 16, random);
        REQUIRE (births == 0);
    }
    REQUIRE (warmedAge == Catch::Approx (0.596f).margin (0.00001f));
    WallpaperEngine::Application::sampleRenderFrameClock (7.512f, current, previous);
    REQUIRE (advanceEmitter (schedule, state, current - previous, 16, random) == 1);
    REQUIRE (current == Catch::Approx (7.512f));
}

TEST_CASE ("Fullscreen resume excludes the paused interval from the next scene tick", "[particle][clock]") {
    float current = 10.0f;
    float previous = 9.984f;
    double particleAccumulator = 0.0;
    WallpaperEngine::Application::sampleRenderFrameClock (10.016f, current, previous);
    const float firstStep = current - previous;
    WallpaperEngine::Render::Wallpapers::advanceParticleSceneClock (particleAccumulator, firstStep);
    REQUIRE (firstStep == Catch::Approx (0.016f).margin (0.00001f));

    // The resumed render-loop branch itself dispatches no scene frame.
    WallpaperEngine::Application::resumeRenderFrameClock (100.0f, current, previous);
    REQUIRE (particleAccumulator == Catch::Approx (firstStep));
    WallpaperEngine::Application::sampleRenderFrameClock (100.016f, current, previous);
    const float resumedStep = current - previous;
    const float published = WallpaperEngine::Render::Wallpapers::advanceParticleSceneClock (
        particleAccumulator, resumedStep);
    REQUIRE (resumedStep == Catch::Approx (0.016f).margin (0.00001f));
    REQUIRE (published == Catch::Approx (firstStep + resumedStep).margin (0.00001f));
    REQUIRE (current == Catch::Approx (100.016f)); // absolute shader time remains wall-clock based
}

TEST_CASE ("engine.runtime follows the scene clock, not the host clock", "[script][engine][clock]") {
    // Stand-in for CScene's clock accessor; the host clock is the real global.
    struct Scene {
        float time;
        float getSceneTime () const { return time; }
    };
    extern float g_Time;
    extern float g_TimeLast;
    const float savedTime = g_Time;
    const float savedLast = g_TimeLast;
    double accumulator = 0.0;
    // The host rebases after seven seconds of setup and loading, then renders
    // the first scene frame; CScene advances its clock by that frame's delta.
    WallpaperEngine::Application::resumeRenderFrameClock (7.0f, g_Time, g_TimeLast);
    WallpaperEngine::Application::sampleRenderFrameClock (7.016f, g_Time, g_TimeLast);
    const Scene scene {WallpaperEngine::Render::Wallpapers::advanceParticleSceneClock (
        accumulator, g_Time - g_TimeLast)};
    const double runtime = WallpaperEngine::Scripting::engineRuntime (scene);
    const double host = g_Time;
    g_Time = savedTime;
    g_TimeLast = savedLast;
    REQUIRE (host == Catch::Approx (7.016).margin (0.00001));
    REQUIRE (runtime == Catch::Approx (0.016).margin (0.00001));
}

namespace {
// Stands in for ScriptEngine's running module and top-level evaluation state.
class ScriptTimers : public EngineTimers {
public:
    using EngineTimers::EngineTimers;
    std::optional<Origin> running;
    bool topLevel = false;

    std::optional<Origin> runningScript () const override { return running; }
    bool evaluatingModuleTopLevel () const override { return topLevel; }
};

struct QuickJsFixture {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    std::unique_ptr<ScriptTimers> timers;

    QuickJsFixture () {
	timers = std::make_unique<ScriptTimers> (context);
	JSValue global = JS_GetGlobalObject (context);
	JSValue engine = JS_NewObject (context);
	timers->install (engine);
	JS_SetPropertyStr (context, global, "engine", engine);
	JS_FreeValue (context, global);
    }

    ~QuickJsFixture () {
	timers.reset ();
	JS_FreeContext (context);
	JS_FreeRuntime (runtime);
    }

    JSValue eval (const char* source) {
	return JS_Eval (context, source, std::char_traits<char>::length (source), "<timer-test>",
			JS_EVAL_TYPE_GLOBAL);
    }

    void run (const char* source) {
	JSValue result = eval (source);
	REQUIRE_FALSE (JS_IsException (result));
	JS_FreeValue (context, result);
    }

    // Runs source as a function body of the script at this position in the script order.
    void runAs (size_t order, const char* source, JSValueConst receiver = JS_UNDEFINED) {
	timers->running = EngineTimers::Origin {.receiver = receiver, .order = order};
	run (source);
	timers->running.reset ();
    }

    std::string string (const char* source) {
	JSValue result = eval (source);
	REQUIRE_FALSE (JS_IsException (result));
	const char* chars = JS_ToCString (context, result);
	std::string value = chars ? chars : "";
	if (chars) JS_FreeCString (context, chars);
	JS_FreeValue (context, result);
	return value;
    }
};

int finalized = 0;
void markerFinalizer (JSRuntime*, JSValue) { ++finalized; }
} // namespace

TEST_CASE ("SceneScript class brands register independently across QuickJS runtimes", "[script][asset]") {
    const JSClassDef engineClass {.class_name = "IEngine"};
    const JSClassDef assetClass {.class_name = "IAssetHandle"};
    const JSClassDef sceneClass {.class_name = "IScene"};
    const JSClassDef unrelatedClass {.class_name = "Unrelated"};
    for (int round = 0; round < 3; ++round) {
        JSRuntime* runtime = JS_NewRuntime ();
        REQUIRE (runtime != nullptr);
        JSContext* context = JS_NewContext (runtime);
        REQUIRE (context != nullptr);
        if (round != 0) registerSceneScriptClass (runtime, unrelatedClass);
        const JSClassID engine = registerSceneScriptClass (runtime, engineClass);
        const JSClassID asset = registerSceneScriptClass (runtime, assetClass);
        const JSClassID scene = registerSceneScriptClass (runtime, sceneClass);
        REQUIRE (engine != asset);
        REQUIRE (asset != scene);
        REQUIRE (engine != scene);
        JSValue engineValue = JS_NewObjectClass (context, engine);
        JSValue assetValue = JS_NewObjectClass (context, asset);
        JSValue sceneValue = JS_NewObjectClass (context, scene);
        REQUIRE_FALSE (JS_IsException (engineValue));
        REQUIRE_FALSE (JS_IsException (assetValue));
        REQUIRE_FALSE (JS_IsException (sceneValue));
        int marker = 42;
        JS_SetOpaque (assetValue, &marker);
        REQUIRE (JS_GetOpaque2 (context, assetValue, asset) == &marker);
        REQUIRE (JS_GetOpaque2 (context, assetValue, scene) == nullptr);
        JSValue error = JS_GetException (context);
        JS_FreeValue (context, error);
        JS_FreeValue (context, sceneValue);
        JS_FreeValue (context, assetValue);
        JS_FreeValue (context, engineValue);
        JS_FreeContext (context);
        JS_FreeRuntime (runtime);
    }
}

// One 60 Hz scene frame, in seconds.
constexpr float frame = 0.016f;

TEST_CASE ("Engine timer cancel functions answer once and not after their timeout fired", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        globalThis.stopOne = engine.setTimeout(() => events.push('one'), 0);
        globalThis.stopTwo = engine.setTimeout(() => events.push('two'), 0);
        globalThis.cancels = [stopOne(), stopOne()];
        globalThis.stopInterval = engine.setInterval(() => events.push('interval'), 16);
    )");
    JS_RunGC (js.runtime);
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "two,interval");
    // The timeout already fired; zero-argument cancels ignore any passed ID and
    // answer true only on their first call.
    js.run ("cancels.push(stopTwo(), stopInterval(9999), stopInterval());");
    REQUIRE (js.string ("cancels.join(',')") == "true,false,false,true,false");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "two,interval");
}

TEST_CASE ("Timer dispatch tolerates self-cancel, native sibling-cancel and newly scheduled callbacks", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        globalThis.self = engine.setInterval(() => {
            events.push('self'); self();
            engine.setTimeout(() => events.push('new'), -1);
        }, 1);
        globalThis.killer = engine.setInterval(() => {
            events.push('killer'); victim(); killer();
        }, 1);
        globalThis.victim = engine.setInterval(() => events.push('victim'), 1);
        globalThis.oneShot = engine.setTimeout(() => {
            events.push('one-shot'); oneShot();
        }, 0);
    )");
    // A timer scheduled by a callback is neither advanced nor fired in that
    // frame. victim() erases the script's oldest pending timer, killer itself,
    // so victim keeps running and killer() then has nothing to cancel.
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "self,killer,victim,one-shot");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "self,killer,victim,one-shot,victim,new");
}

TEST_CASE ("Timer callbacks survive released script values and release on clear or owner teardown", "[script][timer]") {
    finalized = 0;
    QuickJsFixture js;
    JSClassID classId = 0;
    JS_NewClassID (js.runtime, &classId);
    JSClassDef definition {.class_name = "TimerMarker", .finalizer = markerFinalizer};
    REQUIRE (JS_NewClass (js.runtime, classId, &definition) == 0);
    JSValue marker = JS_NewObjectClass (js.context, classId);
    JSValue global = JS_GetGlobalObject (js.context);
    JS_SetPropertyStr (js.context, global, "marker", marker);
    JS_FreeValue (js.context, global);
    js.run (R"(
        globalThis.stop = engine.setInterval((held => () => held)(marker), 500);
        delete globalThis.marker;
    )");
    JS_RunGC (js.runtime);
    REQUIRE (finalized == 0);
    js.run ("stop();");
    JS_RunGC (js.runtime);
    REQUIRE (finalized == 1);

    JSValue second = JS_NewObjectClass (js.context, classId);
    global = JS_GetGlobalObject (js.context);
    JS_SetPropertyStr (js.context, global, "marker", second);
    JS_FreeValue (js.context, global);
    js.run (R"(engine.setTimeout((held => () => held)(marker), 500); delete globalThis.marker;)");
    JS_RunGC (js.runtime);
    REQUIRE (finalized == 1);
    js.timers.reset ();
    JS_RunGC (js.runtime);
    REQUIRE (finalized == 2);
    REQUIRE (js.string ("String(stop())") == "false"); // A stale cancel closure is safe after teardown.
}

TEST_CASE ("Thrown timer callbacks are logged without changing any timer's schedule", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        globalThis.failures = 0;
        engine.setTimeout(() => { throw new Error('timer failure'); }, 0);
        engine.setInterval(() => { ++failures; throw new Error('interval failure'); }, 10);
        engine.setTimeout(() => events.push('after'), 0);
    )");
    for (int i = 0; i < 3; ++i) js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "after");
    REQUIRE (js.string ("String(failures)") == "3");
}

TEST_CASE ("Timer owner IDs survive QuickJS function-magic boundaries", "[script][timer]") {
    QuickJsFixture js;
    uint32_t last = 0;
    do {
	EngineTimers previous (js.context);
	last = previous.instanceId ();
    } while (last <= 65535);

    auto high = std::make_unique<EngineTimers> (js.context);
    REQUIRE (high->instanceId () > 65535);
    JSValue global = JS_GetGlobalObject (js.context);
    JSValue highEngine = JS_NewObject (js.context);
    high->install (highEngine);
    JS_SetPropertyStr (js.context, global, "highEngine", highEngine);
    JS_FreeValue (js.context, global);

    js.run (R"(
        globalThis.events = [];
        globalThis.lowCancel = engine.setTimeout(() => events.push('low'), 0);
        globalThis.highCancel = highEngine.setTimeout(() => events.push('high'), 0);
        lowCancel();
    )");
    high->tick (frame);
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "high");
    high.reset ();
    js.run ("highCancel();");
    JSValue stale = js.eval ("highEngine.setTimeout(() => {}, 0)");
    REQUIRE (JS_IsException (stale));
    JS_FreeValue (js.context, stale);
    JSValue exception = JS_GetException (js.context);
    JS_FreeValue (js.context, exception);
}

TEST_CASE ("Engine timers reject invalid native arguments with null", "[script][timer]") {
    // Native setInterval needs a function and a positive numeric
    // delay; setTimeout needs only a function.
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        const bad = () => events.push('bad');
        globalThis.results = [
            engine.setInterval(bad, 0), engine.setInterval(bad, -1), engine.setInterval(bad),
            engine.setInterval(bad, '100'), engine.setInterval(42, 100), engine.setInterval(),
            engine.setTimeout(42), engine.setTimeout(),
        ].map(value => value === null);
        globalThis.accepted = typeof engine.setTimeout(() => {}, '300');
    )");
    REQUIRE (js.string ("results.join(',')") == "true,true,true,true,true,true,true,true");
    REQUIRE (js.string ("accepted") == "function");
    for (int i = 0; i < 3; ++i) js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')").empty ());
}

TEST_CASE ("Engine timer delays come only from numbers, in milliseconds", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        engine.setTimeout(() => events.push('string'), '300');
        engine.setTimeout(() => events.push('number'), 300);
        engine.setTimeout(() => events.push('negative'), -5);
        engine.setTimeout(() => events.push('nan'), NaN);
        engine.setTimeout(() => events.push('missing'));
    )");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "string,negative,nan,missing");
    js.timers->tick (0.25f);
    REQUIRE (js.string ("events.join(',')") == "string,negative,nan,missing");
    js.timers->tick (0.05f);
    REQUIRE (js.string ("events.join(',')") == "string,negative,nan,missing,number");
}

TEST_CASE ("A NaN interval delay fires on every frame", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.count = 0;
        globalThis.kind = typeof engine.setInterval(() => ++count, NaN);
    )");
    REQUIRE (js.string ("kind") == "function");
    for (int i = 0; i < 4; ++i) js.timers->tick (frame);
    REQUIRE (js.string ("String(count)") == "4");
}

TEST_CASE ("A long frame fires an interval once and drops the overshoot", "[script][timer]") {
    QuickJsFixture js;
    js.run ("globalThis.count = 0; engine.setInterval(() => ++count, 100);");
    js.timers->tick (1.0f);
    REQUIRE (js.string ("String(count)") == "1");
    // Re-armed to the full 0.1 s, not to the 0.9 s already elapsed past it.
    js.timers->tick (0.05f);
    REQUIRE (js.string ("String(count)") == "1");
    js.timers->tick (0.05f);
    REQUIRE (js.string ("String(count)") == "2");
}

TEST_CASE ("Engine timers advance only by the scene delta", "[script][timer]") {
    // A paused wallpaper dispatches no scene delta; wall time never drives timers.
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        engine.setTimeout(() => events.push('timeout'), 100);
        engine.setInterval(() => events.push('interval'), 50);
    )");
    for (int i = 0; i < 10; ++i) js.timers->tick (0.0f);
    REQUIRE (js.string ("events.join(',')").empty ());
    js.timers->tick (0.05f);
    REQUIRE (js.string ("events.join(',')") == "interval");
    js.timers->tick (0.05f);
    REQUIRE (js.string ("events.join(',')") == "interval,timeout,interval");
}

TEST_CASE ("Native engine.clearTimeout is a no-op and there is no clearInterval", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        const stop = engine.setTimeout(() => events.push('kept'), 0);
        globalThis.cleared = String(engine.clearTimeout(stop));
        globalThis.clearIntervalType = typeof engine.clearInterval;
    )");
    REQUIRE (js.string ("cleared") == "false");
    REQUIRE (js.string ("clearIntervalType") == "undefined");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "kept");
}

TEST_CASE ("Timeouts and intervals fire together in creation order", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        engine.setTimeout(() => events.push('T1'), 10);
        engine.setInterval(() => events.push('I1'), 10);
        engine.setTimeout(() => events.push('T2'), 10);
    )");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "T1,I1,T2");
}

TEST_CASE ("Engine timers fire in script order before creation order", "[script][timer]") {
    QuickJsFixture js;
    js.run ("globalThis.events = [];");
    // The later script schedules first. Its callback's timeout belongs to it too.
    js.runAs (1, R"(
        let first = true;
        engine.setInterval(() => {
            events.push('B');
            if (first) engine.setTimeout(() => events.push('B-child'), 0);
            first = false;
        }, 10);
    )");
    js.runAs (0, "engine.setTimeout(() => events.push('A'), 10);");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "A,B");
    js.runAs (0, "engine.setTimeout(() => events.push('A-late'), 0);");
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "A,B,A-late,B,B-child");
}

TEST_CASE ("Engine timer calls from module global scope throw native errors", "[script][timer]") {
    QuickJsFixture js;
    js.run ("globalThis.events = []; globalThis.stop = engine.setTimeout(() => events.push('kept'), 0);");
    js.timers->topLevel = true;
    REQUIRE (js.string (R"([
        () => engine.setTimeout(() => {}, 0), () => engine.setInterval(() => {}, 10),
        () => stop(), () => engine.clearTimeout(stop),
    ].map(call => { try { call(); return 'none'; } catch (e) { return e.name + ': ' + e.message; } })
     .join('|'))") ==
        "SyntaxError: setTimeout cannot be called from global scope.|"
        "SyntaxError: setInterval cannot be called from global scope.|"
        "SyntaxError: timeout cannot be cleared from global scope.|"
        "SyntaxError: timeout cannot be cleared from global scope.");
    js.timers->topLevel = false;
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "kept");
}

TEST_CASE ("A cancel function erases its script's oldest pending timer", "[script][timer]") {
    // Native cancel erases the first timer of the target's script,
    // whichever that is, and clears its own handle so later calls answer false.
    QuickJsFixture js;
    js.run ("globalThis.events = [];");
    js.runAs (0, "globalThis.a1 = engine.setTimeout(() => events.push('a1'), 100);");
    js.runAs (1, R"(
        globalThis.d1 = engine.setTimeout(() => events.push('d1'), 100);
        globalThis.d2 = engine.setTimeout(() => events.push('d2'), 100);
    )");
    REQUIRE (js.string ("[d2(), d2(), d1()].join(',')") == "true,false,false");
    js.timers->tick (0.2f);
    REQUIRE (js.string ("events.join(',')") == "a1,d2");
}

TEST_CASE ("A single pending timer debounce cancels exactly that timer", "[script][timer]") {
    QuickJsFixture js;
    js.runAs (0, R"(
        globalThis.events = [];
        let cancel = null;
        globalThis.bounce = () => {
            if (cancel) events.push('cancel:' + cancel());
            cancel = engine.setTimeout(() => { events.push('fire'); }, 100);
        };
        bounce(); bounce(); bounce();
    )");
    js.timers->tick (0.2f);
    REQUIRE (js.string ("events.join(',')") == "cancel:true,cancel:true,fire");
    js.run ("bounce();");
    js.timers->tick (0.2f);
    REQUIRE (js.string ("events.join(',')") == "cancel:true,cancel:true,fire,cancel:false,fire");
}

TEST_CASE ("Engine timer callbacks receive their script's module namespace as this", "[script][timer]") {
    // Native calls a timer callback with the owning script's module namespace
    // (the loader's Module::GetModuleNamespace handle) as the receiver.
    QuickJsFixture js;
    JSValue global = JS_GetGlobalObject (js.context);
    JSValue namespaceObject = JS_NewObject (js.context);
    JS_SetPropertyStr (js.context, global, "moduleNamespace", JS_DupValue (js.context, namespaceObject));
    JS_FreeValue (js.context, global);
    js.runAs (0, R"(
        globalThis.events = [];
        engine.setTimeout(function () {
            events.push(this === moduleNamespace);
            engine.setTimeout(function () { events.push(this === moduleNamespace); }, 0);
        }, 0);
    )", namespaceObject);
    JS_FreeValue (js.context, namespaceObject);
    js.timers->tick (frame);
    js.timers->tick (frame);
    REQUIRE (js.string ("events.join(',')") == "true,true");
}
