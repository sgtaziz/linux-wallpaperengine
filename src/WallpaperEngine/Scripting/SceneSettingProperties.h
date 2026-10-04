#pragma once

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Scripting/LayerPropertyWrites.h"

#include <array>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Scripting {
/** The registered host type of an IScene general property. */
enum class SceneSettingType { Bool, Float, Vec3 };

struct SceneSettingProperty {
    const char* name;
    SceneSettingType type;
    const Data::Model::UserSettingUniquePtr& (*setting) (const Data::Model::SceneData&);
};

/**
 * The IScene general properties (lib.sceneScript.d.ts: none are read-only).
 * Native registers each one in the general settings table with its
 * host type: 6 for the bool flags (bloom, clearenabled, camerafade,
 * camerashake, cameraparallax), 4 for floats (bloomstrength,
 * bloomthreshold, fov, nearz, farz, camerashake* and
 * cameraparallax*), and 2 for Vec3 colors (clearcolor, ambientcolor
 * skylightcolor). Each entry maps the name to its own field;
 * clearenabled is the scene clear flag, not bloom, and skylightcolor is not
 * the ambient color.
 */
inline const std::array<SceneSettingProperty, 19>& sceneSettingProperties () {
    using Data::Model::SceneData;
    using Data::Model::UserSettingUniquePtr;
    using enum SceneSettingType;
    static const std::array<SceneSettingProperty, 19> properties {{
        {"bloom", Bool, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.bloom.enabled; }},
        {"bloomstrength", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.bloom.strength; }},
        {"bloomthreshold", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.bloom.threshold; }},
        {"clearenabled", Bool, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.clearEnabled; }},
        {"clearcolor", Vec3, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.colors.clear; }},
        {"ambientcolor", Vec3, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.colors.ambient; }},
        {"skylightcolor", Vec3,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.colors.skylight; }},
        {"fov", Float, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.projection.fov; }},
        {"nearz", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.projection.nearz; }},
        {"farz", Float, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.projection.farz; }},
        {"camerafade", Bool, [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.fade; }},
        {"camerashake", Bool,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.shake.enabled; }},
        {"camerashakespeed", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.shake.speed; }},
        {"camerashakeamplitude", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.shake.amplitude; }},
        {"camerashakeroughness", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.shake.roughness; }},
        {"cameraparallax", Bool,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.parallax.enabled; }},
        {"cameraparallaxamount", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.parallax.amount; }},
        {"cameraparallaxdelay", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.parallax.delay; }},
        {"cameraparallaxmouseinfluence", Float,
         [] (const SceneData& s) -> const UserSettingUniquePtr& { return s.camera.parallax.mouseInfluence; }},
    }};
    return properties;
}

/**
 * Reads a Bool or Float general property. The native getter for type 4
 * returns the stored float, so a strength of 2.5 reads as 2.5.
 */
inline JSValue sceneSettingScalarValue (JSContext* context, const Data::Model::DynamicValue& value,
                                        SceneSettingType type) {
    return type == SceneSettingType::Bool ? JS_NewBool (context, value.getBool ())
                                          : JS_NewFloat64 (context, value.getFloat ());
}

/**
 * Stores a script write to a general property. Native general setters
 * store the converted
 * value and run the field's change callback, so the value is kept rather than
 * rejected. The value is converted with the scenescript host converter
 * cases (LayerPropertyWrites.h): a value of the wrong type is
 * skipped without an exception. Returns false only when the conversion threw.
 */
inline bool storeSceneSetting (JSContext* context, JSValueConst input, Data::Model::DynamicValue& value,
                               SceneSettingType type) {
    using Data::Model::DynamicValue;
    switch (type) {
        case SceneSettingType::Bool: {
            bool converted = false;
            if (convertLayerBoolean (input, converted) == LayerWrite::Assign)
                value.update (converted, DynamicValue::Script);
            return true;
        }
        case SceneSettingType::Float: {
            float converted = 0.0f;
            const auto result = convertLayerFloat (context, input, converted);
            if (result == LayerWrite::Assign) value.update (converted, DynamicValue::Script);
            return result != LayerWrite::Exception;
        }
        case SceneSettingType::Vec3: {
            float components[3] {};
            const auto result = convertLayerVector (context, input, 3, false, components);
            if (result == LayerWrite::Assign)
                value.update (glm::vec3 (components[0], components[1], components[2]), DynamicValue::Script);
            return result != LayerWrite::Exception;
        }
    }
    return true;
}
} // namespace WallpaperEngine::Scripting
