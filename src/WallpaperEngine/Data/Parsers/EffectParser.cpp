#include "EffectParser.h"
#include "MaterialParser.h"

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/FileSystem/Container.h"

#include <limits>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

namespace {
std::optional<uint32_t> fboDimension (const JSON& object, const char* key) {
    const auto raw = object.optional (key);
    if (!raw) return std::nullopt;
    if (!raw->is_number_integer ())
        throw std::invalid_argument (std::string ("Effect FBO ") + key + " must be a positive integer");
    if (!raw->is_number_unsigned () && raw->get<int64_t> () <= 0)
        throw std::invalid_argument (std::string ("Effect FBO ") + key + " must be a positive integer");
    const auto value = raw->get<uint64_t> ();
    if (value == 0 || value > std::numeric_limits<uint16_t>::max ())
        throw std::invalid_argument (std::string ("Effect FBO ") + key + " is out of range");
    return static_cast<uint32_t> (value);
}

std::optional<glm::vec4> fboClear (const JSON& object) {
    const auto raw = object.optional ("clear");
    if (!raw) return std::nullopt;
    if (!raw->is_string ()) throw std::invalid_argument ("Effect FBO clear must be an RGBA string");
    const auto source = raw->get<std::string> ();
    if (source.empty ()) return std::nullopt;
    std::istringstream values (source);
    float components[4] = {};
    int count = 0;
    std::string token;
    while (values >> token) {
        if (count == 4)
            throw std::invalid_argument ("Effect FBO clear must contain one to four numeric components");
        try {
            size_t consumed = 0;
            components[count] = std::stof (token, &consumed);
            if (consumed != token.size () || !std::isfinite (components[count]))
                throw std::invalid_argument ("Effect FBO clear components must be finite numbers");
        } catch (const std::out_of_range&) {
            throw std::invalid_argument ("Effect FBO clear components are out of range");
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument ("Effect FBO clear components must be finite numbers");
        }
        ++count;
    }
    if (count == 0)
        throw std::invalid_argument ("Effect FBO clear must contain one to four numeric components");
    return glm::vec4 (components[0], components[1], components[2], components[3]);
}
}

EffectUniquePtr EffectParser::load (const Project& project, const std::string& filename) {
    const auto effectJson = WallpaperEngine::Data::JSON::parseAuthoringJson (
        project.assetLocator->readString (filename), filename);

    return parse (effectJson, project);
}

EffectUniquePtr EffectParser::parse (const JSON& it, const Project& project) {
    const auto dependencies = it.optional ("dependencies");
    const auto fbos = it.optional ("fbos");
    const auto functions = it.optional ("functions");
    auto parsedFBOs = fbos.has_value () ? parseFBOs (*fbos) : std::vector<FBOUniquePtr> {};
    auto parsedFunctions = functions.has_value ()
        ? parseClearFunctions (*functions, parsedFBOs)
        : std::map<std::string, std::vector<std::string>> {};

    return std::make_unique<Effect> (Effect {
	.name = it.optional<std::string> ("name", ""),
	.description = it.optional<std::string> ("description", ""),
	.group = it.optional<std::string> ("group", ""),
	.preview = it.optional<std::string> ("preview", ""),
	.dependencies = dependencies.has_value () ? parseDependencies (*dependencies) : std::vector<std::string> {},
	.passes = parseEffectPasses (it.require ("passes", "Effect file must have passes"), project),
	.fbos = std::move (parsedFBOs),
	.clearFunctions = std::move (parsedFunctions),
    });
}

std::map<std::string, std::vector<std::string>> EffectParser::parseClearFunctions (
    const JSON& it, const std::vector<FBOUniquePtr>& fbos) {
    std::map<std::string, std::vector<std::string>> result;
    if (!it.is_object ()) throw std::invalid_argument ("Effect functions must be an object");
    for (const auto& [name, function] : it.items ()) {
        if (!function.is_object ()) continue;
        const auto action = function.optional ("action");
        if (!action || !action->is_string () || *action != "clear") continue;
        const auto targets = function.optional ("fbos");
        if (!targets || !targets->is_array ()) continue;
        auto& selected = result[name];
        for (const auto& target : *targets) {
            if (!target.is_string ()) continue;
            const auto literal = target.get<std::string> ();
            if (std::any_of (fbos.begin (), fbos.end (), [&] (const auto& fbo) {
                    return fbo->name == literal;
                })) selected.push_back (literal);
        }
    }
    return result;
}

std::vector<std::string> EffectParser::parseDependencies (const JSON& it) {
    std::vector<std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (cur);
    }

    return result;
}

std::vector<EffectPassUniquePtr> EffectParser::parseEffectPasses (const JSON& it, const Project& project) {
    std::vector<EffectPassUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	const auto binds = cur.optional ("bind");
	const auto command = cur.optional ("command");
	const auto material = cur.optional ("material");
	const auto compose = cur.optional ("compose");
	if (command.has_value ()) {
	    if (!command->is_string ()) throw std::invalid_argument ("Effect pass command must be a string");
	    if (*command != "copy" && *command != "swap")
		throw std::invalid_argument ("Unknown effect pass command: " + command->get<std::string> ());
	}

	// TODO: CAN TARGET BE SET IF MATERIAL IS SET?

	result.push_back (
	    std::make_unique<EffectPass> (EffectPass {
		.material = material.has_value () ? MaterialParser::load (project, *material)
						  : std::optional<MaterialUniquePtr> {},
		.binds = binds.has_value () ? parseBinds (binds.value ()) : std::map<int, std::string> {},
		.command = command.has_value () ? (command.value () == "copy" ? Command_Copy : Command_Swap)
						: std::optional<PassCommandType> {},
		.source = command.has_value ()
		    ? cur.require<std::string> ("source", "Effect command must have a source")
		    : cur.optional<std::string> ("source"),
		.target = command.has_value ()
		    ? cur.require<std::string> ("target", "Effect command must have a target")
		    : cur.optional<std::string> ("target"),
		.compose = compose && compose->is_boolean () && compose->get<bool> (),
	    })
	);
    }

    return result;
}

std::map<int, std::string> EffectParser::parseBinds (const JSON& it) {
    std::map<int, std::string> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.emplace (
	    cur.require ("index", "Texture binds must have an index"),
	    cur.require ("name", "Texture bind must name the FBO that should be used")
	);
    }

    return result;
}

std::vector<FBOUniquePtr> EffectParser::parseFBOs (const JSON& it) {
    std::vector<FBOUniquePtr> result = {};

    if (!it.is_array ()) {
	return result;
    }

    for (const auto& cur : it) {
	result.push_back (
	    std::make_unique<FBO> (FBO {
		.name = cur.require<std::string> ("name", "FBO must have a name"),
		.format = cur.optional<std::string> ("format", "rgba8888"),
		.scale = cur.optional ("scale", 1.0f),
		.unique = cur.optional ("unique", false),
		.width = fboDimension (cur, "width"),
		.height = fboDimension (cur, "height"),
		.fit = fboDimension (cur, "fit"),
		.uvs = cur.optional<std::string> ("uvs"),
		.clear = fboClear (cur),
	    })
	);
    }

    return result;
}
