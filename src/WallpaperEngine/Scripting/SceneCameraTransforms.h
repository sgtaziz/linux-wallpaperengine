#pragma once

#include "quickjs.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include <glm/vec3.hpp>
#include <optional>

namespace WallpaperEngine::Scripting {

struct SceneCameraTransforms {
    std::optional<glm::vec3> eye;
    std::optional<glm::vec3> center;
    std::optional<glm::vec3> up;
    std::optional<float> zoom;
};

// The installed 2.8.42 DLL's 1816359e0 accepts object vector fields only.
// Its 181620090 initializes XYZ to zero, replacing numeric components only;
// this deliberately differs from the strict general-purpose Vec3 adapter.
// A property getter exception remains pending and returns nullopt.
inline std::optional<SceneCameraTransforms> readSceneCameraTransforms (
    JSContext* context, JSValueConst object) {
    SceneCameraTransforms result;
    const char* fields[] = {"eye", "center", "up", "zoom"};
    JSValue values[] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    WallpaperEngine::Data::Utils::ScopeGuard valuesGuard ([&] {
        for (auto value : values) JS_FreeValue (context, value);
    });
    // Read all four outer fields before converting the vectors, as native
    // does. A later field getter may mutate an earlier vector object.
    for (int field = 0; field < 4; ++field) {
        values[field] = JS_GetPropertyStr (context, object, fields[field]);
        if (JS_IsException (values[field])) return std::nullopt;
    }
    std::optional<glm::vec3>* vectors[] = {&result.eye, &result.center, &result.up};
    for (int field = 0; field < 3; ++field) {
        JSValue value = values[field];
        if (JS_IsObject (value)) {
            glm::vec3 vector (0.0f);
            for (int component = 0; component < 3; ++component) {
                const char name[] = {"xyz"[component], '\0'};
                JSValue part = JS_GetPropertyStr (context, value, name);
                if (JS_IsException (part)) {
                    return std::nullopt;
                }
                if (JS_IsNumber (part)) {
                    double number;
                    if (JS_ToFloat64 (context, &number, part) < 0) {
                        JS_FreeValue (context, part);
                        return std::nullopt;
                    }
                    vector[component] = static_cast<float> (number);
                }
                JS_FreeValue (context, part);
            }
            *vectors[field] = vector;
        }
    }
    JSValue zoom = values[3];
    if (JS_IsNumber (zoom)) {
        double number;
        if (JS_ToFloat64 (context, &number, zoom) < 0) {
            return std::nullopt;
        }
        result.zoom = static_cast<float> (number);
    }
    return result;
}

} // namespace WallpaperEngine::Scripting
