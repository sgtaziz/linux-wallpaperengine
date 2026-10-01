#include "TextureAnimationObject.h"

#include "SceneScriptClassRegistration.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/TextureAnimation.h"

#include <cmath>
#include <bit>
#include <limits>
#include <string_view>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Render::ImageTextureAnimation;
using WallpaperEngine::Data::Utils::ScopeGuard;

namespace {
struct AnimationHandle {
    std::weak_ptr<ImageTextureAnimation> animation;
};

enum Method { Play, Pause, Stop, IsPlaying, SetFrame, GetFrame, Join };

void finalizeAnimation (JSRuntime*, JSValueConst value) {
    delete static_cast<AnimationHandle*> (JS_GetOpaque (value, JS_GetClassID (value)));
}

JSValue animationMethod (JSContext* context, JSValueConst receiver, int argc,
                         JSValueConst* argv, int method, JSValue* data) {
    int32_t classId = 0;
    if (JS_ToInt32 (context, &classId, data[0]) < 0) return JS_EXCEPTION;
    auto* handle = static_cast<AnimationHandle*> (JS_GetOpaque2 (context, receiver, classId));
    if (!handle) return JS_EXCEPTION;
    const auto animation = handle->animation.lock ();
    if (!animation) return JS_UNDEFINED;
    switch (method) {
        case Play: animation->play (); break;
        case Pause: animation->pause (); break;
        case Stop: animation->stop (); break;
        case IsPlaying: return JS_NewBool (context, animation->isPlaying ());
        case GetFrame: return JS_NewInt32 (context, std::bit_cast<int32_t> (animation->getFrame ()));
        case Join: animation->join (); break;
        case SetFrame: {
            uint32_t frame = 0;
            if (argc > 0 && JS_IsNumber (argv[0])) {
                double number = 0;
                if (JS_ToFloat64 (context, &number, argv[0]) < 0) return JS_EXCEPTION;
                if (std::isfinite (number) && std::trunc (number) == number
                    && number >= std::numeric_limits<int32_t>::min ()
                    && number <= std::numeric_limits<int32_t>::max ())
                    frame = static_cast<uint32_t> (static_cast<int32_t> (number));
            }
            animation->setFrame (frame);
            break;
        }
    }
    return JS_UNDEFINED;
}

JSValue animationProperty (JSContext* context, JSValueConst object, JSAtom atom, JSValueConst) {
    auto* handle = static_cast<AnimationHandle*> (JS_GetOpaque (object, JS_GetClassID (object)));
    const char* name = JS_AtomToCString (context, atom);
    if (!name) return JS_EXCEPTION;
    ScopeGuard release ([=] { JS_FreeCString (context, name); });
    const std::string_view property (name);
    const int method = property == "play" ? Play
        : property == "pause" ? Pause
        : property == "stop" ? Stop
        : property == "isPlaying" ? IsPlaying
        : property == "setFrame" ? SetFrame
        : property == "getFrame" ? GetFrame
        : property == "join" ? Join : -1;
    if (method >= 0) {
        // A removed image's methods remain callable and return undefined;
        // only its numeric properties lose their values.
        JSValue classId = JS_NewInt32 (context, JS_GetClassID (object));
        JSValue result = JS_NewCFunctionData (context, animationMethod, method == SetFrame ? 1 : 0,
                                            method, 1, &classId);
        JS_FreeValue (context, classId);
        return result;
    }
    const auto animation = handle ? handle->animation.lock () : nullptr;
    if (!animation) return JS_UNDEFINED;
    if (property == "rate") return JS_NewFloat64 (context, animation->rate ());
    if (property == "frameCount") return JS_NewUint32 (context, animation->frameCount ());
    if (property == "duration") return JS_NewFloat64 (context, animation->duration ());
    return JS_UNDEFINED;
}

int setAnimationProperty (JSContext* context, JSValueConst object, JSAtom atom,
                          JSValueConst value, JSValueConst, int) {
    auto* handle = static_cast<AnimationHandle*> (JS_GetOpaque (object, JS_GetClassID (object)));
    const auto animation = handle ? handle->animation.lock () : nullptr;
    if (!animation) return 1;
    const char* name = JS_AtomToCString (context, atom);
    if (!name) return -1;
    ScopeGuard release ([=] { JS_FreeCString (context, name); });
    const std::string_view property (name);
    if (property == "frameCount" || property == "duration")
        return JS_ThrowTypeError (context, "TextureAnimation property is read-only: %s", name), -1;
    // Native numeric properties accept numeric values, including NaN, but
    // silently retain their value for strings, booleans and null.
    if (property == "rate" && JS_IsNumber (value)) {
        double rate = 0;
        if (JS_ToFloat64 (context, &rate, value) < 0) return -1;
        animation->setRate (static_cast<float> (rate));
    }
    return 1;
}
}

TextureAnimationObject::TextureAnimationObject (JSContext* context) :
    m_context (context), m_classId (0), m_instances (context) {
    m_exoticMethods.get_property = animationProperty;
    m_exoticMethods.set_property = setAnimationProperty;
    const JSClassDef definition {
        .class_name = "TextureAnimation",
        .finalizer = finalizeAnimation,
        .exotic = &m_exoticMethods,
    };
    m_classId = registerSceneScriptClass (JS_GetRuntime (context), definition);
}

JSValue TextureAnimationObject::instantiate (
    const void* layer, const std::shared_ptr<ImageTextureAnimation>& animation) {
    if (!animation) return JS_NULL;
    JSValue cached = m_instances.find (layer);
    if (!JS_IsUndefined (cached)) return cached;
    JSValue result = JS_NewObjectClass (m_context, m_classId);
    if (JS_IsException (result)) return result;
    JS_SetOpaque (result, new AnimationHandle {animation});
    m_instances.retain (layer, result);
    return result;
}
