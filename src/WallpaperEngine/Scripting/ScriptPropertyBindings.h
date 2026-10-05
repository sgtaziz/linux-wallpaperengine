#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace WallpaperEngine::Scripting {
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Model::Image;
using WallpaperEngine::Data::Model::Particle;
using WallpaperEngine::Data::Model::ScenePointLight;
using WallpaperEngine::Data::Model::Sound;
using WallpaperEngine::Data::Model::Text;

struct ScriptPropertyBinding {
    const char* name;
    DynamicValue& value;
};

// Transform-only scene nodes participate in the same script lifecycle as
// visible layers; child renderers resolve these live parent values.
inline auto scriptPropertyBindings (const WallpaperEngine::Data::Model::Object& object) {
    return std::array<ScriptPropertyBinding, 4> {{
        {"origin", *object.origin->value}, {"scale", *object.groupScale->value},
        {"angles", *object.groupAngles->value}, {"visible", *object.groupVisible->value},
    }};
}

// Native sound property lookup searches the sound table
// Built (volume, mintime...), then the layer's
// dynamic table, then falls back to the base layer lookup 
// over built (origin, scale, angles,
// parallaxDepth, sortorder, name, solid, disablepropagation). Those base vec3
// entries load, which reaches the generic loader 
// that retains "script". A sound layer therefore runs scripts on its
// transform values as well as on volume. "visible" lives in a different table
// that the sound vtable does not reach,
// so it is not a sound property: thisLayer.visible stays an expando.
inline std::vector<ScriptPropertyBinding> scriptPropertyBindings (const Sound& sound) {
    std::vector<ScriptPropertyBinding> result {
        {"origin", *sound.origin->value}, {"scale", *sound.groupScale->value},
        {"angles", *sound.groupAngles->value},
    };
    if (sound.volume && sound.volume->value) result.push_back ({"volume", *sound.volume->value});
    return result;
}

struct MaterialConstantBinding {
    std::string key;
    DynamicValue& value;
    const WallpaperEngine::Data::Model::ShaderConstantMap& constants;
};

// Native material loading keeps each pass's constantshadervalues
// and applies them through the material's property table (at
// 435). Those typed loaders reach the generic loader, which
// appends a "script" to the scene script list. The image's
// own model material is the one its base passes bind, so each of its pass
// constants is a script-bearing value like an effect material constant.
inline std::vector<MaterialConstantBinding> imageMaterialScriptBindings (const Image& image) {
    std::vector<MaterialConstantBinding> result;
    if (!image.model || !image.model->material) return result;
    const auto& passes = image.model->material->passes;
    for (size_t passIndex = 0; passIndex < passes.size (); ++passIndex) {
        const auto& constants = passes[passIndex]->constants;
        for (const auto& [name, setting] : constants)
            if (setting && setting->value)
                result.push_back ({"pass" + std::to_string (passIndex) + "_constant_" + name, *setting->value,
                                   constants});
    }
    return result;
}

// Bind concrete renderer values when present. Text has no typed angles field,
// so its previously exposed group angle remains an explicit fallback.
inline auto scriptPropertyBindings (const Image& image) {
    return std::array<ScriptPropertyBinding, 9> {{
	{"origin", *image.origin->value}, {"scale", *image.scale->value},
	{"angles", *image.angles->value}, {"visible", *image.visible->value},
	{"alpha", *image.alpha->value}, {"color", *image.color->value},
	{"brightness", *image.brightness->value},
	{"parallaxDepth", *image.parallaxDepth->value}, {"size", *image.size->value},
    }};
}

inline auto scriptPropertyBindings (const Particle& particle) {
    return std::array<ScriptPropertyBinding, 5> {{
	{"origin", *particle.origin->value}, {"scale", *particle.scale->value},
	{"angles", *particle.angles->value}, {"visible", *particle.visible->value},
	{"parallaxDepth", *particle.parallaxDepth->value},
    }};
}

inline auto scriptPropertyBindings (const WallpaperEngine::Data::Model::SceneCamera& camera) {
    // Native registers camera controls alongside base layer transforms.
    return std::array<ScriptPropertyBinding, 6> {{
        {"origin", *camera.origin->value}, {"scale", *camera.groupScale->value},
        {"angles", *camera.groupAngles->value}, {"visible", *camera.groupVisible->value},
        {"fov", *camera.fov->value}, {"zoom", *camera.zoom->value},
    }};
}

inline auto scriptPropertyBindings (const ScenePointLight& light) {
    return std::array<ScriptPropertyBinding, 8> {{
        {"origin", *light.origin->value}, {"scale", *light.groupScale->value},
        {"angles", *light.groupAngles->value}, {"visible", *light.groupVisible->value},
        {"color", *light.color->value}, {"intensity", *light.intensity->value},
        {"radius", *light.radius->value}, {"exponent", *light.exponent->value},
    }};
}

inline auto scriptPropertyBindings (const WallpaperEngine::Data::Model::SceneSpotLight& light) {
    return std::array<ScriptPropertyBinding, 11> {{
        {"origin", *light.origin->value}, {"scale", *light.groupScale->value},
        {"angles", *light.groupAngles->value}, {"visible", *light.groupVisible->value},
        {"color", *light.color->value}, {"intensity", *light.intensity->value},
        {"radius", *light.radius->value}, {"exponent", *light.exponent->value},
        {"innercone", *light.innerCone->value}, {"outercone", *light.outerCone->value},
        {"controlpoint", *light.controlPoint->value},
    }};
}

inline auto scriptPropertyBindings (const Text& text) {
    return std::array<ScriptPropertyBinding, 20> {{
	{"origin", *text.origin->value}, {"scale", *text.scale->value},
	// Text currently has no typed angles field or rotation consumer. Keep the
	// previously exposed generic property until text rotation is implemented.
	{"angles", *text.groupAngles->value}, {"visible", *text.visible->value},
	{"parallaxDepth", *text.parallaxDepth->value},
	{"color", *text.color->value},
	{"brightness", *text.brightness->value},
	{"alpha", *text.alpha->value}, {"pointSize", *text.pointSize->value},
	{"spacing", *text.spacing->value},
	{"limitRows", *text.limitRows->value}, {"maxRows", *text.maxRows->value},
	{"limitWidth", *text.limitWidth->value}, {"maxWidth", *text.maxWidth->value},
	{"limitUseEllipsis", *text.limitUseEllipsis->value},
	{"opaqueBackground", *text.opaqueBackground->value},
	{"backgroundColor", *text.backgroundColor->value},
	{"backgroundBrightness", *text.backgroundBrightness->value},
	{"padding", *text.padding->value},
	{"text", *text.text->value},
    }};
}
} // namespace WallpaperEngine::Scripting
