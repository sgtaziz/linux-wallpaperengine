#include "EffectThisObject.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Scripting/Adapters/ScriptableObjectAdapter.h"

#include <limits>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Render::Objects::CImage;

namespace {
const WallpaperEngine::Data::Model::ImageEffect* effectAt (JSValueConst layer, int index) {
    const auto* image = dynamic_cast<const CImage*> (
        WallpaperEngine::Scripting::Adapters::ScriptableObjectAdapter::resolve (layer));
    if (!image || index < 0 || static_cast<size_t> (index) >= image->getImage ().effects.size ()) return nullptr;
    return image->getImage ().effects[static_cast<size_t> (index)].get ();
}

JSValue visibleGet (JSContext* context, JSValueConst, int, JSValueConst*, int index, JSValue* data) {
    const auto* effect = effectAt (data[0], index);
    if (!effect || !effect->visible || !effect->visible->value)
        return JS_ThrowTypeError (context, "Effect is no longer available");
    return JS_NewBool (context, effect->visible->value->getBool ());
}

JSValue visibleSet (JSContext* context, JSValueConst, int argc, JSValueConst* argv,
                    int index, JSValue* data) {
    const auto* effect = effectAt (data[0], index);
    if (!effect || !effect->visible || !effect->visible->value)
        return JS_ThrowTypeError (context, "Effect is no longer available");
    if (argc != 1 || !JS_IsBool (argv[0]))
        return JS_ThrowTypeError (context, "Effect visibility requires a boolean");
    effect->visible->value->update (JS_ToBool (context, argv[0]) != 0,
                                    WallpaperEngine::Data::Model::DynamicValue::Script);
    return JS_UNDEFINED;
}
}

JSValue WallpaperEngine::Scripting::createEffectThisObject (
    JSContext* context, JSValueConst layer, size_t effectIndex) {
    if (effectIndex > static_cast<size_t> (std::numeric_limits<int>::max ()))
        return JS_ThrowRangeError (context, "Too many image effects");
    const auto* effect = effectAt (layer, static_cast<int> (effectIndex));
    if (!effect) return JS_ThrowTypeError (context, "Effect is no longer available");
    JSValue result = JS_NewObject (context);
    if (JS_IsException (result)) return result;
    JSValue captured[] { JS_DupValue (context, layer) };
    JSValue getter = JS_NewCFunctionData (context, visibleGet, 0,
                                          static_cast<int> (effectIndex), 1, captured);
    JSValue setter = JS_NewCFunctionData (context, visibleSet, 1,
                                          static_cast<int> (effectIndex), 1, captured);
    JS_FreeValue (context, captured[0]);
    JSAtom visible = JS_NewAtom (context, "visible");
    const int defined = JS_DefinePropertyGetSet (context, result, visible, getter, setter,
                                                 JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom (context, visible);
    if (defined < 0) { JS_FreeValue (context, result); return JS_EXCEPTION; }
    JS_DefinePropertyValueStr (context, result, "name",
                               JS_NewString (context, effect->name.c_str ()), JS_PROP_ENUMERABLE);
    return result;
}
