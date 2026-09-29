#include "InputObject.h"

#include "EngineObject.h"
#include "ScriptEngine.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"

#include <mutex>
#include <set>

using namespace WallpaperEngine::Scripting;

namespace {
std::mutex inputRegistryMutex;
std::set<InputObject*> inputRegistry;
}

InputObject* InputObject::lookup (JSValueConst value) {
    JSClassID classId = 0;
    auto* candidate = static_cast<InputObject*> (JS_GetAnyOpaque (value, &classId));
    const std::lock_guard lock (inputRegistryMutex);
    return inputRegistry.contains (candidate) && candidate->m_classId == classId ? candidate : nullptr;
}

JSValue get_cursor_world_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* input = InputObject::lookup (this_val);
    if (!input) return JS_ThrowTypeError (ctx, "Invalid input receiver");
    const auto position = input->getScene ().getMouseWorldPosition ();
    if (!position) return JS_ThrowTypeError (ctx, "Cursor world position requires a 2D scene camera");
    auto& adapter = input->getScene ().getScriptEngine ().getAdapters ().vec3;
    JSValue result = adapter->instantiate ();
    JS_SetPropertyStr (ctx, result, "x", JS_NewFloat64 (ctx, position->x));
    JS_SetPropertyStr (ctx, result, "y", JS_NewFloat64 (ctx, position->y));
    JS_SetPropertyStr (ctx, result, "z", JS_NewFloat64 (ctx, position->z));
    return result;
}

JSValue get_cursor_screen_position (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* input = InputObject::lookup (this_val);
    if (!input) return JS_ThrowTypeError (ctx, "Invalid input receiver");
    const auto& position = input->getScene ().getMouseScreenPosition ();

    JSValue result = input->getScene ().getScriptEngine ().getAdapters ().vec2->instantiate ();

    JS_SetPropertyStr (ctx, result, "x", JS_NewFloat64 (ctx, position.x));
    JS_SetPropertyStr (ctx, result, "y", JS_NewFloat64 (ctx, position.y));

    return result;
}

JSValue get_cursor_left_down (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* input = InputObject::lookup (this_val);
    if (!input) return JS_ThrowTypeError (ctx, "Invalid input receiver");
    return JS_NewBool (ctx, input->getScene ().isMouseLeftDown ());
}

JSValue input_set_value (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) { return JS_EXCEPTION; }

InputObject::InputObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene) :
    m_scene (scene), m_engine (engine), m_classId (0) {
    this->m_definition = { .class_name = "IInput" };
    JS_NewClassID (this->m_engine.getRuntime (), &this->m_classId);
    JS_NewClass (this->m_engine.getRuntime (), this->m_classId, &this->m_definition);
    this->m_instance = JS_NewObjectClass (this->m_engine.getContext (), this->m_classId);

    JS_DupValue (this->m_engine.getContext (), this->m_instance);

    // set properties
    JS_SetOpaque (this->m_instance, this);
    {
        const std::lock_guard lock (inputRegistryMutex);
        inputRegistry.insert (this);
    }
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorWorldPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_world_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance,
	JS_NewAtom (this->m_engine.getContext (), "cursorScreenPosition"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_screen_position, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
    JS_DefinePropertyGetSet (
	this->m_engine.getContext (), this->m_instance, JS_NewAtom (this->m_engine.getContext (), "cursorLeftDown"),
	JS_NewCFunction (this->m_engine.getContext (), get_cursor_left_down, "get", 0),
	JS_NewCFunction (this->m_engine.getContext (), input_set_value, "set", 1), JS_PROP_ENUMERABLE
    );
}
InputObject::~InputObject () {
    {
        const std::lock_guard lock (inputRegistryMutex);
        inputRegistry.erase (this);
    }
    JS_FreeValue (this->m_engine.getContext (), this->m_instance);
}
