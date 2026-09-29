#include "ColorModule.h"

#include "WallpaperEngine/Scripting/ScriptEngine.h"

#include <algorithm>
#include <cmath>

using namespace WallpaperEngine::Scripting::Modules;

static uint32_t ColorModuleInstanceId = 0;
std::map<uint32_t, ColorModule&> colorModules;

JSValue wecolor_rgb2hsv (JSContext*, JSValueConst, int, JSValueConst*, int);
JSValue wecolor_hsv2rgb (JSContext*, JSValueConst, int, JSValueConst*, int);
JSValue wecolor_normalizecolor (JSContext*, JSValueConst, int, JSValueConst*, int);
JSValue wecolor_expandcolor (JSContext*, JSValueConst, int, JSValueConst*, int);

int wecolor_init (JSContext* ctx, JSModuleDef* m) {
    uint32_t instanceId = 0;
    for (const auto& [id, module] : colorModules)
        if (module.getDefinition () == m) { instanceId = id; break; }
    if (!instanceId) return -1;
    JS_SetModuleExport (ctx, m, "rgb2hsv", JS_NewCFunctionMagic (
        ctx, wecolor_rgb2hsv, "rgb2hsv", 1, JS_CFUNC_generic_magic, instanceId));
    JS_SetModuleExport (ctx, m, "hsv2rgb", JS_NewCFunctionMagic (
        ctx, wecolor_hsv2rgb, "hsv2rgb", 1, JS_CFUNC_generic_magic, instanceId));
    JS_SetModuleExport (ctx, m, "normalizeColor", JS_NewCFunctionMagic (
        ctx, wecolor_normalizecolor, "normalizeColor", 1, JS_CFUNC_generic_magic, instanceId));
    JS_SetModuleExport (ctx, m, "expandColor", JS_NewCFunctionMagic (
        ctx, wecolor_expandcolor, "expandColor", 1, JS_CFUNC_generic_magic, instanceId));
    return 0;
}

JSValue wecolor_rgb2hsv (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    if (JS_VALUE_GET_TAG (argv[0]) != JS_TAG_OBJECT) {
	return JS_EXCEPTION;
    }

    JSValue x = JS_GetPropertyStr (ctx, argv[0], "x");
    JSValue y = JS_GetPropertyStr (ctx, argv[0], "y");
    JSValue z = JS_GetPropertyStr (ctx, argv[0], "z");

    double xVal = 0.0f, yVal = 0.0f, zVal = 0.0f;

    JS_ToFloat64 (ctx, &xVal, x);
    JS_ToFloat64 (ctx, &yVal, y);
    JS_ToFloat64 (ctx, &zVal, z);

    JS_FreeValue (ctx, x);
    JS_FreeValue (ctx, y);
    JS_FreeValue (ctx, z);

    // Shipped assets/scripts/jsmodules/wecolor.js represents hue on [0, 1].
    float h, s, v;

    const double max = std::max ({xVal, yVal, zVal});
    const double min = std::min ({xVal, yVal, zVal});

    v = max;

    if (max == 0.0f) {
	s = 0;
	h = 0;
    } else if (max - min == 0.0f) {
	s = 0;
	h = 0;
    } else {
	s = (max - min) / max;

	if (max == xVal) {
	    h = (yVal - zVal) / (6 * (max - min)) + (yVal < zVal ? 1.0f : 0.0f);
	} else if (max == yVal) {
	    h = ((zVal - xVal) / (max - min) + 2.0f) / 6.0f;
	} else {
	    h = ((xVal - yVal) / (max - min) + 4.0f) / 6.0f;
	}
    }

    const auto it = colorModules.find (magic);

    if (it == colorModules.end ()) {
	return JS_UNDEFINED;
    }

    JSValue value = it->second.getEngine ().getAdapters ().vec3->instantiate ();

    JS_SetPropertyStr (ctx, value, "x", JS_NewFloat64 (ctx, h));
    JS_SetPropertyStr (ctx, value, "y", JS_NewFloat64 (ctx, s));
    JS_SetPropertyStr (ctx, value, "z", JS_NewFloat64 (ctx, v));

    return value;
}

JSValue wecolor_hsv2rgb (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    if (JS_VALUE_GET_TAG (argv[0]) != JS_TAG_OBJECT) {
	return JS_EXCEPTION;
    }

    JSValue x = JS_GetPropertyStr (ctx, argv[0], "x");
    JSValue y = JS_GetPropertyStr (ctx, argv[0], "y");
    JSValue z = JS_GetPropertyStr (ctx, argv[0], "z");

    double xVal = 0.0f, yVal = 0.0f, zVal = 0.0f;

    JS_ToFloat64 (ctx, &xVal, x);
    JS_ToFloat64 (ctx, &yVal, y);
    JS_ToFloat64 (ctx, &zVal, z);

    JS_FreeValue (ctx, x);
    JS_FreeValue (ctx, y);
    JS_FreeValue (ctx, z);

    // Match shipped wecolor.js: hue is turns, not degrees. Its positive hue
    // values may exceed one (as in original scene 1888636115), so wrap them.
    float r, g, b; // 0.0-1.0

    const double sector = std::floor (xVal * 6.0);
    int hi = static_cast<int> (std::fmod (sector, 6.0));
    if (hi < 0) hi += 6;
    float f = xVal * 6.0 - sector;
    float p = zVal * (1.0f - yVal);
    float q = zVal * (1.0f - yVal * f);
    float t = zVal * (1.0f - yVal * (1.0f - f));

    switch (hi) {
	case 0:
	    r = zVal, g = t, b = p;
	    break;
	case 1:
	    r = q, g = zVal, b = p;
	    break;
	case 2:
	    r = p, g = zVal, b = t;
	    break;
	case 3:
	    r = p, g = q, b = zVal;
	    break;
	case 4:
	    r = t, g = p, b = zVal;
	    break;
	case 5:
	    r = zVal, g = p, b = q;
	    break;
	default:
	    r = 0, g = 0, b = 0;
	    break;
    }

    const auto it = colorModules.find (magic);

    if (it == colorModules.end ()) {
	return JS_UNDEFINED;
    }

    JSValue value = it->second.getEngine ().getAdapters ().vec3->instantiate ();

    JS_SetPropertyStr (ctx, value, "x", JS_NewFloat64 (ctx, r));
    JS_SetPropertyStr (ctx, value, "y", JS_NewFloat64 (ctx, g));
    JS_SetPropertyStr (ctx, value, "z", JS_NewFloat64 (ctx, b));

    return value;
}

JSValue wecolor_normalizecolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    if (JS_VALUE_GET_TAG (argv[0]) != JS_TAG_OBJECT) {
	return JS_EXCEPTION;
    }

    const auto it = colorModules.find (magic);

    if (it == colorModules.end ()) {
	return JS_UNDEFINED;
    }

    JSValue x = JS_GetPropertyStr (ctx, argv[0], "x");
    JSValue y = JS_GetPropertyStr (ctx, argv[0], "y");
    JSValue z = JS_GetPropertyStr (ctx, argv[0], "z");

    if (!JS_IsNumber (x) || !JS_IsNumber (y) || !JS_IsNumber (z)) {
	return JS_EXCEPTION;
    }

    double xVal = 0.0f, yVal = 0.0f, zVal = 0.0f;

    JS_ToFloat64 (ctx, &xVal, x);
    JS_ToFloat64 (ctx, &yVal, y);
    JS_ToFloat64 (ctx, &zVal, z);

    JSValue value = it->second.getEngine ().getAdapters ().vec3->instantiate ();

    JS_SetPropertyStr (ctx, value, "x", JS_NewFloat64 (ctx, xVal / 255.0f));
    JS_SetPropertyStr (ctx, value, "y", JS_NewFloat64 (ctx, yVal / 255.0f));
    JS_SetPropertyStr (ctx, value, "z", JS_NewFloat64 (ctx, zVal / 255.0f));

    return value;
}

JSValue wecolor_expandcolor (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic) {
    if (argc != 1) {
	return JS_EXCEPTION;
    }

    if (JS_VALUE_GET_TAG (argv[0]) != JS_TAG_OBJECT) {
	return JS_EXCEPTION;
    }

    const auto it = colorModules.find (magic);

    if (it == colorModules.end ()) {
	return JS_UNDEFINED;
    }

    JSValue x = JS_GetPropertyStr (ctx, argv[0], "x");
    JSValue y = JS_GetPropertyStr (ctx, argv[0], "y");
    JSValue z = JS_GetPropertyStr (ctx, argv[0], "z");

    if (!JS_IsNumber (x) || !JS_IsNumber (y) || !JS_IsNumber (z)) {
	return JS_EXCEPTION;
    }

    double xVal = 0.0f, yVal = 0.0f, zVal = 0.0f;

    JS_ToFloat64 (ctx, &xVal, x);
    JS_ToFloat64 (ctx, &yVal, y);
    JS_ToFloat64 (ctx, &zVal, z);

    JSValue value = it->second.getEngine ().getAdapters ().vec3->instantiate ();

    JS_SetPropertyStr (ctx, value, "x", JS_NewFloat64 (ctx, xVal * 255.0f));
    JS_SetPropertyStr (ctx, value, "y", JS_NewFloat64 (ctx, yVal * 255.0f));
    JS_SetPropertyStr (ctx, value, "z", JS_NewFloat64 (ctx, zVal * 255.0f));

    return value;
}

ColorModule::ColorModule (ScriptEngine& engine) : ScriptModule (engine, "WEColor", wecolor_init) {
    this->m_instanceId = ++ColorModuleInstanceId;
    for (const char* name : {"rgb2hsv", "hsv2rgb", "normalizeColor", "expandColor"})
        JS_AddModuleExport (engine.getContext (), getDefinition (), name);

    colorModules.emplace (this->m_instanceId, *this);
}

ColorModule::~ColorModule () { colorModules.erase (this->m_instanceId); }
