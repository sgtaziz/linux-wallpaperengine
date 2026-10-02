#include <algorithm>
#include <set>

#include "ProjectParser.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperParser.h"

#include "PropertyParser.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"

using namespace WallpaperEngine::Data::Parsers;

static int backgroundId = 0;

namespace {
std::optional<std::string> projectMetadata (const std::filesystem::path& path) {
    WallpaperEngine::FileSystem::Container container;
    container.mount (path, "/");
    for (const auto* package : {"scene.pkg", "gifscene.pkg"}) {
        try { container.mount (path / package, "/"); }
        catch (const std::runtime_error&) { }
    }
    try { return container.readString ("project.json"); }
    catch (const std::filesystem::filesystem_error&) { return std::nullopt; }
}
}

ProjectParser::ResolvedProject ProjectParser::resolve (
    const std::filesystem::path& directory, const DependencyLookup& lookup) {
    std::set<std::filesystem::path> visiting;
    std::function<ResolvedProject (const std::filesystem::path&)> load;
    load = [&] (const std::filesystem::path& path) -> ResolvedProject {
        const auto canonical = std::filesystem::weakly_canonical (path);
        if (!visiting.insert (canonical).second)
            throw std::runtime_error ("Project dependency cycle at " + canonical.string ());
        if (visiting.size () > 64)
            throw std::runtime_error ("Project dependency chain exceeds 64 projects");
        const auto filename = canonical / "project.json";
        const auto contents = projectMetadata (canonical);
        if (!contents) throw std::runtime_error ("Cannot read project metadata: " + filename.string ());
        auto data = WallpaperEngine::Data::JSON::parseAuthoringJson (*contents, filename.string ());
        if (!data.is_object ()) throw std::runtime_error ("Project metadata must be an object: " + filename.string ());
        ResolvedProject result {data, {canonical}};
        const auto dependency = data.optional ("dependency");
        const auto file = data.optional ("file");
        const auto preset = data.optional ("preset");
        const bool needsBase = !file || !file->is_string () || file->get<std::string> ().empty ();
        if (needsBase && preset && preset->is_object () && dependency) {
            if (!dependency->is_string () || dependency->get<std::string> ().empty ())
                throw std::runtime_error ("Project dependency must be a Workshop ID: " + filename.string ());
            const auto id = dependency->get<std::string> ();
            if (!std::ranges::all_of (id, [] (unsigned char c) { return c >= '0' && c <= '9'; }))
                throw std::runtime_error ("Invalid project dependency Workshop ID: " + id);
            auto basePath = canonical.parent_path () / id;
            if ((!std::filesystem::is_directory (basePath) || !projectMetadata (basePath)) && lookup)
                basePath = lookup (id);
            if (basePath.empty () || !std::filesystem::is_directory (basePath) || !projectMetadata (basePath))
                throw std::runtime_error ("Missing wallpaper dependency " + id + " required by "
                    + canonical.string () + "; install that Workshop wallpaper first");
            auto base = load (basePath);
            result.assetDirectories.insert (result.assetDirectories.end (),
                base.assetDirectories.begin (), base.assetDirectories.end ());
            // Presets keep their own identity and files while using the base's
            // renderer, entry point and property definitions.
            auto merged = base.data;
            if (!data.contains ("workshopid")) merged.erase ("workshopid");
            for (const auto& entry : data.items ()) {
                // The dependency owns the renderer, entry point and complete
                // general metadata; native copies its general wholesale.
                if (entry.key () == "file" || entry.key () == "type" || entry.key () == "general") continue;
                merged[entry.key ()] = entry.value ();
            }
            result.data = std::move (merged);
        }
        if (needsBase && preset && preset->is_object () && dependency) {
            auto general = result.data.optional ("general");
            if (general && general->is_object ()) {
                auto properties = general->optional ("properties");
                if (properties && properties->is_object ()) {
                    for (const auto& entry : preset->items ()) {
                        if (properties->contains (entry.key ()) && (*properties)[entry.key ()].is_object ())
                            (*properties)[entry.key ()]["value"] = entry.value ();
                    }
                    (*general)["properties"] = *properties;
                    result.data["general"] = *general;
                }
            }
        }
        visiting.erase (canonical);
        return result;
    };
    return load (directory);
}

ProjectUniquePtr ProjectParser::parse (const JSON& data, AssetLocatorUniquePtr container) {
    const auto general = data.optional ("general");
    const auto workshopId = data.optional ("workshopid");
    auto actualWorkshopId = std::to_string (--backgroundId);
    const auto projectType = data.optional ("type");
    if (!projectType || !projectType->is_string ())
        throw std::runtime_error ("Project type missing or invalid; a preset needs an installed wallpaper dependency");
    auto type = projectType->get<std::string> ();

    if (workshopId.has_value ()) {
	if (workshopId->is_number ()) {
	    actualWorkshopId = std::to_string (workshopId->get<int> ());
	} else if (workshopId->is_string ()) {
	    actualWorkshopId = workshopId->get<std::string> ();
	} else {
	    sLog.error ("Invalid workshop id: ", workshopId->dump ());
	}
    }

    // lowercase for consistency
    std::ranges::transform (type, type.begin (), tolower);

    auto result = std::make_unique<Project> (Project {
	.title = data.require<std::string> ("title", "Project title missing"),
	.type = parseType (type),
	.workshopId = actualWorkshopId,
	.supportsAudioProcessing = general.has_value () && general.value ().optional ("supportsaudioprocessing", false),
	.properties = parseProperties (general),
	.assetLocator = std::move (container),
    });
    if (general && general->is_object ()) {
        if (const auto definitions = general->optional ("properties"); definitions && definitions->is_object ()) {
            for (const auto& entry : definitions->items ()) {
                if (entry.value ().is_object () && entry.value ().optional ("type") == "combo")
                    result->numericComboProperties[entry.key ()] = entry.value ().contains ("value")
                        && entry.value ().at ("value").is_number ();
            }
        }
    }

    const auto file = data.optional ("file");
    if (!file || !file->is_string () || file->get<std::string> ().empty ())
        throw std::runtime_error ("Project main file missing or empty; cannot load wallpaper entry point");
    result->wallpaper = WallpaperParser::parse (*file, *result);

    return result;
}

Project::Type ProjectParser::parseType (const std::string& type) {
    if (type == "scene") {
	return Project::Type_Scene;
    }

    if (type == "video") {
	return Project::Type_Video;
    }

    if (type == "web") {
	return Project::Type_Web;
    }

    sLog.exception ("Unsupported project type ", type);
}

Properties ProjectParser::parseProperties (const std::optional<JSON>& data) {
    if (!data.has_value ()) {
	return {};
    }

    const auto properties = data.value ().optional ("properties");

    if (!properties.has_value ()) {
	return {};
    }

    Properties result = {};

    for (const auto& cur : properties.value ().items ()) {
	const auto& property = PropertyParser::parse (cur.value (), cur.key ());

	// ignore properties that failed, these are generally groups
	if (property == nullptr) {
	    continue;
	}

	result.emplace (cur.key (), property);
    }

    return result;
}
