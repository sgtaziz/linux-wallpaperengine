#include "ObjectParser.h"
#include "EffectParser.h"
#include "MaterialParser.h"
#include "ModelParser.h"

#include "ShaderConstantParser.h"
#include "TextureParser.h"
#include "UserSettingParser.h"
#include "WallpaperEngine/Data/Builders/ColorBuilder.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Objects/ParticleInitialColor.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

namespace {
void mergeAuthoredNonNull (JSON& target, const JSON& overlay) {
    if (overlay.is_null ()) return;
    if (overlay.is_object ()) {
        if (!target.is_object ()) target = JSON::object ();
        for (const auto& field : overlay.items ())
            if (!field.value ().is_null ())
                mergeAuthoredNonNull (target[field.key ()], field.value ());
    } else if (overlay.is_array ()) {
        if (!target.is_array ()) target = JSON::array ();
        for (size_t index = 0; index < overlay.size (); ++index) {
            if (overlay[index].is_null ()) continue;
            while (target.size () <= index) target.push_back (nullptr);
            mergeAuthoredNonNull (target[index], overlay[index]);
        }
    } else {
        target = overlay;
    }
}

int dependencyInteger (const JSON& value, const char* field) {
    if (!value.is_number_integer ()) {
        throw std::invalid_argument (std::string ("Object dependency ") + field + " must be an integer");
    }
    const double number = value.get<double> ();
    if (number < std::numeric_limits<int>::min () || number > std::numeric_limits<int>::max ()) {
        throw std::invalid_argument (std::string ("Object dependency ") + field + " is out of range");
    }
    return value.get<int> ();
}

glm::vec2 textPaddingValue (const JSON& authored) {
    if (authored.is_number ()) {
        const double value = authored.get<double> ();
	if (!std::isfinite (value) || std::abs (value) > std::numeric_limits<float>::max ())
	    throw std::invalid_argument ("Text padding scalar is not a finite float");
	return glm::vec2 (static_cast<float> (value));
    }
    if (authored.is_string ()) {
        std::istringstream input (authored.get<std::string> ());
	input.imbue (std::locale::classic ());
	float horizontal = 0.0f, vertical = 0.0f;
	std::string remainder;
	if (!(input >> horizontal >> vertical) || (input >> remainder) || !std::isfinite (horizontal)
	    || !std::isfinite (vertical))
	    throw std::invalid_argument ("Text padding must contain exactly two finite numbers");
	return {horizontal, vertical};
    }
    throw std::invalid_argument ("Text padding must be a number or a two-number string");
}

UserSettingUniquePtr textPadding (const JSON& object, const Project& project) {
    const auto authored = object.optional ("padding");
    if (!authored.has_value ())
        return WallpaperEngine::Data::Builders::UserSettingBuilder::fromValue (glm::vec2 (32.0f));
    if (!authored->is_object ())
        return WallpaperEngine::Data::Builders::UserSettingBuilder::fromValue (textPaddingValue (*authored));

    JSON normalized = *authored;
    const auto value = authored->require ("value", "Text padding setting requires a value");
    const glm::vec2 parsed = textPaddingValue (value);
    std::ostringstream encoded;
    encoded.imbue (std::locale::classic ());
    encoded.precision (std::numeric_limits<float>::max_digits10);
    encoded << parsed.x << ' ' << parsed.y;
    normalized["value"] = encoded.str ();
    return UserSettingParser::parse (normalized, project.properties);
}
}

ObjectUniquePtr ObjectParser::parse (const JSON& it, const Project& project) {
    const auto imageIt = it.find ("image");
    const auto soundIt = it.find ("sound");
    const auto particleIt = it.find ("particle");
    const auto textIt = it.find ("text");
    const auto lightIt = it.find ("light");
    const auto modelIt = it.find ("model");
    const auto cameraIt = it.find ("camera");
    // use shape to refer to VolumeLight
    const auto shapeIt = it.find ("shape");

    // Parse base object data
    // Some particle objects have numeric 'name' fields, so handle type mismatches gracefully
    ObjectData basedata;
    try {
	basedata = ObjectData {
	    .id = it.require<int> ("id", "Object must have an id"),
	    .name = it.require<std::string> ("name", "Object must have a name"),
	    .dependencies = parseDependencies (it),
	    .typedDependencies = parseTypedDependencies (it),
	    .parent = it.optional<int> ("parent"),
	    .attachment = it.optional<std::string> ("attachment"),
	    .solid = it.optional ("solid", true),
	    .disablePropagation = it.optional ("disablepropagation", false),
	    .origin = it.user ("origin", project.properties, glm::vec3 (0.0f)),
	    .groupScale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .groupAngles = it.user ("angles", project.properties, glm::vec3 (0.0f)),
	    .groupVisible = it.user ("visible", project.properties, true),
	};
    } catch (const std::exception& e) {
	sLog.error ("Error parsing object base data: ", e.what ());
	const auto idIt = it.find ("id");
	const auto nameIt = it.find ("name");
	int id = (idIt != it.end () && idIt->is_number ()) ? idIt->get<int> () : -1;
	std::string name = "unknown";
	if (nameIt != it.end ()) {
	    if (nameIt->is_string ()) {
		name = nameIt->get<std::string> ();
	    } else if (nameIt->is_number ()) {
		name = std::to_string (nameIt->get<int> ());
	    }
	}
	basedata = ObjectData {
	    .id = id,
	    .name = name,
	    .dependencies = parseDependencies (it),
	    .typedDependencies = parseTypedDependencies (it),
	    .parent = it.optional<int> ("parent"),
	    .attachment = it.optional<std::string> ("attachment"),
	    .solid = it.optional ("solid", true),
	    .disablePropagation = it.optional ("disablepropagation", false),
	    .origin = it.user ("origin", project.properties, glm::vec3 (0.0f)),
	    .groupScale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .groupAngles = it.user ("angles", project.properties, glm::vec3 (0.0f)),
	    .groupVisible = it.user ("visible", project.properties, true),
	};
    }

    auto initialConfiguration = it;
    initialConfiguration.erase ("id");
    basedata.initialConfiguration = initialConfiguration.dump ();

    for (const auto& dependency : basedata.typedDependencies) {
	if (dependency.type == "emitterimage") continue;
	sLog.error ("Typed scene dependency is not bound at runtime: object_id=", basedata.id,
	            " target_id=", dependency.id, " index=", dependency.index,
	            " type=", dependency.type);
    }

    if (imageIt != it.end () && imageIt->is_string ()) {
	return parseImage (it, project, std::move (basedata), *imageIt);
    } else if (soundIt != it.end () && (soundIt->is_array () || soundIt->is_string ())) {
	return parseSound (it, project, std::move (basedata));
    } else if (particleIt != it.end () && !particleIt->is_null ()) {
	return parseParticle (it, project, std::move (basedata));
    } else if (textIt != it.end ()) {
	return parseText (it, project, std::move (basedata));
    } else if (modelIt != it.end () && modelIt->is_string ()) {
	const auto path = modelIt->get<std::string> ();
	if (path.empty ()) throw std::invalid_argument ("Scene model path must not be empty");
	const int skin = it.optional ("skin", 0);
	if (skin < 0) throw std::invalid_argument ("Scene model skin must be nonnegative");
	return std::make_unique<SceneModel> (std::move (basedata), SceneModelData {.path = path, .skin = skin});
    } else if (cameraIt != it.end () && cameraIt->is_string ()) {
	return std::make_unique<SceneCamera> (std::move (basedata), SceneCameraData {
	    .mode = cameraIt->get<std::string> (),
	    .path = it.optional<std::string> ("path", ""),
	    .fov = it.user ("fov", project.properties, 45.0f),
	    .zoom = it.user ("zoom", project.properties, 1.0f),
	});
    } else if (lightIt != it.end ()) {
        if (lightIt->is_string () && lightIt->get<std::string> () == "lspot") {
            // 14018ff60 constructor / 14025da80 descriptors: cones are degrees.
            return std::make_unique<SceneSpotLight> (std::move (basedata), SceneSpotLightData {
                .color = it.user ("color", project.properties, glm::vec3 (0.0f)),
                .intensity = it.user ("intensity", project.properties, 0.0f),
                .radius = it.user ("radius", project.properties, 1.0f),
                .exponent = it.user ("exponent", project.properties, 2.0f),
                .innerCone = it.user ("innercone", project.properties, 20.0f),
                .outerCone = it.user ("outercone", project.properties, 30.0f),
                .controlPoint = it.user ("controlpoint", project.properties, glm::vec3 (0.0f, 2.0f, 0.0f)),
                .castShadow = it.optional ("castshadow", false),
                .useCookie = it.optional ("usecookie", false),
            });
        }
	if (!lightIt->is_string () || (lightIt->get<std::string> () != "point"
	                              && lightIt->get<std::string> () != "lpoint")) {
	    sLog.error ("Scene light type is not supported yet: ", lightIt->dump ());
	} else {
	    return std::make_unique<ScenePointLight> (std::move (basedata), ScenePointLightData {
	        .lightingV1 = lightIt->get<std::string> () == "lpoint",
	        .color = it.user ("color", project.properties, glm::vec3 (0.0f)),
	        .intensity = it.user ("intensity", project.properties, 0.0f),
	        .radius = it.user ("radius", project.properties, 1.0f),
	        .exponent = it.user ("exponent", project.properties, 2.0f),
	    });
	}
    } else if (shapeIt != it.end ()) {
	sLog.error ("VolumeLight objects are not supported yet");
    } else {
	if (!it.optional ("solid", false)) {
	    // dump the object for now, might want to change later
	    // TODO: RE-EVALUATE IF THIS MAKES SENSE, THERE'S OBJECTS THAT CONTAIN OTHER OBJECTS AND THUS AREN'T REALLY
	    // ANYTHING SPECIAL
	    sLog.error ("Unknown object type found: ", it.dump ());
	}
    }

    return std::make_unique<Object> (std::move (basedata));
}

std::vector<int> ObjectParser::parseDependencies (const JSON& it) {
    const auto dependenciesIt = it.find ("dependencies");

    if (dependenciesIt == it.end () || !dependenciesIt->is_array ()) {
	return {};
    }

    std::vector<int> result = {};

    for (const auto& cur : *dependenciesIt) {
	if (cur.is_object ()) {
	    const auto id = cur.find ("id");
	    if (id == cur.end ()) throw std::invalid_argument ("Object dependency is missing id");
	    result.push_back (dependencyInteger (*id, "id"));
	} else {
	    result.push_back (dependencyInteger (cur, "id"));
	}
    }

    return result;
}

std::vector<ObjectDependency> ObjectParser::parseTypedDependencies (const JSON& it) {
    const auto dependenciesIt = it.find ("dependencies");
    if (dependenciesIt == it.end () || !dependenciesIt->is_array ()) return {};
    std::vector<ObjectDependency> result;
    for (const auto& cur : *dependenciesIt) {
	if (!cur.is_object ()) continue;
	const auto id = cur.find ("id");
	const auto index = cur.find ("index");
	const auto type = cur.find ("type");
	if (id == cur.end () || index == cur.end () || type == cur.end () || !type->is_string ()) {
	    throw std::invalid_argument ("Typed object dependency requires integer id/index and string type");
	}
	const auto mask = cur.find ("mask");
	if (mask != cur.end () && !mask->is_string ())
	    throw std::invalid_argument ("Typed object dependency mask must be a string");
	result.push_back ({ dependencyInteger (*id, "id"), dependencyInteger (*index, "index"),
	                    type->get<std::string> (),
	                    mask == cur.end () ? std::string {} : mask->get<std::string> () });
    }
    return result;
}

SoundUniquePtr ObjectParser::parseSound (const JSON& it, const Project& project, ObjectData base) {
    const auto soundIt = it.require ("sound", "Object must have a sound");
    std::vector<std::string> sounds = {};

    if (soundIt.is_string ()) {
	sounds.push_back (soundIt.get<std::string> ());
    } else if (soundIt.is_array ()) {
	for (const auto& cur : soundIt) sounds.push_back (cur.get<std::string> ());
    } else {
	throw std::invalid_argument ("Sound must be a file path or array of file paths");
    }

    return std::make_unique<Sound> (
	std::move (base),
	SoundData {
	    .playbackmode = it.optional<std::string> ("playbackmode"),
	    .sounds = sounds,
	    .volume = it.user ("volume", project.properties, 1.0f),
	    .minTime = it.optional<float> ("mintime", 0.0f),
	    .maxTime = it.optional<float> ("maxtime", 0.0f),
	    .startSilent = it.optional<bool> ("startsilent", false),
	    .spatialization = it.optional<bool> ("spatialization", false),
	    .attenuation = it.optional<float> ("attenuation", 1.0f),
	    .minDistance = it.optional<float> ("mindistance", 0.0f),
	}
    );
}

TextUniquePtr ObjectParser::parseText (const JSON& it, const Project& project, ObjectData base) {
    const auto effects = it.optional ("effects");
    return std::make_unique<Text> (
	std::move (base),
	TextData {
	    .text = UserSettingParser::parse (
	        it.require ("text", "Text object requires text"), project.properties, false, true),
	    .font = it.optional ("font", std::string ()),
	    .pointSize = it.user ("pointsize", project.properties, 32.0f),
	    .spacing = it.user ("spacing", project.properties, glm::vec2 (0.0f)),
	    .limitRows = it.user ("limitrows", project.properties, false),
	    .maxRows = it.user ("maxrows", project.properties, 1),
	    .limitWidth = it.user ("limitwidth", project.properties, false),
	    .maxWidth = it.user ("maxwidth", project.properties, 500.0f),
	    .limitUseEllipsis = it.user ("limituseellipsis", project.properties, false),
	    .opaqueBackground = it.user ("opaquebackground", project.properties, false),
	    .backgroundColor = it.color ("backgroundcolor", project.properties, Builders::ColorBuilder::Black, true),
	    .effects = effects.has_value ()
	        ? parseEffects (*effects, project)
	        : std::vector<ImageEffectUniquePtr> {},
	    .size = it.optional ("size", glm::vec2 (0.0f)),
	    .scale = it.user ("scale", project.properties, glm::vec3 (1.0f)),
	    .color = it.color ("color", project.properties, Builders::ColorBuilder::White, true),
	    .alpha = it.user ("alpha", project.properties, 1.0f),
	    .visible = it.user ("visible", project.properties, true),
	    .parallaxDepth = it.user ("parallaxDepth", project.properties, glm::vec2 (0.0f)),
	    .alignment = it.optional ("horizontalalign", it.optional ("alignment", std::string ("center"))),
	    .verticalalign = it.optional ("verticalalign", std::string ("center")),
            .padding = textPadding (it, project),
	}
    );
}

ImageUniquePtr
ObjectParser::parseImage (const JSON& it, const Project& project, ObjectData base, const std::string& image) {
    const auto& properties = project.properties;
    const auto& effects = it.optional ("effects");
    const auto& animationLayers = it.optional ("animationlayers");

    auto result = std::make_unique<Image> (
	std::move (base),
	ImageData {
	    .scale = it.user ("scale", properties, glm::vec3 (1.0f)),
	    .angles = it.user ("angles", properties, glm::vec3 (0.0f)),
	    .visible = it.user ("visible", properties, true),
	    .alpha = it.user ("alpha", properties, 1.0f),
	    .color = it.color ("color", properties, Builders::ColorBuilder::White, true),
	    .copyBackground = it.user ("copybackground", properties, true),
	    .alignment = it.optional ("horizontalalign", it.optional ("alignment", std::string ("center"))),
	    .size = it.user ("size", properties, glm::vec2 (0.0f)),
	    .parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (0.0f)),
	    .colorBlendMode = it.user ("colorBlendMode", properties, 0),
	    .brightness = it.user ("brightness", properties, 1.0f),
	    .model = ModelParser::load (project, image),
	    .effects = effects.has_value () ? parseEffects (*effects, project) : std::vector<ImageEffectUniquePtr> {},
	    .animationLayers = animationLayers.has_value () ? parseAnimationLayers (*animationLayers, project)
							    : std::vector<ImageAnimationLayerUniquePtr> {},
	}
    );

    const auto instance = it.optional ("instance");

    if (instance.has_value () && instance->is_object () && !result->model->material->passes.empty ()) {
	auto& firstPass = **result->model->material->passes.begin ();
	const auto instanceTextures = instance->optional ("textures");

	if (instanceTextures.has_value ()) {
	    const auto parsed = TextureParser::parseTextureMap (*instanceTextures);
	    for (const auto& [slot, texture] : parsed) firstPass.textures.insert_or_assign (slot, texture);
	}

	const auto instanceUserTextures = instance->optional ("usertextures");

	if (instanceUserTextures.has_value ()) {
	    const auto parsed = TextureParser::parseUserTextureMap (*instanceUserTextures);
	    for (const auto& [slot, texture] : parsed) firstPass.usertextures.insert_or_assign (slot, texture);
	}

	const auto instanceCombos = instance->optional ("combos");
	if (instanceCombos.has_value ()) {
	    JSON nonNull = JSON::object ();
	    if (instanceCombos->is_object ())
		for (const auto& field : instanceCombos->items ())
		    if (!field.value ().is_null ()) nonNull[field.key ()] = field.value ();
	    const auto parsed = parseComboMap (nonNull);
	    for (const auto& [name, value] : parsed) firstPass.combos.insert_or_assign (name, value);
	}

	const auto instanceConstants = instance->optional ("constantshadervalues");
	if (instanceConstants.has_value () && instanceConstants->is_object () && !instanceConstants->empty ()) {
	    // Native material loading merges the instance JSON into pass zero before
	    // parsing constants. Keep nested script/user metadata when an instance
	    // changes only the value of a structured constant.
	    const auto& filename = result->model->material->filename;
	    const JSON source = WallpaperEngine::Data::JSON::parseAuthoringJson (
		project.assetLocator->readString (filename), filename);
	    const auto passes = source.require ("passes", "Material must have passes to render");
	    JSON merged = passes.at (0).optional ("constantshadervalues").value_or (JSON::object ());
	    mergeAuthoredNonNull (merged, *instanceConstants);
	    firstPass.constants = ShaderConstantParser::parse (merged, project);
	}
    }

    return result;
}

std::vector<ImageEffectUniquePtr> ObjectParser::parseEffects (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageEffectUniquePtr> result = {};

    for (const auto& cur : it) {
	result.push_back (parseEffect (cur, project));
    }

    return result;
}

ImageEffectUniquePtr ObjectParser::parseEffect (const JSON& it, const Project& project) {
    const auto& passsOverrides = it.optional ("passes");
    return std::make_unique<ImageEffect> (ImageEffect {
	.id = it.optional<int> ("id", -1),
	.name = it.optional<std::string> ("name", "Effect without name"),
	.visible = it.user ("visible", project.properties, true),
	.passOverrides = passsOverrides.has_value () ? parseEffectPassOverrides (passsOverrides.value (), project)
						     : std::vector<ImageEffectPassOverrideUniquePtr> {},
	.effect = EffectParser::load (project, it.require ("file", "Image effect must have an effect")) });
}

std::vector<ImageEffectPassOverrideUniquePtr>
ObjectParser::parseEffectPassOverrides (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageEffectPassOverrideUniquePtr> result = {};

    for (const auto& cur : it) {
	result.push_back (parseEffectPass (cur, project));
    }

    return result;
}

ImageEffectPassOverrideUniquePtr ObjectParser::parseEffectPass (const JSON& it, const Project& project) {
    const auto& combos = it.optional ("combos");
    const auto& textures = it.optional ("textures");
    const auto& constants = it.optional ("constantshadervalues");
    const auto& usertextures = it.optional ("usertextures");

    // TODO: PARSE CONSTANT SHADER VALUES AND FIND REFS?
    return std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
	.id = it.optional<int> ("id", -1),
	.combos = combos.has_value () ? parseComboMap (combos.value ()) : ComboMap {},
	.constants
	= constants.has_value () ? ShaderConstantParser::parse (constants.value (), project) : ShaderConstantMap {},
	.textures = textures.has_value () ? TextureParser::parseTextureMap (textures.value ()) : TextureMap {},
	.usertextures
	= usertextures.has_value () ? TextureParser::parseUserTextureMap (usertextures.value ()) : UserTextureMap {},
    });
}

ComboMap ObjectParser::parseComboMap (const JSON& it) {
    if (!it.is_object ()) {
	return {};
    }

    ComboMap result = {};

    for (const auto& cur : it.items ()) {
	result.emplace (cur.key (), cur.value ());
    }

    return result;
}

std::vector<ImageAnimationLayerUniquePtr> ObjectParser::parseAnimationLayers (const JSON& it, const Project& project) {
    if (!it.is_array ()) {
	return {};
    }

    std::vector<ImageAnimationLayerUniquePtr> result = {};

    for (const auto& cur : it.items ()) {
	result.push_back (parseAnimationLayer (cur.value (), project));
    }

    return result;
}

ImageAnimationLayerUniquePtr ObjectParser::parseAnimationLayer (const JSON& it, const Project& project) {
    const auto& properties = project.properties;

    return std::make_unique<ImageAnimationLayer> (ImageAnimationLayer {
	.id = it.require<int> ("id", "Animation layer must have an id"),
	.rate = it.user ("rate", properties, 1.0f),
	.visible = it.user ("visible", properties, false),
	.blend = it.user ("blend", properties, 1.0f),
	.animation = it.user ("animation", properties, 0),
	.additive = it.optional ("additive", false),
	.blendIn = it.optional ("blendin", false),
	.blendOut = it.optional ("blendout", false),
	.blendTime = it.optional ("blendtime", 0.5f),
    });
}

ParticleUniquePtr ObjectParser::parseParticle (const JSON& it, const Project& project, ObjectData base) {
    try {
	const auto& properties = project.properties;
	const auto particleIt = it.find ("particle");

	if (particleIt == it.end ()) {
	    sLog.error ("Particle object must have a particle definition");
	    return std::make_unique<Particle> (
		std::move (base),
		ParticleData {
		    .scale = it.user ("scale", properties, glm::vec3 (1.0f)),
		    .angles = it.user ("angles", properties, glm::vec3 (0.0f)),
		    .visible = it.user ("visible", properties, true),
		    .parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (0.0f)),
		    .particleFile = "",
		    .animationMode = "sequence",
		    .sequenceMultiplier = 1.0f,
		    .maxCount = 100,
		    .startTime = 0,
		    .flags = 0,
		    .material = nullptr,
		    .emitters = {},
		    .initializers = {},
		    .operators = {},
		    .renderers = {},
		    .controlPoints = {},
		    .children = {},
		    .instanceOverride = {
		        .enabled = Builders::UserSettingBuilder::fromValue(false),
			.alpha = Builders::UserSettingBuilder::fromValue(1.0f),
			.brightness = Builders::UserSettingBuilder::fromValue(1.0f),
			.size = Builders::UserSettingBuilder::fromValue(1.0f),
			.lifetime = Builders::UserSettingBuilder::fromValue(1.0f),
			.rate = Builders::UserSettingBuilder::fromValue(1.0f),
			.speed = Builders::UserSettingBuilder::fromValue(1.0f),
			.count = Builders::UserSettingBuilder::fromValue(1.0f),
			.color = Builders::UserSettingBuilder::fromValue(1.0f),
			.colorn = Builders::UserSettingBuilder::fromValue(glm::vec3 (-1.0f)),
		    },
		}
	    );
	}

	std::string particleFile;
	if (particleIt->is_string ()) {
	    particleFile = particleIt->get<std::string> ();
	}

	// Load particle definition from file if it's a string reference
	JSON particleJson = JSON::object ();
	if (!particleFile.empty ()) {
	    try {
		particleJson = WallpaperEngine::Data::JSON::parseAuthoringJson (
		    project.assetLocator->readString (particleFile), particleFile);
	    } catch (std::runtime_error& e) {
		sLog.error ("Cannot load particle file: ", particleFile, " - ", e.what ());
	    }
	} else if (particleIt->is_object ()) {
	    particleJson = *particleIt;
	}
	const auto reportUnsupported = [&] (const char* component, size_t index, const JSON& entry,
	                                    const char* action) {
	    const auto nameIt = entry.find ("name");
	    const std::string name = nameIt != entry.end () && nameIt->is_string ()
	        ? nameIt->get<std::string> () : "<missing>";
	    sLog.error ("Unsupported particle component: object_id=", base.id,
	                " source=", particleFile.empty () ? "<inline>" : particleFile,
	                " path=particle.", component, "[", index, "] name=", name,
	                " action=", action);
	};

	// Parse emitters (note: field is named "emitter" not "emitters")
	std::vector<ParticleEmitter> emitters;
	const auto emittersIt = particleJson.find ("emitter");
	if (emittersIt != particleJson.end () && emittersIt->is_array ()) {
	    for (const auto& emitter : *emittersIt) {
		emitters.push_back (parseParticleEmitter (emitter));
	    }
	}

	// Parse initializers (note: field is named "initializer" not "initializers")
	std::vector<ParticleInitializerUniquePtr> initializers;
	const auto initializersIt = particleJson.find ("initializer");
	if (initializersIt != particleJson.end () && initializersIt->is_array ()) {
	    for (size_t index = 0; index < initializersIt->size (); ++index) {
		const auto& initializer = (*initializersIt)[index];
		auto init = parseParticleInitializer (
                    initializer, project.properties, project.sceneOrthogonalProjection,
                    particleJson.optional<uint32_t> ("flags", 0));
		if (init) {
		    initializers.push_back (std::move (init));
		} else {
		    reportUnsupported ("initializer", index, initializer, "skipped");
		}
	    }
	}

	// Parse operators (note: field is named "operator" not "operators")
	std::vector<ParticleOperatorUniquePtr> operators;
	const auto operatorsIt = particleJson.find ("operator");
	if (operatorsIt != particleJson.end () && operatorsIt->is_array ()) {
	    for (size_t index = 0; index < operatorsIt->size (); ++index) {
	const auto& op = (*operatorsIt)[index];
		auto oper = parseParticleOperator (op, project.properties, false, project.sceneOrthogonalProjection);
		if (oper) {
		    const bool hasBlend = op.find ("blendinstart") != op.end ()
		        || op.find ("blendinend") != op.end ()
		        || op.find ("blendoutstart") != op.end ()
		        || op.find ("blendoutend") != op.end ();
		    if (hasBlend) {
			oper->blendEnvelope.emplace (ParticleOperatorBase::BlendEnvelope {
			    op.user ("blendinstart", project.properties, 0.0f),
			    op.user ("blendinend", project.properties, 0.0f),
			    op.user ("blendoutstart", project.properties, 1.0f),
			    op.user ("blendoutend", project.properties, 1.0f),
			});
		    }
		    operators.push_back (std::move (oper));
		} else {
		    reportUnsupported ("operator", index, op, "skipped");
		}
	    }
	}

	// Parse renderers (note: field is named "renderer" not "renderers")
	std::vector<ParticleRenderer> renderers;
	const auto renderersIt = particleJson.find ("renderer");
	if (renderersIt != particleJson.end () && renderersIt->is_array ()) {
	    for (size_t index = 0; index < renderersIt->size (); ++index) {
		const auto& renderer = (*renderersIt)[index];
		const auto nameIt = renderer.find ("name");
		if (nameIt != renderer.end () && nameIt->is_string ()) {
		    const auto& name = nameIt->get_ref<const std::string&> ();
		    if (name != "sprite" && name != "spritetrail" && name != "rope" && name != "ropetrail") {
			reportUnsupported ("renderer", index, renderer, "sprite_fallback");
		    }
		}
		renderers.push_back (parseParticleRenderer (renderer));
	    }
	}

	// Add default sprite renderer if none specified
	if (renderers.empty ()) {
	    renderers.push_back (
		ParticleRenderer {
		    .name = "sprite",
		    .orientation = "screen",
		    .axis = glm::vec3 (0.0f),
		    .flags = 0,
		    .length = 0.05f,
		    .maxLength = 10.0f,
		    .minLength = 0.0f,
		    .subdivision = 1.0f,
		    .segments = 4.0f,
		    .uvScale = 1.0f,
		    .uvScrolling = false,
		    .uvSmoothing = true,
		    .fadeAlpha = false,
		    .fadeSize = false,
		}
	    );
	}

	// Parse control points (note: field is named "controlpoint" not "controlpoints")
	std::vector<ParticleControlPoint> controlPoints;
	const auto controlPointsIt = particleJson.find ("controlpoint");
	if (controlPointsIt != particleJson.end () && controlPointsIt->is_array ()) {
            // Native 1401c5490 reads slots 0..7 by array index. The editor's
            // id annotation does not select a runtime slot; skipped records
            // retain their original array position.
            const size_t count = std::min<size_t> (8, controlPointsIt->size ());
            for (size_t index = 0; index < count; ++index) {
                const auto& cp = (*controlPointsIt)[index];
                if (cp.is_object ())
                    controlPoints.push_back (parseParticleControlPoint (cp, static_cast<int> (index)));
            }
	}

	// Parse children
	std::vector<ParticleChild> children;
	const auto childrenIt = particleJson.optional ("children");
	if (childrenIt.has_value () && childrenIt->is_array ()) {
	    for (const auto& child : *childrenIt) {
		children.push_back (parseParticleChild (child, project));
	    }
	}

	// Parse instance override
	ParticleInstanceOverride instanceOverride = {
	    .enabled = Builders::UserSettingBuilder::fromValue (false),
	    .alpha = Builders::UserSettingBuilder::fromValue (1.0f),
	    .brightness = Builders::UserSettingBuilder::fromValue (1.0f),
	    .size = Builders::UserSettingBuilder::fromValue (1.0f),
	    .lifetime = Builders::UserSettingBuilder::fromValue (1.0f),
	    .rate = Builders::UserSettingBuilder::fromValue (1.0f),
	    .speed = Builders::UserSettingBuilder::fromValue (1.0f),
	    .count = Builders::UserSettingBuilder::fromValue (1.0f),
	    .color = Builders::UserSettingBuilder::fromValue (1.0f),
	    .colorn = Builders::UserSettingBuilder::fromValue (glm::vec3 (-1.0f)),
	};
	const auto instanceOverrideIt = it.optional ("instanceoverride");
	if (instanceOverrideIt.has_value ()) {
	    instanceOverride = parseParticleInstanceOverride (*instanceOverrideIt, project.properties);
	}

	// Parse material - particles reference materials directly, not models
	ModelUniquePtr material = nullptr;
	const auto materialIt = particleJson.find ("material");
	if (materialIt != particleJson.end () && materialIt->is_string ()) {
	    try {
		std::string materialPath = materialIt->get<std::string> ();

		// Particle materials are stored as just material definitions, not model files
		// So we need to wrap them in a model structure
		auto mat = MaterialParser::load (project, materialPath);

		material = std::make_unique<ModelStruct> (ModelStruct {
		    .filename = materialPath,
		    .material = std::move (mat),
		    .solidlayer = false,
		    .fullscreen = false,
		    .passthrough = false,
		    .autosize = false,
		    .nopadding = false,
		    .width = std::nullopt,
		    .height = std::nullopt,
		    .puppet = std::nullopt,
		});
	    } catch (std::runtime_error& e) {
		sLog.error ("Cannot load particle material: ", materialIt->get<std::string> (), " - ", e.what ());
	    }
	}

	// Parse string fields safely
	std::string animationMode = "sequence";
	const auto animModeIt = particleJson.find ("animationmode");
	if (animModeIt != particleJson.end () && animModeIt->is_string ()) {
	    animationMode = animModeIt->get<std::string> ();
	}

	// Parse numeric fields safely
	float sequenceMultiplier = 1.0f;
	uint32_t maxCount = 100;
	float startTime = 0.0f;
	uint32_t flags = 0;
	// 1401c45f0 synthesizes the compiled preset's colorn/hascolor from its
	// color components, overwriting any top-level authored values. Color-random
	// uses the midpoint; color-change's start color takes precedence afterward.
	glm::vec3 presetColorN (1.0f);
	bool presetHasColor = false;
	const bool extendedBirthColors = std::any_of (initializers.begin (), initializers.end (), [] (const auto& initializer) {
	    return initializer->template is<HsvColorRandomInitializer> ()
	        || initializer->template is<ColorListInitializer> ();
	});
	for (const auto& initializer : initializers) {
	    if (initializer->is<ColorRandomInitializer> ()) {
		const auto* random = initializer->as<ColorRandomInitializer> ();
		presetColorN = (random->min->value->getVec3 ()
		    + random->max->value->getVec3 ()) * 0.5f;
		presetHasColor = true;
		if (!extendedBirthColors) break;
	    } else if (initializer->is<HsvColorRandomInitializer> ()) {
	        const auto* hsv = initializer->as<HsvColorRandomInitializer> ();
	        const glm::vec3 midpoint (
	            (hsv->hueMin->value->getFloat () + hsv->hueMax->value->getFloat ()) * 0.5f,
	            (hsv->saturationMin->value->getFloat () + hsv->saturationMax->value->getFloat ()) * 0.5f,
	            (hsv->valueMin->value->getFloat () + hsv->valueMax->value->getFloat ()) * 0.5f);
	        presetColorN = WallpaperEngine::Render::Objects::ParticleCore::nativeInitialHsvToRgb (
	            glm::clamp (midpoint, glm::vec3 (0.0f), glm::vec3 (1.0f)));
	        presetHasColor = true;
	    } else if (initializer->is<ColorListInitializer> ()) {
	        const auto* colors = initializer->as<ColorListInitializer> ();
	        if (!colors->colors.empty ()) {
	            presetColorN = colors->colors[0]->value->getVec3 ();
	            presetHasColor = true;
	        }
	    }
	}
	// New color families follow native last-initializer/fallback precedence.
	// The accepted legacy-only path retains its prior colorchange behavior;
	// changing that path requires a separate native/user regression batch.
	for (const auto& op : operators) {
	    if (op->is<ColorChangeOperator> () && (!extendedBirthColors || !presetHasColor)) {
		const auto* change = op->as<ColorChangeOperator> ();
		presetColorN = change->startValue->value->getVec3 ();
		presetHasColor = true;
		break;
	    }
	}

	const auto seqMultIt = particleJson.find ("sequencemultiplier");
	if (seqMultIt != particleJson.end () && seqMultIt->is_number ()) {
	    sequenceMultiplier = seqMultIt->get<float> ();
	}

	const auto maxCountIt = particleJson.find ("maxcount");
	if (maxCountIt != particleJson.end () && !maxCountIt->is_null ()) {
	    if (!maxCountIt->is_number_integer ()) {
		throw std::invalid_argument ("Particle maxcount must be a nonnegative integer");
	    }
	    if (maxCountIt->is_number_unsigned ()) {
		const auto value = maxCountIt->get<uint64_t> ();
		if (value > std::numeric_limits<uint32_t>::max ()) {
		    throw std::invalid_argument ("Particle maxcount exceeds uint32 range");
		}
		maxCount = static_cast<uint32_t> (value);
	    } else {
		const auto value = maxCountIt->get<int64_t> ();
		if (value < 0 || static_cast<uint64_t> (value) > std::numeric_limits<uint32_t>::max ()) {
		    throw std::invalid_argument ("Particle maxcount must fit a nonnegative uint32");
		}
		maxCount = static_cast<uint32_t> (value);
	    }
	}

	const auto startTimeIt = particleJson.find ("starttime");
	if (startTimeIt != particleJson.end () && startTimeIt->is_number ()) {
	    startTime = startTimeIt->get<float> ();
	    if (!std::isfinite (startTime) || startTime < 0.0f) {
		throw std::invalid_argument ("Particle starttime must be finite and nonnegative");
	    }
	}

	const auto flagsIt = particleJson.find ("flags");
	if (flagsIt != particleJson.end () && flagsIt->is_number ()) {
	    flags = flagsIt->get<uint32_t> ();
	}

	return std::make_unique<Particle> (
	    std::move (base),
	    ParticleData {
		.scale = it.user ("scale", properties, glm::vec3 (1.0f)),
		.angles = it.user ("angles", properties, glm::vec3 (0.0f)),
		.visible = it.user ("visible", properties, true),
		.parallaxDepth = it.user ("parallaxDepth", properties, glm::vec2 (0.0f)),
		.particleFile = particleFile,
		.animationMode = animationMode,
		.sequenceMultiplier = sequenceMultiplier,
		.maxCount = maxCount,
		.startTime = startTime,
		.flags = flags,
		.presetColorN = presetColorN,
		.presetHasColor = presetHasColor,
		.presetTintCompiled = (flags & 0x200000u) != 0
		    || !presetHasColor || project.sceneVersion < 5,
		.material = std::move (material),
		.emitters = std::move (emitters),
		.initializers = std::move (initializers),
		.operators = std::move (operators),
		.renderers = std::move (renderers),
		.controlPoints = std::move (controlPoints),
		.children = std::move (children),
		.instanceOverride = std::move (instanceOverride),
	    }
	);
    } catch (nlohmann::json::exception& e) {
	sLog.error ("Error parsing particle '", base.name, "': ", e.what ());
	sLog.error ("Particle JSON: ", it.dump ());
	throw;
    }
}

ParticleEmitter ObjectParser::parseParticleEmitter (const JSON& it) {
    // Parse string name safely
    std::string name;
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    // Helper lambda to parse vec3 fields that might be strings, arrays, single numbers, or missing
    auto parseVec3 = [&] (const char* fieldName, const glm::vec3& defaultValue) -> glm::vec3 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_number ()) {
	    // Single number - use for all components (common for distancemax/distancemin)
	    float val = fieldIt->get<float> ();
	    return glm::vec3 (val, val, val);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 3) {
	    return glm::vec3 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> (), (*fieldIt)[2].get<float> ());
	}
	return defaultValue;
    };

    auto parseIVec3 = [&] (const char* fieldName, const glm::ivec3& defaultValue) -> glm::ivec3 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 3) {
	    return glm::ivec3 ((*fieldIt)[0].get<int> (), (*fieldIt)[1].get<int> (), (*fieldIt)[2].get<int> ());
	}
	return defaultValue;
    };

    auto parseVec2 = [&] (const char* fieldName, const glm::vec2& defaultValue) -> glm::vec2 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 2) {
	    return glm::vec2 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> ());
	}
	return defaultValue;
    };

    try {
	const float rate = it.optional ("rate", 10.0f);
	if (!std::isfinite (rate) || rate < 0.0f) {
	    throw std::invalid_argument ("Particle emitter rate must be finite and nonnegative");
	}
	return ParticleEmitter {
	    .id = it.optional ("id", -1),
	    .name = name,
	    .directions = parseVec3 ("directions", glm::vec3 (1.0f, 1.0f, 0.0f)),
	    .distanceMin = parseVec3 ("distancemin", glm::vec3 (0.0f, 0.0f, 0.0f)),
	    .distanceMax = parseVec3 ("distancemax", glm::vec3 (256.0f, 256.0f, 0.0f)),
	    .distanceMaxAuthored = it.find ("distancemax") != it.end (),
	    .origin = parseVec3 ("origin", glm::vec3 (0.0f)),
	    .offsetMin = parseVec3 ("offsetmin", name == "layerimage"
	        ? glm::vec3 (-5.0f, -5.0f, 0.0f) : glm::vec3 (0.0f)),
	    .offsetMax = parseVec3 ("offsetmax", name == "layerimage"
	        ? glm::vec3 (5.0f, 5.0f, 0.0f) : glm::vec3 (0.0f)),
	    .sign = parseIVec3 ("sign", glm::ivec3 (0)),
	    .instantaneous = it.optional ("instantaneous", 0u),
	    .speedMin = it.optional ("speedmin", name == "layerimage" ? 0.1f : 0.0f),
	    .speedMax = it.optional ("speedmax", name == "layerimage" ? 0.2f : 0.0f),
	    .rate = rate,
	    .controlPoint = it.optional ("controlpoint", 0),
	    .flags = it.optional ("flags", name == "layerimage" ? 0x10000u
	        : (name == "sphererandom" ? 1u : 0u)),
	    .cone = it.optional ("cone", 0.0f),
	    .delay = it.optional ("delay", 0.0f),
	    .duration = it.optional ("duration", 0.0f),
	    .audioProcessingBounds = parseVec2 ("audioprocessingbounds", glm::vec2 (0.8f, 1.0f)),
	    .audioProcessingExponent = it.optional ("audioprocessingexponent", 2.0f),
	    .audioProcessingFrequencyStart = it.optional ("audioprocessingfrequencystart", 0),
	    .audioProcessingFrequencyEnd = it.optional ("audioprocessingfrequencyend", 1),
	    .audioProcessingMode = it.optional ("audioprocessingmode", 0),
	    .minPeriodicDelay = it.optional ("minperiodicdelay", 1.0f),
	    .maxPeriodicDelay = it.optional ("maxperiodicdelay", 2.0f),
	    .minPeriodicDuration = it.optional ("minperiodicduration", 2.0f),
	    .maxPeriodicDuration = it.optional ("maxperiodicduration", 3.0f),
	    .maxToEmitPerPeriod = it.optional ("maxtoemitperperiod", 0u),
	};
    } catch (const std::exception& e) {
	sLog.error ("Error parsing emitter: ", e.what ());
	sLog.error ("Emitter JSON: ", it.dump ());
	throw;
    }
}

namespace {
uint32_t eventInheritanceMode (const JSON& it, const char* fallback) {
    const auto input = it.optional<std::string> ("input", fallback);
    static constexpr const char* modes[] = {"setcolor", "multiplycolor", "setopacity", "multiplyopacity",
        "setcoloropacity", "multiplycoloropacity", "setvelocity", "addvelocity", "setsize", "multiplysize",
        "setrotation", "addrotation", "setangularvelocity", "addangularvelocity"};
    for (uint32_t i = 0; i < 14; ++i) if (input == modes[i]) return i;
    return 14; // Native unknown selector is a no-op, never another channel.
}
}

ParticleInitializerUniquePtr ObjectParser::parseParticleInitializer (
    const JSON& it, const Properties& properties, bool orthogonalScene, uint32_t nodeFlags) {
    std::string name = it.optional<std::string> ("name", "");

    if (name == "inheritcontrolpointvelocity") {
        auto result = std::make_unique<InheritControlPointVelocityInitializer> ();
        result->controlPoint = it.user ("controlpoint", properties, 0);
        result->min = it.user ("min", properties, 0.1f);
        result->max = it.user ("max", properties, 0.2f);
        return result;
    } else if (name == "inheritinitialvaluefromevent") {
        auto result = std::make_unique<InheritInitialValueFromEventInitializer> ();
        result->mode = eventInheritanceMode (it, "setcolor");
        return result;
    } else if (name == "remapinitialvalue") {
        JSON normalized = it;
        normalized["name"] = "remapvalue";
        if (!it.contains ("input") || it.at ("input").is_null ()) normalized["input"] = "maxlifetime";
        auto remap = parseParticleOperator (normalized, properties, true);
        if (!remap) return nullptr;
        return std::make_unique<RemapInitialValueInitializer> (std::move (remap));
    } else if (name == "hsvcolorrandom") {
        auto result = std::make_unique<HsvColorRandomInitializer> ();
        result->hueMin = it.user ("huemin", properties, 0.0f);
        result->hueMax = it.user ("huemax", properties, 1.0f);
        result->hueSteps = it.user ("huesteps", properties, 6);
        result->saturationMin = it.user ("saturationmin", properties, 0.5f);
        result->saturationMax = it.user ("saturationmax", properties, 1.0f);
        result->valueMin = it.user ("valuemin", properties, 0.5f);
        result->valueMax = it.user ("valuemax", properties, 1.0f);
        return result;
    } else if (name == "colorlist") {
        auto result = std::make_unique<ColorListInitializer> ();
        const auto colors = it.optional ("colors", JSON::array ({"1 1 1"}));
        if (!colors.is_array ()) throw std::invalid_argument ("Particle colorlist colors must be an array");
        for (const auto& color : colors) {
            // Native list entries are normalized RGB vectors, including integer
            // strings such as "1 1 1"; ColorBuilder's byte-color heuristic is
            // inappropriate here.
            if (!color.is_string ()) continue;
            JSON entry = {{"color", color}};
            result->colors.push_back (entry.user ("color", properties, glm::vec3 (1.0f)));
        }
        result->hueNoise = it.user ("huenoise", properties, 0.0f);
        result->saturationNoise = it.user ("saturationnoise", properties, 0.0f);
        result->valueNoise = it.user ("valuenoise", properties, 0.0f);
        return result;
    } else if (name == "positionoffsetrandom") {
        auto result = std::make_unique<PositionOffsetRandomInitializer> ();
        result->scale = it.user ("scale", properties, orthogonalScene ? 0.001f : 1.0f);
        result->distance = it.user ("distance", properties, orthogonalScene ? 100.0f : 0.1f);
        result->timeScale = it.user ("timescale", properties, 1.0f);
        result->directions = it.optional ("directions", orthogonalScene
            ? glm::vec3 (1.0f, 1.0f, 0.0f) : glm::vec3 (1.0f));
        result->sign = it.optional ("sign", glm::vec3 (0.0f));
        result->octaves = it.optional<int> ("octaves", 6);
        return result;
    } else if (name == "mapsequencebetweencontrolpoints") {
        auto result = std::make_unique<MapSequenceBetweenControlPointsInitializer> ();
        result->controlPointStart = it.user ("controlpointstart", properties, 0);
        result->controlPointEnd = it.user ("controlpointend", properties, 1);
        result->count = it.user ("count", properties, 32);
        result->bounds = it.optional ("bounds", glm::vec2 (0.0f, 1.0f));
        result->limitBehavior = it.optional<std::string> ("limitbehavior", "repeat");
        result->flags = it.optional<uint32_t> ("flags", 0);
        if ((result->flags & 16u) != 0 && (nodeFlags & 0x20u) == 0) {
            sLog.error ("Unsupported mapsequencebetweencontrolpoints count patch: initializer flags=16",
                " requires native instance dirty-write timing; node flags=0x20 suppresses this patch");
            return nullptr;
        }
        result->arcAmount = it.optional<float> ("arcamount", 0.3f);
        result->arcDirection = it.optional ("arcdirection", glm::vec3 (0.0f, 1.0f, 0.0f));
        result->sizeReduction = it.optional<float> ("sizereductionamount", 0.9f);
        return result;
    } else if (name == "colorrandom") {
	return std::make_unique<ColorRandomInitializer> (
	    it.color ("min", properties, Builders::ColorBuilder::Black),
	    it.color ("max", properties, Builders::ColorBuilder::White)
	);
    } else if (name == "sizerandom") {
	// 1401b9e70 inserts missing bounds according to the native scene
	// projection context before decoding the initializer. Perspective
	// defaults are 0.001..1; orthographic defaults are 5..50.
	return std::make_unique<SizeRandomInitializer> (
	    it.user ("min", properties, orthogonalScene ? 5.0f : 0.001f),
            it.user ("max", properties, orthogonalScene ? 50.0f : 1.0f),
	    it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "alpharandom") {
	return std::make_unique<AlphaRandomInitializer> (
	    it.user ("min", properties, 0.05f), it.user ("max", properties, 1.0f),
	    it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "lifetimerandom") {
	return std::make_unique<LifetimeRandomInitializer> (
	    it.user ("min", properties, 0.0f), it.user ("max", properties, 1.0f)
	);
    } else if (name == "velocityrandom") {
	return std::make_unique<VelocityRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (-32.0f)), it.user ("max", properties, glm::vec3 (32.0f))
	);
    } else if (name == "rotationrandom") {
	return std::make_unique<RotationRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (0.0f)),
	    it.user ("max", properties, glm::vec3 (0.0f, 0.0f, glm::two_pi<float> ()))
	);
    } else if (name == "angularvelocityrandom") {
	return std::make_unique<AngularVelocityRandomInitializer> (
	    it.user ("min", properties, glm::vec3 (0.0f, 0.0f, -5.0f)),
	    it.user ("max", properties, glm::vec3 (0.0f, 0.0f, 5.0f)), it.user ("exponent", properties, 1.0f)
	);
    } else if (name == "turbulentvelocityrandom") {
	return std::make_unique<TurbulentVelocityRandomInitializer> (
	    it.user ("speedmin", properties, 100.0f), it.user ("speedmax", properties, 250.0f),
	    it.user ("scale", properties, 1.0f), it.user ("offset", properties, 0.0f),
	    it.user ("forward", properties, glm::vec3 (0.0f, 1.0f, 0.0f)), it.user ("timescale", properties, 1.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, 0.1f),
	    it.user ("right", properties, glm::vec3 (0.0f, 0.0f, 1.0f)),
	    it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.8f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 2.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 1),
	    !it.contains ("speedmin"), !it.contains ("speedmax")
	);
    } else if (name == "mapsequencearoundcontrolpoint") {
	return std::make_unique<MapSequenceAroundControlPointInitializer> (
	    it.user ("controlpoint", properties, 0), it.user ("count", properties, 1),
	    it.user ("speedmin", properties, glm::vec3 (0.0f)), it.user ("speedmax", properties, glm::vec3 (0.0f)),
	    it.optional ("bounds", glm::vec2 (0.0f, 1.0f)),
	    it.optional ("axis", glm::vec3 (0.0f, 0.0f, 1.0f)),
	    it.optional<std::string> ("limitbehavior", "repeat"),
	    it.optional<uint32_t> ("flags", 0)
	);
    }

    return nullptr;
}

ParticleOperatorUniquePtr ObjectParser::parseParticleOperator (
    const JSON& it, const Properties& properties, bool birth, bool orthogonalScene) {
    std::string name = it.optional<std::string> ("name", "");

    if (name == "inheritvaluefromevent") {
        auto result = std::make_unique<InheritValueFromEventOperator> ();
        result->mode = eventInheritanceMode (it, "setcoloropacity");
        return result;
    } else if (name == "maintaindistancetocontrolpoint") {
        auto result = std::make_unique<MaintainDistanceToControlPointOperator> ();
        result->controlPoint = it.user ("controlpoint", properties, 0);
        result->distance = it.user ("distance", properties, orthogonalScene ? 200.0f : 1.0f);
        result->variableStrength = it.user ("variablestrength", properties, 0.0f);
        return result;
    } else if (name == "maintaindistancebetweencontrolpoints") {
        auto result = std::make_unique<MaintainDistanceBetweenControlPointsOperator> ();
        result->controlPointStart = it.user ("controlpointstart", properties, 0);
        result->controlPointEnd = it.user ("controlpointend", properties, 1);
        return result;
    } else if (name == "reducemovementnearcontrolpoint") {
        auto result = std::make_unique<ReduceMovementNearControlPointOperator> ();
        result->controlPoint = it.user ("controlpoint", properties, 0);
        result->distanceInner = it.user ("distanceinner", properties, orthogonalScene ? 100.0f : 0.5f);
        result->distanceOuter = it.user ("distanceouter", properties, orthogonalScene ? 350.0f : 1.0f);
        result->reductionInner = it.user ("reductioninner", properties, 100.0f);
        result->reductionOuter = it.user ("reductionouter", properties, 0.0f);
        return result;
    } else if (name == "movement") {
	return std::make_unique<MovementOperator> (
	    it.user ("drag", properties, 0.0f), it.user ("gravity", properties, glm::vec3 (0.0f))
	);
    } else if (name == "angularmovement") {
	return std::make_unique<AngularMovementOperator> (
	    it.user ("drag", properties, 0.0f), it.user ("force", properties, glm::vec3 (0.0f))
	);
    } else if (name == "capvelocity") {
	return std::make_unique<CapVelocityOperator> (
	    it.user ("maxspeed", properties, 100.0f), !it.contains ("maxspeed"));
    } else if (name == "remapvalue") {
	// Only the numeric scalar path is represented. Native 1401bfbb0 inserts
	// numeric range defaults; property-driven/vector ranges remain unsupported.
	const auto isDefaultNumber = [&it] (const char* key, float value) {
	    if (!it.contains (key)) return true;
	    const auto& field = it.at (key);
	    return field.is_number () && field.get<float> () == value;
	};
	const auto isNumber = [&it] (const char* key) {
	    return !it.contains (key) || it.at (key).is_number ();
	};
	const auto transform = it.optional<std::string> ("transformfunction", "none");
	if ((transform != "none" && transform != "sine" && transform != "square"
	     && transform != "saw" && transform != "triangle"
	     && transform != "simplexnoise" && transform != "fbmnoise")
	    || !isDefaultNumber ("outputcontrolpoint0", 0.0f)
	    || !isDefaultNumber ("outputcontrolpoint1", 1.0f)
	    || !isNumber ("transforminputscale")
	    || (transform == "fbmnoise" ?
	        (it.contains ("transformoctaves")
	         && !it.at ("transformoctaves").is_number_integer ())
	        : !isDefaultNumber ("transformoctaves", 3.0f))) return nullptr;
	const int transformOctaves = it.optional<int> ("transformoctaves", 3);
	if (transform == "fbmnoise" && (transformOctaves < 0 || transformOctaves > 32))
	    return nullptr;
	const auto output = it.optional<std::string> ("output", "size");
	const auto input = it.optional<std::string> ("input", "lifetimefraction");
	const auto inputComponent = it.optional<std::string> ("inputcomponent", "all");
	const auto operation = it.optional<std::string> ("operation", "multiply");
	const bool controlPointInput = input == "distancetocontrolpoint"
	    || input == "positionbetweentwocontrolpoints"
	    || input == "controlpoint" || input == "deltatocontrolpoint"
	    || input == "directiontocontrolpoint";
	if (controlPointInput) {
	    if (it.contains ("inputcontrolpoint0")
	        && !it.at ("inputcontrolpoint0").is_number_integer ()) return nullptr;
	} else if (!isDefaultNumber ("inputcontrolpoint0", 0.0f)) return nullptr;
	const int authoredControlPoint = it.optional<int> ("inputcontrolpoint0", 0);
	const int inputControlPoint = static_cast<int> (std::min (
	    static_cast<uint32_t> (authoredControlPoint), 7u));
	if (input == "positionbetweentwocontrolpoints") {
	    if (it.contains ("inputcontrolpoint1")
	        && !it.at ("inputcontrolpoint1").is_number_integer ()) return nullptr;
	} else if (!isDefaultNumber ("inputcontrolpoint1", 1.0f)) return nullptr;
	const int authoredControlPoint1 = it.optional<int> ("inputcontrolpoint1", 1);
	const int inputControlPoint1 = static_cast<int> (std::min (
	    static_cast<uint32_t> (authoredControlPoint1), 7u));
	const auto outputComponent = it.optional<std::string> ("outputcomponent", "all");
	const bool vectorOutput = output == "color" || output == "position" || output == "velocity";
	if (it.contains ("flags") && !it.at ("flags").is_number_integer ()) return nullptr;
	const int flags = it.optional<int> ("flags", 1);
	const bool birthScalarOutput = birth && (output == "maxlifetime"
            || output == "rotation" || output == "angularspeed");
	if (output != "size" && output != "opacity" && output != "speed" && !birthScalarOutput
	    && !vectorOutput) return nullptr;
	if (vectorOutput) {
	    if (outputComponent != "all" && outputComponent != "x"
	        && outputComponent != "y" && outputComponent != "z") return nullptr;
	} else if (outputComponent != "all" || !isNumber ("inputrangemin")
	    || !isNumber ("inputrangemax") || !isNumber ("outputrangemin")
	    || !isNumber ("outputrangemax")) return nullptr;
	if (input != "lifetimefraction" && input != "maxlifetime"
	    && input != "size" && input != "opacity" && input != "speed"
	    && input != "rotation" && input != "angularspeed"
	    && !controlPointInput
	    && input != "color" && input != "position" && input != "velocity") return nullptr;
	const bool vectorInput = input == "color" || input == "position" || input == "velocity"
	    || input == "controlpoint" || input == "deltatocontrolpoint"
	    || input == "directiontocontrolpoint";
	if (vectorInput) {
	    if (inputComponent != "all" && inputComponent != "x"
	        && inputComponent != "y" && inputComponent != "z"
	        && inputComponent != "sum" && inputComponent != "average"
	        && inputComponent != "max" && inputComponent != "min") return nullptr;
	} else if (inputComponent != "all") return nullptr;
	if (operation != "remap" && operation != "multiply"
	    && operation != "add" && operation != "subtract") return nullptr;
	if (flags < 0 || flags > 3) return nullptr;
	const float transformScale = it.optional<float> ("transforminputscale", 2.0f);
	if (vectorOutput) {
	    const auto vectorRange = [&] (const char* key, float defaultValue) -> std::optional<glm::vec3> {
	        if (!it.contains (key)) return glm::vec3 (defaultValue);
	        const auto& field = it.at (key);
	        if (field.is_number ()) return glm::vec3 (field.get<float> ());
	        if (field.is_string ()) return it.optional (key, glm::vec3 (defaultValue));
	        if (field.is_array () && field.size () == 3
	            && field[0].is_number () && field[1].is_number () && field[2].is_number ())
	            return glm::vec3 (field[0].get<float> (), field[1].get<float> (), field[2].get<float> ());
	        return std::nullopt;
	    };
	    const auto inputMin = vectorRange ("inputrangemin", 0.0f);
	    const auto inputMax = vectorRange ("inputrangemax", 1.0f);
	    const auto outputMin = vectorRange ("outputrangemin", 0.0f);
	    const auto outputMax = vectorRange ("outputrangemax", 1.0f);
	    if (!inputMin || !inputMax || !outputMin || !outputMax) return nullptr;
	    return std::make_unique<VectorRemapValueOperator> (
	        input == "lifetimefraction" ? VectorRemapValueOperator::Input::LifetimeFraction
	        : input == "maxlifetime" ? VectorRemapValueOperator::Input::MaxLifetime
	        : input == "size" ? VectorRemapValueOperator::Input::Size
	        : input == "opacity" ? VectorRemapValueOperator::Input::Opacity
	        : input == "speed" ? VectorRemapValueOperator::Input::Speed
	        : input == "rotation" ? VectorRemapValueOperator::Input::Rotation
	        : input == "angularspeed" ? VectorRemapValueOperator::Input::AngularSpeed
	        : input == "distancetocontrolpoint" ? VectorRemapValueOperator::Input::DistanceToControlPoint
	        : input == "positionbetweentwocontrolpoints" ? VectorRemapValueOperator::Input::PositionBetweenTwoControlPoints
	        : input == "controlpoint" ? VectorRemapValueOperator::Input::ControlPoint
	        : input == "deltatocontrolpoint" ? VectorRemapValueOperator::Input::DeltaToControlPoint
	        : input == "directiontocontrolpoint" ? VectorRemapValueOperator::Input::DirectionToControlPoint
	        : input == "color" ? VectorRemapValueOperator::Input::Color
	        : input == "position" ? VectorRemapValueOperator::Input::Position
	                               : VectorRemapValueOperator::Input::Velocity,
	        inputComponent == "x" ? VectorRemapValueOperator::InputComponent::X
	        : inputComponent == "y" ? VectorRemapValueOperator::InputComponent::Y
	        : inputComponent == "z" ? VectorRemapValueOperator::InputComponent::Z
	        : inputComponent == "sum" ? VectorRemapValueOperator::InputComponent::Sum
	        : inputComponent == "average" ? VectorRemapValueOperator::InputComponent::Average
	        : inputComponent == "max" ? VectorRemapValueOperator::InputComponent::Max
	        : inputComponent == "min" ? VectorRemapValueOperator::InputComponent::Min
	                                  : VectorRemapValueOperator::InputComponent::All,
	        output == "color" ? VectorRemapValueOperator::Output::Color
	        : output == "position" ? VectorRemapValueOperator::Output::Position
	                               : VectorRemapValueOperator::Output::Velocity,
	        outputComponent == "x" ? VectorRemapValueOperator::OutputComponent::X
	        : outputComponent == "y" ? VectorRemapValueOperator::OutputComponent::Y
	        : outputComponent == "z" ? VectorRemapValueOperator::OutputComponent::Z
	                                   : VectorRemapValueOperator::OutputComponent::All,
	        operation == "remap" ? VectorRemapValueOperator::Operation::Set
	        : operation == "multiply" ? VectorRemapValueOperator::Operation::Multiply
	        : operation == "add" ? VectorRemapValueOperator::Operation::Add
	                              : VectorRemapValueOperator::Operation::Subtract,
	        flags, *inputMin, *inputMax, *outputMin, *outputMax,
	        transform == "sine" ? VectorRemapValueOperator::Transform::Sine
	        : transform == "square" ? VectorRemapValueOperator::Transform::Square
	        : transform == "saw" ? VectorRemapValueOperator::Transform::Saw
	        : transform == "triangle" ? VectorRemapValueOperator::Transform::Triangle
	        : transform == "simplexnoise" ? VectorRemapValueOperator::Transform::SimplexNoise
	        : transform == "fbmnoise" ? VectorRemapValueOperator::Transform::FBMNoise
	                            : VectorRemapValueOperator::Transform::Identity,
	        transformScale, inputControlPoint, transformOctaves, inputControlPoint1);
	}
	return std::make_unique<ScalarRemapValueOperator> (
	    input == "lifetimefraction" ? ScalarRemapValueOperator::Input::LifetimeFraction
	    : input == "maxlifetime" ? ScalarRemapValueOperator::Input::MaxLifetime
	    : input == "size" ? ScalarRemapValueOperator::Input::Size
	    : input == "opacity" ? ScalarRemapValueOperator::Input::Opacity
	    : input == "speed" ? ScalarRemapValueOperator::Input::Speed
	    : input == "rotation" ? ScalarRemapValueOperator::Input::Rotation
	    : input == "angularspeed" ? ScalarRemapValueOperator::Input::AngularSpeed
	    : input == "distancetocontrolpoint" ? ScalarRemapValueOperator::Input::DistanceToControlPoint
	    : input == "positionbetweentwocontrolpoints" ? ScalarRemapValueOperator::Input::PositionBetweenTwoControlPoints
	    : input == "controlpoint" ? ScalarRemapValueOperator::Input::ControlPoint
	    : input == "deltatocontrolpoint" ? ScalarRemapValueOperator::Input::DeltaToControlPoint
	    : input == "directiontocontrolpoint" ? ScalarRemapValueOperator::Input::DirectionToControlPoint
	    : input == "color" ? ScalarRemapValueOperator::Input::Color
	    : input == "position" ? ScalarRemapValueOperator::Input::Position
	                            : ScalarRemapValueOperator::Input::Velocity,
	    inputComponent == "x" ? ScalarRemapValueOperator::InputComponent::X
	    : inputComponent == "y" ? ScalarRemapValueOperator::InputComponent::Y
	    : inputComponent == "z" ? ScalarRemapValueOperator::InputComponent::Z
	    : inputComponent == "sum" ? ScalarRemapValueOperator::InputComponent::Sum
	    : inputComponent == "average" ? ScalarRemapValueOperator::InputComponent::Average
	    : inputComponent == "max" ? ScalarRemapValueOperator::InputComponent::Max
	    : inputComponent == "min" ? ScalarRemapValueOperator::InputComponent::Min
	                               : ScalarRemapValueOperator::InputComponent::All,
	    output == "size" ? ScalarRemapValueOperator::Output::Size
	    : output == "speed" ? ScalarRemapValueOperator::Output::Speed
	    : output == "maxlifetime" ? ScalarRemapValueOperator::Output::MaxLifetime
	    : output == "rotation" ? ScalarRemapValueOperator::Output::Rotation
	    : output == "angularspeed" ? ScalarRemapValueOperator::Output::AngularSpeed
	                         : ScalarRemapValueOperator::Output::Opacity,
	    operation == "remap" ? ScalarRemapValueOperator::Operation::Set
	    : operation == "multiply" ? ScalarRemapValueOperator::Operation::Multiply
	    : operation == "add" ? ScalarRemapValueOperator::Operation::Add
	                           : ScalarRemapValueOperator::Operation::Subtract,
	    flags, it.optional<float> ("inputrangemin", 0.0f),
	    it.optional<float> ("inputrangemax", 1.0f),
	    it.optional<float> ("outputrangemin", 0.0f),
	    it.optional<float> ("outputrangemax", 1.0f),
	    transform == "sine" ? ScalarRemapValueOperator::Transform::Sine
	    : transform == "square" ? ScalarRemapValueOperator::Transform::Square
	    : transform == "saw" ? ScalarRemapValueOperator::Transform::Saw
	    : transform == "triangle" ? ScalarRemapValueOperator::Transform::Triangle
	    : transform == "simplexnoise" ? ScalarRemapValueOperator::Transform::SimplexNoise
	    : transform == "fbmnoise" ? ScalarRemapValueOperator::Transform::FBMNoise
	                        : ScalarRemapValueOperator::Transform::Identity,
	    transformScale, inputControlPoint, transformOctaves, inputControlPoint1);
    } else if (name == "alphafade") {
	return std::make_unique<AlphaFadeOperator> (
	    it.user ("fadeintime", properties, 0.5f), it.user ("fadeouttime", properties, 0.5f)
	);
    } else if (name == "sizechange") {
	return std::make_unique<SizeChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, 1.0f), it.user ("endvalue", properties, 0.0f)
	);
    } else if (name == "alphachange") {
	return std::make_unique<AlphaChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, 1.0f), it.user ("endvalue", properties, 0.0f)
	);
    } else if (name == "colorchange") {
	return std::make_unique<ColorChangeOperator> (
	    it.user ("starttime", properties, 0.0f), it.user ("endtime", properties, 1.0f),
	    it.user ("startvalue", properties, glm::vec3 (1.0f)), it.user ("endvalue", properties, glm::vec3 (1.0f))
	);
    } else if (name == "turbulence") {
	return std::make_unique<TurbulenceOperator> (
	    it.user ("scale", properties, 0.005f), it.user ("speedmin", properties, 500.0f),
	    it.user ("speedmax", properties, 1000.0f), it.user ("timescale", properties, 0.01f),
	    it.user ("mask", properties, glm::vec3 (1.0f, 1.0f, 0.0f)), it.user ("phasemin", properties, 0.0f),
	    it.user ("phasemax", properties, 0.0f), it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.0f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 1.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 15)
	);
    } else if (name == "vortex" || name == "vortex_v2") {
	const bool v2 = name == "vortex_v2";
	return std::make_unique<VortexOperator> (
	    v2 ? VortexOperator::Variant::VortexV2 : VortexOperator::Variant::Vortex,
	    VortexOperator::SceneDefaults {
		it.find ("distanceinner") == it.end (), it.find ("distanceouter") == it.end (),
		it.find ("speedinner") == it.end ()
	    },
	    it.optional ("controlpoint", 0),
	    it.optional ("flags", 0), // 1 = infinite axis, 2 = maintain distance, 4 = ring shape
	    it.user ("axis", properties, glm::vec3 (1.0f, 0.0f, 0.0f)),
	    it.user ("offset", properties, glm::vec3 (0.0f)), it.user ("distanceinner", properties, 500.0f),
	    it.user ("distanceouter", properties, 650.0f), it.user ("speedinner", properties, 2500.0f),
	    it.user ("speedouter", properties, 0.0f), it.user ("centerforce", properties, 1.0f),
	    it.user ("ringradius", properties, 300.0f), it.user ("ringwidth", properties, 50.0f),
	    it.user ("ringpulldistance", properties, 50.0f), it.user ("ringpullforce", properties, 10.0f),
	    it.user ("audioprocessingmode", properties, 0),
	    it.user ("audioprocessingbounds", properties, glm::vec2 (0.8f, 1.0f)),
	    it.user ("audioprocessingexponent", properties, 2.0f),
	    it.user ("audioprocessingfrequencystart", properties, 0),
	    it.user ("audioprocessingfrequencyend", properties, 1)
	);
    } else if (name == "controlpointattract") {
	return std::make_unique<ControlPointAttractOperator> (
	    it.optional ("controlpoint", 0),
	    it.user (it.contains ("offset") ? "offset" : "origin", properties, glm::vec3 (0.0f)),
	    it.user ("scale", properties, 100.0f), it.user ("threshold", properties, 1000.0f),
	    it.optional<uint32_t> ("flags", 2u)
	);
    } else if (name == "oscillatealpha") {
	return std::make_unique<OscillateAlphaOperator> (
	    it.user ("frequencymin", properties, 1.0f), it.user ("frequencymax", properties, 10.0f),
	    it.user ("scalemin", properties, 0.0f), it.user ("scalemax", properties, 1.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ())
	);
    } else if (name == "oscillatesize") {
	return std::make_unique<OscillateSizeOperator> (
	    it.user ("frequencymin", properties, 1.0f), it.user ("frequencymax", properties, 10.0f),
	    it.user ("scalemin", properties, 0.8f), it.user ("scalemax", properties, 1.2f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ())
	);
    } else if (name == "oscillateposition") {
	return std::make_unique<OscillatePositionOperator> (
	    it.user ("frequencymin", properties, 0.0f), it.user ("frequencymax", properties, 5.0f),
	    it.user ("scalemin", properties, 0.0f), it.user ("scalemax", properties, 10.0f),
	    it.user ("phasemin", properties, 0.0f), it.user ("phasemax", properties, glm::two_pi<float> ()),
	    it.user ("mask", properties, glm::vec3 (1.0f, 1.0f, 0.0f))
	);
    }

    return nullptr;
}

ParticleRenderer ObjectParser::parseParticleRenderer (const JSON& it) {
    std::string name = "sprite";
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    // Renderer-type-specific defaults
    float subdivisionDefault = (name == "rope") ? 4.0f : 1.0f;
    float lengthDefault = (name == "ropetrail") ? 1.0f : 0.05f;

    return ParticleRenderer {
	.name = name,
	.orientation = it.optional ("orientation", std::string ("screen")),
	.axis = it.optional ("axis", glm::vec3 (0.0f)),
	.flags = static_cast<uint8_t> (it.optional ("flags", 0)),
	.length = it.optional ("length", lengthDefault),
	.maxLength = it.optional ("maxlength", 10.0f),
	.minLength = it.optional ("minlength", 0.0f),
	.subdivision = it.optional ("subdivision", subdivisionDefault),
	.segments = it.optional ("segments", 4.0f),
	.uvScale = it.optional ("uvscale", 1.0f),
	.uvScrolling = it.optional ("uvscrolling", false),
	.uvSmoothing = it.optional ("uvsmoothing", true),
	.fadeAlpha = it.optional ("fadealpha", false),
	.fadeSize = it.optional ("fadesize", false),
    };
}

ParticleControlPoint ObjectParser::parseParticleControlPoint (const JSON& it, int index) {
    auto parseVec3 = [&it] (const char* fieldName) {
        glm::vec3 value (0.0f);
        const auto field = it.find (fieldName);
        if (field == it.end ()) return value;
        if (field->is_string ()) {
            std::istringstream input (field->get<std::string> ());
            input >> value.x >> value.y >> value.z;
        } else {
            try { value = it.optional (fieldName, value); }
            catch (...) { value = glm::vec3 (0.0f); }
        }
        return value;
    };

    return ParticleControlPoint {
	.id = index,
	.flags = it.optional ("flags", 0u),
	.parentControlPoint = it.optional ("parentcontrolpoint", 0),
	.offset = parseVec3 ("offset"),
	.angles = parseVec3 ("angles"),
	.lockToPointer = it.optional ("locktopointer", false),
    };
}

ParticleChild ObjectParser::parseParticleChild (const JSON& it, const Project& project) {
    // Native 1401c5490 reads the child asset from `name` (DAT_1404748b8).
    // Retain `particle` as an explicit Linux fixture override, including an
    // intentionally empty value; Workshop child descriptors use `name` alone.
    std::string particleFile = "";
    const auto particleIt = it.find ("particle");
    if (particleIt != it.end () && particleIt->is_string ()) {
	particleFile = particleIt->get<std::string> ();
    } else if (particleIt == it.end ()) {
        const auto nameIt = it.find ("name");
        if (nameIt != it.end () && nameIt->is_string ())
            particleFile = nameIt->get<std::string> ();
    }

    std::string type = "static";
    const auto typeIt = it.find ("type");
    if (typeIt != it.end () && typeIt->is_string ()) {
	type = typeIt->get<std::string> ();
    }

    std::string name = "";
    const auto nameIt = it.find ("name");
    if (nameIt != it.end () && nameIt->is_string ()) {
	name = nameIt->get<std::string> ();
    }

    // Helper lambda to parse vec3 fields that might be strings, arrays, single numbers, or missing
    auto parseVec3 = [&] (const char* fieldName, const glm::vec3& defaultValue) -> glm::vec3 {
	const auto fieldIt = it.find (fieldName);
	if (fieldIt == it.end ()) {
	    return defaultValue;
	}
	if (fieldIt->is_string ()) {
	    return it.optional (fieldName, defaultValue);
	}
	if (fieldIt->is_number ()) {
	    // Single number - use for all components
	    float val = fieldIt->get<float> ();
	    return glm::vec3 (val, val, val);
	}
	if (fieldIt->is_array () && fieldIt->size () >= 3) {
	    return glm::vec3 ((*fieldIt)[0].get<float> (), (*fieldIt)[1].get<float> (), (*fieldIt)[2].get<float> ());
	}
	return defaultValue;
    };

    return ParticleChild {
	.type = type,
	.name = name,
	.flags = it.optional ("flags", 0u),
	.maxCount = it.optional ("maxcount", 20),
	.controlPointStartIndex = it.optional ("controlpointstartindex", 0),
	.probability = it.optional ("probability", 1.0f),
	.angles = parseVec3 ("angles", glm::vec3 (0.0f)),
	.origin = parseVec3 ("origin", glm::vec3 (0.0f)),
	.scale = parseVec3 ("scale", glm::vec3 (1.0f)),
	.particleFile = particleFile,
    };
}

ParticleInstanceOverride ObjectParser::parseParticleInstanceOverride (const JSON& it, const Properties& properties) {
    // Native 14022af30 rewrites an authored 0..255 `color` string into the
    // normalized `colorn` context field before parsing the instance registry.
    // It wins over a simultaneous raw `colorn` entry. With neither field,
    // 14024d760 leaves the negative tint sentinel in the context.
    auto compiledColorn = it.user ("colorn", properties, glm::vec3 (-1.0f));
    const auto authoredColor = it.optional ("color");
    if (authoredColor && authoredColor->is_string ()) {
        compiledColorn = Builders::UserSettingBuilder::fromValue (
            Builders::VectorBuilder::parse<glm::vec3> (
                authoredColor->get<std::string> ()) / 255.0f);
    }
    ParticleInstanceOverride result {
	.enabled = it.user ("enabled", properties, true),
	.alpha = it.user ("alpha", properties, 1.0f),
	.brightness = it.user ("brightness", properties, 1.0f),
	.size = it.user ("size", properties, 1.0f),
	.lifetime = it.user ("lifetime", properties, 1.0f),
	.rate = it.user ("rate", properties, 1.0f),
	.speed = it.user ("speed", properties, 1.0f),
	.count = it.user ("count", properties, 1.0f),
	.color = it.user ("color", properties, glm::vec3 (1.0f)),
	.colorn = std::move (compiledColorn),
    };
    for (size_t index = 0; index < result.controlPoints.size (); ++index) {
        const std::string key = "controlpoint" + std::to_string (index);
        if (it.contains (key))
            result.controlPoints[index] = it.user (key, properties, glm::vec3 (0.0f));
        const std::string angleKey = "controlpointangle" + std::to_string (index);
        if (it.contains (angleKey))
            result.controlPointAngles[index] = it.user (angleKey, properties, glm::vec3 (0.0f));
    }
    return result;
}
