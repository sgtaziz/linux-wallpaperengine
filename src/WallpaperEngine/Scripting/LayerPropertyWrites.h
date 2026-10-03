#pragma once

#include "quickjs.h"

#include <string>

namespace WallpaperEngine::Scripting {
enum class LayerStringWrite { Assign, Ignore, Exception };

/**
 * Converts a value assigned to a String-typed layer property (text,
 * horizontalalign, verticalalign, alignment). Native 2.8.42 native
 * converts host property writes, and its String case
 * does this:
 * - a JS string is used as it is;
 * - null or undefined (oddball kinds 3 and 4) are skipped, leaving the
 *   property unchanged without an exception;
 * any other value goes through v8::Value::ToString, so
 *   `text = Date.now()` stores the decimal string.
 * If the ToString call throws (for example, for a Symbol), the exception
 * reaches the script.
 */
inline LayerStringWrite convertLayerString (JSContext* context, JSValueConst value, std::string& result) {
    if (JS_IsNull (value) || JS_IsUndefined (value)) return LayerStringWrite::Ignore;
    // Convert explicitly: JS_ToCStringLen replaces a failed Error-object
    // conversion with its message, but native propagates the exception.
    JSValue string = JS_ToString (context, value);
    if (JS_IsException (string)) return LayerStringWrite::Exception;
    size_t length = 0;
    const char* chars = JS_ToCStringLen (context, &length, string);
    JS_FreeValue (context, string);
    if (!chars) return LayerStringWrite::Exception;
    result.assign (chars, length);
    JS_FreeCString (context, chars);
    return LayerStringWrite::Assign;
}

/**
 * Stores a write to a name the layer does not expose as an ordinary data
 * property of the receiver. Native layer wrappers come from a v8
 * ObjectTemplate. Each host property is an accessor and each host method is
 * a data member: 2.7.3, and 2.8.42 SetAccessor with
 * getter and setter. The template never installs a
 * named interceptor: the 2.8.42 ObjectTemplate::SetHandler has
 * no callers. Writing an unknown name, such as `opacity` on a text layer,
 * therefore creates an expando property that later reads return. The
 * handler is not aborted.
 */
inline int defineLayerExpando (JSContext* context, JSValueConst receiver, JSAtom name, JSValueConst value,
                               int flags) {
    return JS_DefinePropertyValue (context, receiver, name, JS_DupValue (context, value),
                                   JS_PROP_C_W_E | (flags & (JS_PROP_THROW | JS_PROP_THROW_STRICT)));
}
}
