#include "DynamicValueParser.h"

#include "UserSettingParser.h"
#include "WallpaperEngine/Data/Model/DynamicValue.h"

#include <cmath>
#include <regex>
#include <sstream>

using namespace WallpaperEngine::Data::Parsers;

namespace {
std::unique_ptr<WallpaperEngine::Data::Model::PropertyAnimation> parseAnimation (const json& source) {
    using Animation = WallpaperEngine::Data::Model::PropertyAnimation;
    if (!source.is_object ()) return nullptr;
    const auto options = source.find ("options");
    if (options == source.end () || !options->is_object ()) return nullptr;
    const auto fps = options->find ("fps");
    const auto length = options->find ("length");
    if (fps == options->end () || length == options->end () || !fps->is_number () ||
        !length->is_number ()) return nullptr;
    auto result = std::make_unique<Animation> ();
    result->fps = fps->get<float> ();
    result->length = length->get<int> ();
    if (!(result->fps > 0.0f) || !std::isfinite (result->fps) || result->length <= 0 ||
        !std::isfinite (result->duration ())) return nullptr;
    if (const auto name = options->find ("name"); name != options->end () && name->is_string ())
        result->name = name->get<std::string> ();
    if (const auto mode = options->find ("mode"); mode != options->end () && mode->is_string ()) {
        const auto value = mode->get<std::string> ();
        if (value == "single") result->mode = Animation::Mode::Single;
        else if (value == "mirror") result->mode = Animation::Mode::Mirror;
    }
    if (const auto paused = options->find ("startpaused"); paused != options->end () && paused->is_boolean ())
        result->paused = paused->get<bool> ();
    for (size_t channel = 0; channel < result->channels.size (); ++channel) {
        const auto authored = source.find ("c" + std::to_string (channel));
        if (authored == source.end () || !authored->is_array ()) continue;
        auto& keys = result->channels[channel];
        int previous = -1;
        for (const auto& item : *authored) {
            if (!item.is_object ()) continue;
            const auto frame = item.find ("frame");
            const auto value = item.find ("value");
            if (frame == item.end () || value == item.end () || !frame->is_number () ||
                !value->is_number ()) continue;
            Animation::Key key;
            key.frame = frame->get<int> ();
            key.value = value->get<float> ();
            if (key.frame <= previous || !std::isfinite (key.value)) continue;
            auto parseHandle = [&item] (const char* name, Animation::Handle& output) {
                const auto authoredHandle = item.find (name);
                if (authoredHandle == item.end () || !authoredHandle->is_object ()) return;
                const auto enabled = authoredHandle->find ("enabled");
                if (enabled != authoredHandle->end () && enabled->is_boolean () && !enabled->get<bool> ()) return;
                output.enabled = true;
                const auto x = authoredHandle->find ("x");
                const auto y = authoredHandle->find ("y");
                if (x != authoredHandle->end () && x->is_number ()) output.x = x->get<float> ();
                if (y != authoredHandle->end () && y->is_number ()) output.y = y->get<float> ();
            };
            parseHandle ("back", key.back);
            parseHandle ("front", key.front);
            if (const auto constant = item.find ("step"); constant != item.end () && constant->is_boolean ())
                key.constant = constant->get<bool> ();
            keys.push_back (key);
            previous = key.frame;
        }
    }
    return result;
}

bool isScriptPropertyText (const json& property) {
    const json* authored = &property;
    if (property.is_object ()) {
        const auto value = property.find ("value");
        if (value == property.end ()) return false;
        authored = &*value;
    }
    if (!authored->is_string ()) return false;
    std::istringstream tokens (authored->get<std::string> ());
    std::string token;
    size_t count = 0;
    while (tokens >> token) {
        ++count;
        size_t parsed = 0;
        try {
            const float value = std::stof (token, &parsed);
            if (parsed != token.size () || !std::isfinite (value)) return true;
        } catch (const std::exception&) {
            return true;
        }
    }
    return count == 0 || count > 4;
}

// Combo option values are JavaScript values. In particular, an authored
// option `value: '1'` must remain a string even though its saved JSON value
// looks numeric: scripts can compare it with a string switch case.
bool isStringComboOption (const std::string& script, const std::string& propertyName,
                          const json& property) {
    const json* authored = &property;
    if (property.is_object ()) {
        const auto value = property.find ("value");
        if (value == property.end ()) return false;
        authored = &*value;
    }
    if (!authored->is_string ()) return false;
    static const std::regex namePattern (R"(\bname\s*:\s*(['"])([^'"]+)\1)");
    static const std::regex valuePattern (R"(\bvalue\s*:\s*(['"])([^'"]*)\1)");
    size_t start = 0;
    while ((start = script.find (".addCombo", start)) != std::string::npos) {
        const size_t opening = script.find ('{', start + 9);
        if (opening == std::string::npos) break;
        size_t end = opening;
        int depth = 0;
        char quote = 0;
        for (; end < script.size (); ++end) {
            const char c = script[end];
            if (quote) {
                if (c == '\\') { ++end; continue; }
                if (c == quote) quote = 0;
            } else if (c == '\'' || c == '"') quote = c;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { ++end; break; }
        }
        if (end > script.size ()) end = script.size ();
        const std::string block = script.substr (opening, end - opening);
        std::smatch name;
        if (std::regex_search (block, name, namePattern) && name[2].str () == propertyName) {
            for (std::sregex_iterator it (block.begin (), block.end (), valuePattern), last;
                 it != last; ++it)
                if ((*it)[2].str () == authored->get<std::string> ()) return true;
            return false;
        }
        start = end;
    }
    return false;
}
}

DynamicValueUniquePtr DynamicValueParser::parse (const json& data, const Properties& properties,
                                                bool expectColor, bool expectString, bool floatColor) {
    auto value = std::make_unique<DynamicValue> ();
    auto valueIt = data;
    std::optional<std::string> scriptSource = std::nullopt;
    std::optional<json> scriptPropsJson = std::nullopt;

    if (data.is_object ()) {
	if (const auto animation = data.find ("animation"); animation != data.end ())
	    value->setAnimation (parseAnimation (*animation));
	const auto user = data.optional ("user");
	const auto script = data.optional ("script");
	valueIt = data.require ("value", "User setting must have a value");

	if (script.has_value () && !script->is_null ()) {
	    scriptSource = script->get<std::string> ();
	    scriptPropsJson = data.optional ("scriptproperties");
	}
    }

    // actual value parsing
    if (valueIt.is_string ()) {
	if (expectString) {
	    value->update (valueIt.get<std::string> (), DynamicValue::UpdateSource::Initialization);
	} else if (expectColor) {
	    // Scene image/text RGB is a floating vector even when its string
	    // contains integers; byte/CSS color consumers retain their own encoding.
	    value->update (floatColor ? Builders::ColorBuilder::parseProperty (valueIt)
	                             : Builders::ColorBuilder::parse (valueIt),
	                   DynamicValue::UpdateSource::Initialization);
	} else {
	    std::string str = valueIt;
	    int size = Builders::VectorBuilder::preparseSize (str);

	    if (size == 1) {
		// scalar? text value?
		std::size_t parsed = 0;
		try {
		    float f = std::stof (str, &parsed);

		    if (parsed == str.size ()) {
			value->update (f, DynamicValue::UpdateSource::Initialization);
		    } else {
			value->update (str, DynamicValue::UpdateSource::Initialization);
		    }
		} catch (const std::exception&) {
		    value->update (str, DynamicValue::UpdateSource::Initialization);
		}
	    } else if (size == 2) {
		value->update (static_cast<glm::vec2> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else if (size == 3) {
		value->update (static_cast<glm::vec3> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else {
		value->update (static_cast<glm::vec4> (valueIt), DynamicValue::UpdateSource::Initialization);
	    }
	}
    } else if (valueIt.is_number_integer ()) {
	value->update (valueIt.get<int> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_number_float ()) {
	value->update (valueIt.get<float> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_boolean ()) {
	value->update (valueIt.get<bool> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_null ()) {
	// null value with no connection to property
	value->update (DynamicValue::UpdateSource::Initialization);
    }

    if (scriptSource.has_value ()) {
	std::map<std::string, UserSettingUniquePtr> scriptProps;

	if (scriptPropsJson.has_value () && scriptPropsJson->is_object ()) {
	    for (const auto& [key, propData] : scriptPropsJson->items ()) {
		scriptProps[key] = UserSettingParser::parse (
	                    propData, properties, false,
	                    isScriptPropertyText (propData)
	                        || isStringComboOption (*scriptSource, key, propData));
	    }
	}

	value->setProperties (std::move (scriptProps));
	value->setScriptSource (scriptSource.value ());
    }

    return value;
}
