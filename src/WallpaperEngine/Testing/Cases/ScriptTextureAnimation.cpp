#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/TextureAnimation.h"
#include "WallpaperEngine/Scripting/TextureAnimationObject.h"

#include <memory>
#include <string>
#include <vector>

namespace {
using WallpaperEngine::Render::ImageTextureAnimation;
using WallpaperEngine::Scripting::TextureAnimationObject;
using namespace WallpaperEngine::Data::Assets;

std::shared_ptr<ImageTextureAnimation> twoFrameAnimation () {
    std::vector<FrameSharedPtr> frames;
    for (int i = 0; i < 2; ++i) {
        auto frame = std::make_shared<Frame> ();
        frame->frametime = .9f;
        frames.push_back (std::move (frame));
    }
    return std::make_shared<ImageTextureAnimation> (std::move (frames));
}

struct AnimationJs {
    JSRuntime* runtime = JS_NewRuntime ();
    JSContext* context = JS_NewContext (runtime);
    std::unique_ptr<TextureAnimationObject> objects = std::make_unique<TextureAnimationObject> (context);

    ~AnimationJs () {
        objects->releaseInstances ();
        JS_FreeContext (context);
        JS_RunGC (runtime);
        JS_FreeRuntime (runtime);
        objects.reset ();
    }

    void expose (const char* name, const void* layer, const std::shared_ptr<ImageTextureAnimation>& state) {
        JSValue global = JS_GetGlobalObject (context);
        JS_SetPropertyStr (context, global, name, objects->instantiate (layer, state));
        JS_FreeValue (context, global);
    }

    std::string eval (const char* source) {
        JSValue result = JS_Eval (context, source, std::char_traits<char>::length (source),
                                  "<texture-animation>", JS_EVAL_TYPE_GLOBAL);
        REQUIRE_FALSE (JS_IsException (result));
        const char* chars = JS_ToCString (context, result);
        std::string value = chars ? chars : "";
        if (chars) JS_FreeCString (context, chars);
        JS_FreeValue (context, result);
        return value;
    }
};
}

TEST_CASE ("SceneScript texture handles preserve image identity and native numeric property behavior",
           "[script][texture][animation]") {
    AnimationJs js;
    auto first = twoFrameAnimation ();
    auto second = twoFrameAnimation ();
    int layerA = 0, layerB = 0, staticLayer = 0;
    js.expose ("a", &layerA, first);
    js.expose ("again", &layerA, first);
    js.expose ("b", &layerB, second);
    js.expose ("still", &staticLayer, nullptr);
    REQUIRE (js.eval ("[a===again,a!==b,still===null,a.frameCount,a.duration,a.rate,a.getFrame(),a.isPlaying()].join('|')")
             == "true|true|true|2|1.7999999523162842|1|0|true");
    REQUIRE (js.eval (R"(
        a.rate=9;
        const rates=[];
        for (const value of ['3',true,false,null,undefined,{}]) { a.rate=value; rates.push(a.rate); }
        a.rate=NaN; rates.push(Number.isNaN(a.rate));
        a.rate=-2; rates.push(a.rate);
        rates.push(b.rate);
        rates.join('|');
    )") == "9|9|9|9|9|9|true|-2|1");
    REQUIRE (js.eval (R"(
        const errors=[];
        for (const field of ['frameCount','duration']) {
            try { a[field]=500; errors.push('accepted'); } catch(e) { errors.push(e.name); }
        }
        errors.push(a.frameCount,a.duration);
        errors.join('|');
    )") == "TypeError|TypeError|2|1.7999999523162842");
}

TEST_CASE ("SceneScript texture seeks retain signed frames and reject fractional or coerced arguments",
           "[script][texture][animation]") {
    AnimationJs js;
    auto state = twoFrameAnimation ();
    int layer = 0;
    js.expose ("a", &layer, state);
    REQUIRE (js.eval (R"(
        a.rate=0;
        const rows=[];
        for (const value of [1,1.8,-1,2,147,undefined,'1',true,false,NaN]) {
            rows.push(String(a.setFrame(value)),a.getFrame(),a.isPlaying());
        }
        rows.push(String(a.setFrame()),a.getFrame());
        rows.join('|');
    )") == "undefined|1|true|undefined|0|true|undefined|-1|true|undefined|2|true|undefined|147|true|undefined|0|true|undefined|0|true|undefined|0|true|undefined|0|true|undefined|0|true|undefined|0");
    REQUIRE (js.eval (R"(
        [1.0,Math.floor(1.8),1+0.0,2147483647,2147483648,4294967295,
         4294967296,-2147483649,Infinity].map(value => {
            a.setFrame(value); return a.getFrame();
         }).join('|');
    )") == "1|1|1|2147483647|0|0|0|0|0");
}

TEST_CASE ("SceneScript texture controls preserve private playback and rejoin the shared clock",
           "[script][texture][animation]") {
    AnimationJs js;
    auto state = twoFrameAnimation ();
    int layer = 0;
    state->sharedPlayback ()->sample (1.0f, 0);
    js.expose ("a", &layer, state);
    REQUIRE (js.eval ("[String(a.pause()),a.getFrame(),a.isPlaying()].join('|')") == "undefined|1|false");
    REQUIRE (js.eval ("a.setFrame(0);[a.getFrame(),a.isPlaying()].join('|')") == "0|false");
    REQUIRE (js.eval ("[String(a.play()),a.getFrame(),a.isPlaying()].join('|')") == "undefined|0|true");
    REQUIRE (js.eval ("a.setFrame(1);[String(a.stop()),a.getFrame(),a.isPlaying()].join('|')") == "undefined|0|false");
    REQUIRE (js.eval ("a.rate=9;[String(a.join()),a.getFrame(),a.isPlaying(),a.rate].join('|')") == "undefined|1|true|9");
    state->sharedPlayback ()->sample (.8f, 1);
    REQUIRE (js.eval ("[a.getFrame(),a.isPlaying()].join('|')") == "0|true");
}

TEST_CASE ("Removed image texture handles become inert without retaining their animation state",
           "[script][texture][animation][lifetime]") {
    AnimationJs js;
    auto state = twoFrameAnimation ();
    int layer = 0;
    js.expose ("old", &layer, state);
    REQUIRE (js.eval ("old.rate=0;old.setFrame(1);globalThis.seek=old.setFrame;old.getFrame()") == "1");
    // Removing the retained cache entry does not invalidate a still-live image
    // during its current callback. Destruction invalidates surviving wrappers.
    js.objects->forgetInstance (&layer);
    REQUIRE (js.eval ("old.getFrame()") == "1");
    const std::weak_ptr<ImageTextureAnimation> removed = state;
    state.reset ();
    REQUIRE (removed.expired ());
    REQUIRE (js.eval ("[old.getFrame(),old.frameCount,old.duration,old.rate,old.isPlaying()].map(String).join('|')")
             == "undefined|undefined|undefined|undefined|undefined");
    REQUIRE (js.eval ("[old.setFrame(0),old.play(),old.pause(),old.stop(),old.join()].map(String).join('|')")
             == "undefined|undefined|undefined|undefined|undefined");
    REQUIRE (js.eval ("old.rate=9;String(seek.call(old,0))") == "undefined");
    state = twoFrameAnimation ();
    state->sharedPlayback ()->sample (1.0f, 0);
    js.expose ("fresh", &layer, state);
    REQUIRE (js.eval ("old.rate=0;[old!==fresh,fresh.rate,fresh.getFrame(),fresh.isPlaying()].join('|')")
             == "true|1|1|true");
}
