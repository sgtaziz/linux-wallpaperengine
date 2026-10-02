#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <array>

namespace WallpaperEngine::Scripting {
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Model::Image;
using WallpaperEngine::Data::Model::Particle;
using WallpaperEngine::Data::Model::ScenePointLight;
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
    // Native 1f3460 registers camera controls alongside base layer transforms.
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
