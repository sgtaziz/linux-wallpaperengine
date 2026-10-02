#include "WallpaperParser.h"

#include "ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

#include <new>
#include <stdexcept>

using namespace WallpaperEngine::Data::Parsers;

namespace {
bool hasRootCameraPaths (const JSON& camera, const Project& project) {
    const auto files = camera.optional ("paths");
    if (!files || !files->is_array () || files->empty () || !files->back ().is_string ()) return false;
    // Native 140186c90 clears the path list before each file, so the final
    // filename controls whether its orthographic root pose is retained.
    try {
        const auto data = WallpaperEngine::Data::JSON::parseAuthoringJson (
            project.assetLocator->readString (files->back ().get<std::string> ()), "camera path");
        const auto paths = data.optional ("paths");
        if (!paths || !paths->is_array ()) return false;
        // 140198e20 appends enabled segments with a nonempty transforms array.
        // This only preserves the root-pose contract; path animation is separate.
        for (const auto& path : *paths) {
            if (!path.is_object ()) break;
            const auto transforms = path.optional ("transforms");
            if (!transforms || !transforms->is_array () || transforms->empty ()) break;
            const auto disabled = path.optional ("disabled");
            if (!disabled || !disabled->is_boolean () || !disabled->get<bool> ()) return true;
        }
    } catch (const std::exception& error) {
        sLog.error ("Cannot inspect root camera paths: ", error.what ());
    }
    return false;
}
} // namespace

WallpaperUniquePtr WallpaperParser::parse (const JSON& file, Project& project) {
    switch (project.type) {
	case Project::Type_Scene:
	    return parseScene (file, project);
	case Project::Type_Video:
	    return parseVideo (file, project);
	case Project::Type_Web:
	    return parseWeb (file, project);
	default:
	    sLog.exception ("Unexpected project type value found... This is likely a bug");
    }
}

SceneUniquePtr WallpaperParser::parseScene (const JSON& file, Project& project) {
    const auto scene = WallpaperEngine::Data::JSON::parseAuthoringJson (
        project.assetLocator->readString (file), file);
    project.sceneVersion = scene.optional ("version", 0);
    const auto camera = scene.require ("camera", "Scenes must have a camera section");
    const auto general = scene.require ("general", "Scenes must have a general section");
    const auto projection = general.optional ("orthogonalprojection");
    const bool hasOrthogonalProjection = projection.has_value () && projection->is_object ();
    const bool autoOrthogonalProjection = hasOrthogonalProjection && projection->optional ("auto", false);
    const int orthogonalWidth = hasOrthogonalProjection ? projection->optional ("width", 0) : 0;
    const int orthogonalHeight = hasOrthogonalProjection ? projection->optional ("height", 0) : 0;
    // 140186c90 propagates the native orthographic context bit when both
    // authored dimensions are nonzero, including signed dimensions.
    const bool useOrthogonalProjection = autoOrthogonalProjection ||
                                         (orthogonalWidth != 0 && orthogonalHeight != 0);
    project.sceneOrthogonalProjection = useOrthogonalProjection;
    const auto objects = scene.require ("objects", "Scenes must have an objects section");
    // Native 140186c90 reads lightconfig from the retained general object,
    // then packs its point count into the renderer lighting configuration.
    const auto lightConfig = general.optional ("lightconfig");
    int pointLightSlots = 0;
    int spotLightSlots = 0;
    if (lightConfig.has_value ()) {
	if (!lightConfig->is_object ()) throw std::invalid_argument ("Scene lightconfig must be an object");
	pointLightSlots = lightConfig->optional ("point", 0);
        spotLightSlots = lightConfig->optional ("spot", 0);
        if (spotLightSlots < 0 || spotLightSlots > 15)
            throw std::invalid_argument ("Scene spot lightconfig exceeds its serialized four-bit count");
	if (pointLightSlots < 0 || pointLightSlots > 15)
	    throw std::invalid_argument ("Scene point lightconfig exceeds its serialized four-bit count");
    }
    const auto& properties = project.properties;
    // Native 19b920 ignores wrong JSON types. Preserve registered bool user
    // settings, while invalid authored values retain the initial false bit.
    auto clearGeneral = general;
    if (const auto value = general.optional ("clearenabled"); value
        && !value->is_boolean ()
        && !(value->is_object () && value->contains ("value") && value->at ("value").is_boolean ()))
        clearGeneral["clearenabled"] = false;

    // Shipped scenes author these projection fields under general. Keep camera
    // as a fallback for older definitions that placed them there.
    const auto cameraSetting = [&] (const char* key, float fallback) {
	const auto& section = general.contains (key) ? general : camera;
	return section.user (key, properties, fallback);
    };

    // TODO: FIND IF THESE DEFAULTS ARE SENSIBLE OR NOT AND PERFORM PROPER VALIDATION WHEN CAMERA PREVIEW AND CAMERA
    // PARALLAX ARE PRESENT

    return std::make_unique <Scene> (
        WallpaperData {
            .filename = "",
            .project = project
        }, SceneData {
            .pointLightSlots = pointLightSlots,
            .spotLightSlots = spotLightSlots,
            .clearEnabled = clearGeneral.user ("clearenabled", properties, false),
            .colors = {
                .ambient  = general.user ("ambientcolor", properties, glm::vec3 (0.0f)),
                .skylight = general.user ("skylightcolor", properties, glm::vec3 (0.0f)),
                .clear = general.user ("clearcolor", properties, glm::vec3 (1.0f)),
            },
            .camera = {
                .fade = general.user ("camerafade", properties, false),
		.hdr = general.optional ("hdr", false),
                .preview = general.optional ("camerapreview", false),
                .bloom = {
                    .enabled = general.user ("bloom", properties, false),
                    .strength = general.user ("bloomstrength", properties, 0.0f),
                    .threshold = general.user ("bloomthreshold", properties, 0.0f),
		    .hdrStrength = general.user ("bloomhdrstrength", properties, 2.0f),
		    .hdrThreshold = general.user ("bloomhdrthreshold", properties, 1.0f),
		    .hdrFeather = general.user ("bloomhdrfeather", properties, 0.1f),
		    .hdrScatter = general.user ("bloomhdrscatter", properties, 1.619f),
		    .hdrIterations = general.user ("bloomhdriterations", properties, 8),
		    .tint = general.user ("bloomtint", properties, glm::vec3 (1.0f)),
                },
                .parallax = {
                    .enabled = general.user ("cameraparallax", properties, false),
                    .amount = general.user ("cameraparallaxamount", properties, 1.0f),
                    .delay = general.user ("cameraparallaxdelay", properties, 0.0f),
                    .mouseInfluence = general.user ("cameraparallaxmouseinfluence", properties, 1.0f),
                },
                .shake = {
                    .enabled = general.user ("camerashake", properties, false),
                    .amplitude = general.user ("camerashakeamplitude", properties, 0.0f),
                    .roughness = general.user ("camerashakeroughness", properties, 0.0f),
                    .speed = general.user ("camerashakespeed", properties, 0.0f),
                },
                .configuration = {
                    .center = camera.require <glm::vec3> ("center", "Camera must have a center position"),
                    .eye = camera.require <glm::vec3> ("eye", "Camera must have an eye position"),
                    .up = camera.require <glm::vec3> ("up", "Camera must have an up position"),
                    .hasPaths = hasRootCameraPaths (camera, project),
                },
                .projection = {
		    .width  = autoOrthogonalProjection ? 0 : orthogonalWidth,
		    .height = autoOrthogonalProjection ? 0 : orthogonalHeight,
		    .isAuto = autoOrthogonalProjection,
		    .isOrthogonal = useOrthogonalProjection,
		    .nearz = cameraSetting ("nearz", useOrthogonalProjection ? 0.0f : 0.1f),
		    .farz = cameraSetting ("farz", useOrthogonalProjection ? 1000.0f : 10000.0f),
	            .fov = cameraSetting ("fov", 50.0f),
	            .zoom = general.user ("zoom", properties, 1.0f),
	            .perspectiveOverrideFov = general.user ("perspectiveoverridefov", properties, 95.0f)
                }
            },
            .objects = parseObjects (objects, project),
        }
    );
}

VideoUniquePtr WallpaperParser::parseVideo (const JSON& file, Project& project) {
    return std::make_unique<Video> (WallpaperData { .filename = file, .project = project });
}

WebUniquePtr WallpaperParser::parseWeb (const JSON& file, Project& project) {
    return std::make_unique<Web> (WallpaperData {
	.filename = file,
	.project = project,
    });
}

ObjectList WallpaperParser::parseObjects (const JSON& objects, const Project& project) {
    if (!objects.is_array ()) throw std::invalid_argument ("Scene objects must be an array");
    ObjectList result = {};

    for (size_t index = 0; index < objects.size (); ++index) {
	try {
	    result.emplace_back (ObjectParser::parse (objects[index], project));
	} catch (const std::bad_alloc&) {
	    throw;
	} catch (const std::exception& e) {
	    sLog.error ("Skipping invalid scene object at objects[", index, "]: ", e.what ());
	}
    }

    return result;
}
