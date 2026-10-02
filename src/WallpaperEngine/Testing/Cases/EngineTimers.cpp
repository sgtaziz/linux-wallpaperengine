#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Scripting/EngineTimers.h"
#include "WallpaperEngine/Scripting/SceneScriptClassRegistration.h"
#include "WallpaperEngine/Application/RenderFrameClock.h"
#include "WallpaperEngine/Render/Wallpapers/ParticleSceneClock.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <chrono>
#include <memory>
#include <string>

using WallpaperEngine::Scripting::EngineTimers;
using WallpaperEngine::Scripting::registerSceneScriptClass;
using namespace std::chrono_literals;

TEST_CASE ("Loading time does not advance the first live particle schedule", "[particle][clock][particle-stages]") {
    using namespace WallpaperEngine::Render::Objects::ParticleCore;
    float current = 0.0f;
    float previous = 0.0f;
    EmitterScheduleConfig schedule {};
    schedule.rate = 2.0f;
    auto state = initialState (schedule);
    const auto random = [] (float low, float) { return low; };

    // The host discards seven seconds of setup. Absolute shader/runtime time
    // remains intact, while a warmed particle still consumes the next frame.
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

namespace {
struct QuickJsFixture {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    std::unique_ptr<EngineTimers> timers;

    QuickJsFixture () {
	timers = std::make_unique<EngineTimers> (context);
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

TEST_CASE ("Engine timer bindings retain callbacks and zero-argument cancels use captured IDs", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        globalThis.stopOne = engine.setTimeout(() => events.push('one'), 0);
        globalThis.stopTwo = engine.setTimeout(() => events.push('two'), 0);
        stopOne();
        globalThis.stopInterval = engine.setInterval(() => events.push('interval'), 0);
    )");
    JS_RunGC (js.runtime);
    js.timers->tick (EngineTimers::Clock::now () + 1ms);
    REQUIRE (js.string ("events.join(',')") == "interval,two");
    js.run ("stopInterval(9999); stopTwo();");
    js.timers->tick (EngineTimers::Clock::now () + 2ms);
    REQUIRE (js.string ("events.join(',')") == "interval,two");
}

TEST_CASE ("Timer dispatch tolerates self-cancel, sibling-cancel and newly scheduled callbacks", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        globalThis.self = engine.setInterval(() => {
            events.push('self'); self();
            engine.setTimeout(() => events.push('new'), 0);
        }, 0);
        globalThis.killer = engine.setInterval(() => {
            events.push('killer'); victim(); killer();
        }, 0);
        globalThis.victim = engine.setInterval(() => events.push('victim'), 0);
        globalThis.oneShot = engine.setTimeout(() => {
            events.push('one-shot'); oneShot();
        }, 0);
    )");
    js.timers->tick (EngineTimers::Clock::now () + 1ms);
    REQUIRE (js.string ("events.join(',')") == "self,killer,one-shot");
    js.timers->tick (EngineTimers::Clock::now () + 2ms);
    REQUIRE (js.string ("events.join(',')") == "self,killer,one-shot,new");
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
    js.run ("stop();"); // A stale cancel closure is safe after the owner is destroyed.
}

TEST_CASE ("Thrown timer callback does not prevent later due callbacks", "[script][timer]") {
    QuickJsFixture js;
    js.run (R"(
        globalThis.events = [];
        engine.setTimeout(() => { throw new Error('timer failure'); }, 0);
        engine.setTimeout(() => events.push('after'), 0);
    )");
    js.timers->tick (EngineTimers::Clock::now () + 1ms);
    REQUIRE (js.string ("events.join(',')") == "after");
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
    high->tick (EngineTimers::Clock::now () + 1ms);
    js.timers->tick (EngineTimers::Clock::now () + 1ms);
    REQUIRE (js.string ("events.join(',')") == "high");
    high.reset ();
    js.run ("highCancel();");
    JSValue stale = js.eval ("highEngine.setTimeout(() => {}, 0)");
    REQUIRE (JS_IsException (stale));
    JS_FreeValue (js.context, stale);
    JSValue exception = JS_GetException (js.context);
    JS_FreeValue (js.context, exception);
}
