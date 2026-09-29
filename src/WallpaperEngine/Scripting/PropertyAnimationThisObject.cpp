#include "PropertyAnimationThisObject.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"

#include <cstdint>
#include <cmath>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Model::PropertyAnimation;

namespace {
PropertyAnimation* resolve (JSContext* context, JSValue* data) {
    if (!Adapters::ScriptableObjectAdapter::resolve (data[0])) {
        JS_ThrowTypeError (context, "Property animation is no longer available");
        return nullptr;
    }
    int64_t raw = 0;
    if (JS_ToInt64 (context, &raw, data[1]) < 0) return nullptr;
    auto* value = reinterpret_cast<DynamicValue*> (static_cast<intptr_t> (raw));
    if (!value || !value->getAnimation ()) {
        JS_ThrowTypeError (context, "Property animation is no longer available");
        return nullptr;
    }
    return value->getAnimation ();
}

JSValue animationMethod (JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                         int method, JSValue* data) {
    auto* animation = resolve (context, data);
    if (!animation) return JS_EXCEPTION;
    switch (method) {
        case 0: animation->play (); return JS_UNDEFINED;
        case 1: animation->pause (); return JS_UNDEFINED;
        case 2: animation->stop (); return JS_UNDEFINED;
        case 3: return JS_NewBool (context, animation->isPlaying ());
        case 4: {
            double frame = 0.0;
            if (argc < 1 || JS_ToFloat64 (context, &frame, argv[0]) < 0 || !std::isfinite (frame))
                return JS_ThrowTypeError (context, "setFrame requires a finite frame");
            const float frameValue = static_cast<float> (frame);
            if (!std::isfinite (frameValue))
                return JS_ThrowRangeError (context, "Frame is outside the animation range");
            if (!animation->setFrame (frameValue))
                return JS_ThrowRangeError (context, "Frame is outside the animation range");
            return JS_UNDEFINED;
        }
        case 5: return JS_NewFloat64 (context, animation->frame ());
        default: return JS_UNDEFINED;
    }
}

JSValue getAnimation (JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                      int, JSValue* data) {
    auto* animation = resolve (context, data);
    if (!animation) return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined (argv[0])) {
        const char* requested = JS_ToCString (context, argv[0]);
        if (!requested) return JS_EXCEPTION;
        const bool matches = animation->name == requested;
        JS_FreeCString (context, requested);
        if (!matches) return JS_UNDEFINED;
    }
    JSValue result = JS_NewObject (context);
    if (JS_IsException (result)) return result;
    JSValue captured[] { JS_DupValue (context, data[0]), JS_DupValue (context, data[1]) };
    const char* names[] {"play", "pause", "stop", "isPlaying", "setFrame", "getFrame"};
    for (int i = 0; i < 6; ++i)
        JS_SetPropertyStr (context, result, names[i],
            JS_NewCFunctionData (context, animationMethod, i == 4 ? 1 : 0, i, 2, captured));
    JS_FreeValue (context, captured[0]);
    JS_FreeValue (context, captured[1]);
    JS_SetPropertyStr (context, result, "fps", JS_NewFloat64 (context, animation->fps));
    JS_SetPropertyStr (context, result, "frameCount", JS_NewInt32 (context, animation->length));
    JS_SetPropertyStr (context, result, "duration", JS_NewFloat64 (context, animation->duration ()));
    JS_SetPropertyStr (context, result, "name", JS_NewString (context, animation->name.c_str ()));
    JSValue rateCapture[] { JS_DupValue (context, data[0]), JS_DupValue (context, data[1]) };
    JSValue rateGetter = JS_NewCFunctionData (context,
        [] (JSContext* ctx, JSValueConst, int, JSValueConst*, int, JSValue* captured) -> JSValue {
            auto* current = resolve (ctx, captured);
            return current ? JS_NewFloat64 (ctx, current->rate) : JS_EXCEPTION;
        }, 0, 0, 2, rateCapture);
    JSValue rateSetter = JS_NewCFunctionData (context,
        [] (JSContext* ctx, JSValueConst, int count, JSValueConst* args, int, JSValue* captured) -> JSValue {
            auto* current = resolve (ctx, captured);
            if (!current) return JS_EXCEPTION;
            double rate = 0.0;
            if (count != 1 || JS_ToFloat64 (ctx, &rate, args[0]) < 0 || !std::isfinite (rate))
                return JS_ThrowTypeError (ctx, "Animation rate requires a finite number");
            const float rateValue = static_cast<float> (rate);
            if (!std::isfinite (rateValue))
                return JS_ThrowRangeError (ctx, "Animation rate is outside the supported range");
            current->rate = rateValue;
            return JS_UNDEFINED;
        }, 1, 0, 2, rateCapture);
    JS_FreeValue (context, rateCapture[0]);
    JS_FreeValue (context, rateCapture[1]);
    JSAtom rateAtom = JS_NewAtom (context, "rate");
    JS_DefinePropertyGetSet (context, result, rateAtom, rateGetter, rateSetter,
                             JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom (context, rateAtom);
    return result;
}
}

void WallpaperEngine::Scripting::attachPropertyAnimation (
    JSContext* context, JSValueConst owner, JSValueConst layer, DynamicValue& value) {
    if (!value.getAnimation ()) return;
    JSValue captured[] { JS_DupValue (context, layer),
                         JS_NewInt64 (context, static_cast<int64_t> (reinterpret_cast<intptr_t> (&value))) };
    JS_DefinePropertyValueStr (context, owner, "getAnimation",
        JS_NewCFunctionData (context, getAnimation, 1, 0, 2, captured),
        JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeValue (context, captured[0]);
    JS_FreeValue (context, captured[1]);
}
