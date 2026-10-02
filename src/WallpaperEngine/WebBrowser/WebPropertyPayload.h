#pragma once

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <set>

namespace WallpaperEngine::WebBrowser {
inline std::string webPropertyPayload (const Data::Model::Project& project) {
    using Data::Model::DynamicValue;
    nlohmann::json result = nlohmann::json::object ();
    for (const auto& [name, property] : project.properties) {
        nlohmann::json value;
        switch (property->getType ()) {
            case DynamicValue::Boolean: value = property->getBool (); break;
            case DynamicValue::Float: value = property->getFloat (); break;
            case DynamicValue::Int: value = property->getInt (); break;
            case DynamicValue::Null: value = nullptr; break;
            case DynamicValue::Vec4: {
                const auto& color = property->getVec4 ();
                std::ostringstream text;
                text << color.x << ' ' << color.y << ' ' << color.z;
                value = text.str ();
                break;
            }
            default: value = property->toString (); break;
        }
        if (const auto text = dynamic_cast<const Data::Model::PropertyTextInput*> (property.get ());
            text && text->hasSerializedInitialText ()) {
            // Authored text inputs are stored internally as serialized JSON;
            // the web listener receives the text itself. Literal user/script
            // edits retain their quotes even when they resemble JSON strings.
            const auto authored = nlohmann::json::parse (property->toString (), nullptr, false);
            if (authored.is_string ()) value = authored.get<std::string> ();
        }
        if (dynamic_cast<const Data::Model::PropertyFile*> (property.get ())
            && property->getType () == DynamicValue::String && !property->getString ().empty ()
            && project.assetLocator) {
            const std::filesystem::path path = property->getString ();
            if (path.is_relative ()) {
                // Native web file settings are physical paths. Resolve through
                // the same overlay as page assets so child preset files win.
                try { value = project.assetLocator->physicalPath (path).string (); }
                catch (const Assets::AssetLoadException&) {
                    // Authored file settings can reference unavailable external
                    // files; the page must still receive its other properties.
                }
            }
        }
        if (const auto numeric = project.numericComboProperties.find (name);
            numeric != project.numericComboProperties.end () && numeric->second) {
            // PropertyCombo retains textual option keys internally, whereas
            // native web settings preserve numeric authored option values.
            value = nlohmann::json::parse (property->toString ());
        }
        result[name] = {{"value", std::move (value)}};
    }
    return result.dump ();
}

inline std::set<std::filesystem::path> webFilePropertyPaths (
    const Data::Model::Project& project, const std::string& propertyPayload) {
    const auto payload = nlohmann::json::parse (propertyPayload);
    std::set<std::filesystem::path> result;
    for (const auto& [name, property] : project.properties) {
        if (!dynamic_cast<const Data::Model::PropertyFile*> (property.get ())) continue;
        const auto& value = payload.at (name).at ("value");
        if (!value.is_string ()) continue;
        const std::filesystem::path path = value.get<std::string> ();
        if (!path.is_absolute ()) continue;
        std::error_code error;
        if (!std::filesystem::is_regular_file (path, error)) continue;
        const auto canonical = std::filesystem::canonical (path, error);
        if (!error) result.insert (canonical);
    }
    return result;
}
} // namespace WallpaperEngine::WebBrowser
