#include "SpotLightUniforms.h"
#include "LegacyLightUniforms.h"
#include "WallpaperEngine/Render/Objects/CImage.h"
#include "WallpaperEngine/Render/Objects/ImageDimensions.h"
#include "WallpaperEngine/Render/Objects/CModel.h"
#include "WallpaperEngine/Render/Objects/CParticle.h"
#include "WallpaperEngine/Render/Objects/CSound.h"
#include "WallpaperEngine/Render/Objects/CText.h"

#include "WallpaperEngine/Render/WallpaperState.h"

#include "CScene.h"
#include "SceneDependencies.h"
#include "SceneCursor.h"
#include "ParticleSceneClock.h"
#include "SceneTransform.h"
#include "WallpaperEngine/Logging/Log.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <ranges>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

extern float g_Time;
extern float g_TimeLast;

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Render::Wallpapers;

namespace {
class CTransformObject final : public Scripting::ScriptableObject {
public:
    CTransformObject (CScene& scene, const Object& object) :
        CObject (scene, object), ScriptableObject (scene, object) {
        for (const auto& binding : Scripting::scriptPropertyBindings (object))
            registerProperty (binding.name, binding.value);
    }
};

class CSpotLight final : public Scripting::ScriptableObject {
public:
    CSpotLight (CScene& scene, const SceneSpotLight& light) :
        CObject (scene, light), ScriptableObject (scene, light) {
        for (const auto& binding : Scripting::scriptPropertyBindings (light))
            registerProperty (binding.name, binding.value);
        if (light.castShadow || light.useCookie)
            sLog.error ("Spot light shadow/cookie rendering is not supported: object_id=", light.id);
    }
};

class CPointLight final : public Scripting::ScriptableObject {
public:
    CPointLight (CScene& scene, const ScenePointLight& light) :
        CObject (scene, light), ScriptableObject (scene, light) {
        for (const auto& binding : Scripting::scriptPropertyBindings (light))
            registerProperty (binding.name, binding.value);
    }
};
}

CScene::CScene (
    const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
    const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
) : CWallpaper (wallpaper, context, audioContext, scalingMode, clampMode) {
    const auto explicitSeed = context.getApp ().getContext ().settings.render.debug.particleSeed;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds> (
        std::chrono::system_clock::now ().time_since_epoch ()).count ();
    m_particleRandom.seed (explicitSeed.value_or (static_cast<uint32_t> (milliseconds)));
    // caller should check this, if not a std::bad_cast is good to throw
    auto scene = wallpaper.as<Scene> ();

    // Native 14010df40 enables the float HDR route only for authored HDR
    // scenes with bloom when host postprocessing quality is ultra.
    m_hdrPostprocessing = scene->camera.hdr && scene->camera.bloom.enabled->value->getBool ()
        && context.getApp ().getContext ().settings.render.postprocessing
               == Application::ApplicationContext::POSTPROCESSING_ULTRA;
    sLog.debug ("HDR gate authored=", scene->camera.hdr, " bloom=",
                scene->camera.bloom.enabled->value->getBool (), " quality=",
                context.getApp ().getContext ().settings.render.postprocessing,
                " active=", m_hdrPostprocessing);

    const auto dependencyReport = inspectSceneDependencies (scene->objects);
    for (const auto& [id, reason] : dependencyReport.rejected) {
	m_rejectedObjectIds.insert (id);
	sLog.error ("Skipping scene object ", id, ": ", reason);
    }

    // setup scripting engine
    this->m_scriptEngine = std::make_unique<Scripting::ScriptEngine> (*this, context.getMediaSource ());
    // setup the scene camera
    this->m_camera = std::make_unique<Camera> (
        *this, scene->camera, Camera::selectActiveSceneCamera (scene->objects));

    float width = scene->camera.projection.width;
    float height = scene->camera.projection.height;

    // detect size if the orthogonal project is auto
    if (scene->camera.projection.isAuto) {
	glm::vec2 maxExtent = { 0.0f, 0.0f };

	for (const auto& object : scene->objects) {
	    if (!object->is<Image> ()) {
		continue;
	    }

	    const auto* image = object->as<Image> ();
	    if (!image->origin || !image->origin->value) {
		continue;
	    }

	    const glm::vec3 origin = image->origin->value->getVec3 ();
	    const auto authored = Data::JSON::JSON::parse (image->initialConfiguration);
	    const std::optional<glm::vec2> authoredSize = authored.contains ("size")
	        ? std::optional<glm::vec2> (image->size->value->getVec2 ()) : std::nullopt;
	    maxExtent = glm::max (maxExtent, Objects::imageAutoProjectionExtent (
	        glm::vec2 (origin), authoredSize));
	}

	if (maxExtent.x > 0.0f && maxExtent.y > 0.0f) {
	    width = maxExtent.x * 2.0f;
	    height = maxExtent.y * 2.0f;
	} else {
	    width = this->getContext ().getOutput ().getFullWidth ();
	    height = this->getContext ().getOutput ().getFullHeight ();
	    sLog.debug ("Auto projection: falling back to screen resolution ", width, "x", height);
	}
    }

    this->m_parallaxDisplacement = { 0, 0 };

    if (scene->camera.projection.isOrthogonal) {
	this->m_camera->setOrthogonalProjection (width, height);
    } else {
	// Native 140186c90 leaves its orthographic bit clear when general has
	// no valid orthogonalprojection. Perspective uses the output aspect.
	width = this->getContext ().getOutput ().getFullWidth ();
	height = this->getContext ().getOutput ().getFullHeight ();
	this->m_camera->setPerspectiveProjection (width, height);
    }

    // setup framebuffers here as they're required for the scene setup
    const auto sceneFormat = m_hdrPostprocessing ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
    if (m_hdrPostprocessing) this->setBackbufferFormat (sceneFormat);
    this->setupFramebuffers (sceneFormat);
    // Native _rt_FullFrameBuffer uses D16 depth; effect and auxiliary targets
    // request no depth attachment. Keep that lifetime tied to this target.
    this->m_sceneFBO->attachDepth16 ();

    const uint32_t sceneWidth = this->m_camera->getWidth ();
    const uint32_t sceneHeight = this->m_camera->getHeight ();

    this->_rt_shadowAtlas = this->create (
	"_rt_shadowAtlas", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0, { sceneWidth, sceneHeight },
	{ sceneWidth, sceneHeight }
    );
    this->alias ("_alias_lightCookie", "_rt_shadowAtlas");

    // set clear color
    const glm::vec3 clearColor = scene->colors.clear->value->getVec3 ();

    glClearColor (clearColor.r, clearColor.g, clearColor.b, 1.0f);

    // Native LIGHTS_POINT comes from general.lightconfig's four-bit count,
    // not the number of light objects. Keep unused slots zero-filled.
    const int pointLightSlots = scene->pointLightSlots;
    m_pointLightColors.resize (pointLightSlots, glm::vec4 (0.0f));
    m_pointLightOrigins.resize (pointLightSlots, glm::vec4 (0.0f));
    for (const auto& object : scene->objects) {
	if (!object->is<ScenePointLight> () || m_rejectedObjectIds.contains (object->id)) continue;
	auto* light = object->as<ScenePointLight> ();
	assignLegacyLightSlot (light);
	m_pointLightObjects.push_back (light);
    }
    refreshPointLights ();
    m_spotLightColors.resize (scene->spotLightSlots, glm::vec4 (0.0f));
    m_spotLightOrigins.resize (scene->spotLightSlots, glm::vec4 (0.0f));
    m_spotLightDirections.resize (scene->spotLightSlots, glm::vec4 (0.0f));
    m_spotLightExponents.resize (scene->spotLightSlots, glm::vec4 (0.0f));
    for (const auto& object : scene->objects) {
        if (object->is<SceneSpotLight> () && !m_rejectedObjectIds.contains (object->id))
            m_spotLightObjects.push_back (object->as<SceneSpotLight> ());
    }
    refreshSpotLights ();

    // create all objects based off their dependencies
    for (const auto& object : scene->objects) {
	this->createObject (*object);
    }

    // copy over objects by render order
    for (const auto& object : scene->objects) {
	this->addObjectToRenderOrder (*object);
    }

    // create extra framebuffers for the bloom effect
    this->_rt_4FrameBuffer = this->create (
	"_rt_4FrameBuffer", sceneFormat, TextureFlags_ClampUVs, 1.0, { sceneWidth / 4, sceneHeight / 4 },
	{ sceneWidth / 4, sceneHeight / 4 }
    );
    this->_rt_8FrameBuffer = this->create (
	"_rt_8FrameBuffer", sceneFormat, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    this->_rt_Bloom = this->create (
	"_rt_Bloom", sceneFormat, TextureFlags_ClampUVs, 1.0, { sceneWidth / 8, sceneHeight / 8 },
	{ sceneWidth / 8, sceneHeight / 8 }
    );
    if (m_hdrPostprocessing) {
	this->setHdrPresentation ({
	    .strength = scene->camera.bloom.hdrStrength->value->getFloat (),
	    .threshold = scene->camera.bloom.hdrThreshold->value->getFloat (),
	    .feather = scene->camera.bloom.hdrFeather->value->getFloat (),
	    .scatter = scene->camera.bloom.hdrScatter->value->getFloat (),
	    .iterations = scene->camera.bloom.hdrIterations->value->getInt (),
	    .tint = scene->camera.bloom.tint->value->getVec3 (),
	});
    }

    //
    // Had to get a little creative with the effects to achieve the same bloom effect without any custom code
    // this custom image loads some effect files from the virtual container to achieve the same bloom effect
    // this approach requires of two extra draw calls due to the way the effect works in official WPE
    // (it renders directly to the screen, whereas here we never do that from a scene)
    //

    const auto bloomOrigin = glm::vec3 { sceneWidth / 2, sceneHeight / 2, 0.0f };
    const auto bloomSize = glm::vec2 { sceneWidth, sceneHeight };

    const JSON bloom
	= { { "image", "models/wpenginelinux.json" },
	    { "name", "bloomimagewpenginelinux" },
	    { "visible", true },
	    { "scale", "1.0 1.0 1.0" },
	    { "angles", "0.0 0.0 0.0" },
	    { "origin",
	      std::to_string (bloomOrigin.x) + " " + std::to_string (bloomOrigin.y) + " "
		  + std::to_string (bloomOrigin.z) },
	    { "size", std::to_string (bloomSize.x) + " " + std::to_string (bloomSize.y) },
	    { "id", -1 },
	    { "effects",
	      JSON::array (
		  { { { "file", "effects/wpenginelinux/bloomeffect.json" },
		      { "id", 15242000 },
		      { "name", "" },
		      { "passes",
			JSON::array (
			    { { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } },
			      { { "constantshadervalues",
				  { { "bloomstrength", this->getScene ().camera.bloom.strength->value->getFloat () },
				    { "bloomthreshold",
				      this->getScene ().camera.bloom.threshold->value->getFloat () } } } } }
			) } } }
	      ) } };

    // create image for bloom passes
    if (scene->camera.bloom.enabled->value->getBool () && !m_hdrPostprocessing) {
	this->m_bloomObjectData = ObjectParser::parse (bloom, scene->project);
	this->m_bloomObject = this->createObject (*this->m_bloomObjectData);

	this->m_objectsByRenderOrder.push_back (this->m_bloomObject);
    }
}

CScene::~CScene () {
    // Script destroy hooks may read their layer's model values, so run them
    // while render objects and their bound DynamicValues are still alive.
    m_shuttingDown = true;
    if (m_scriptEngine) m_scriptEngine->shutdown ();
    // bloom object is in the objects list, so no need to explicitly delete it
    this->m_bloomObject = nullptr;

    for (const auto& val : this->m_objects | std::views::values) {
	delete val;
    }

    this->m_objectsByRenderOrder.clear ();
    this->m_objects.clear ();
}

Render::CObject* CScene::createObject (const Object& object) {
    Render::CObject* renderObject = nullptr;

    if (m_rejectedObjectIds.contains (object.id)) return nullptr;

    // ensure the item is not loaded already
    if (const auto current = this->m_objects.find (object.id); current != this->m_objects.end ()) {
	return current->second;
    }

    if (!m_creatingObjects.insert (object.id).second) {
	throw std::invalid_argument ("Scene object creation cycle at id " + std::to_string (object.id));
    }
    struct CreationGuard {
	std::set<int>& active;
	int id;
	~CreationGuard () { active.erase (id); }
    } guard { m_creatingObjects, object.id };

    // check dependencies too!
    for (const auto& cur : object.dependencies) {
	// self-dependency is a possibility...
	if (cur == object.id) {
	    continue;
	}

	if (const auto* dep = findObjectData (cur)) {
	    if (this->createObject (*dep) == nullptr) {
		sLog.error ("Skipping scene object ", object.id,
		            " because dependency ", cur, " could not be created");
		m_rejectedObjectIds.insert (object.id);
		return nullptr;
	    }
	}
    }

    // check if the item has any parent and also create it first
    if (object.parent.has_value ()) {
	int parentId = object.parent.value ();

	const auto* dep = findObjectData (parentId);
	if (!dep) {
	    sLog.exception ("Cannot find parent ", parentId, " for object ", object.id);
	}

	if (this->createObject (*dep) == nullptr) {
	    sLog.error ("Skipping scene object ", object.id,
	                " because parent ", parentId, " could not be created");
	    m_rejectedObjectIds.insert (object.id);
	    return nullptr;
	}
    }

    renderObject = this->dispatchObjectType (object);

    if (renderObject != nullptr) {
	this->m_objects.emplace (renderObject->getId (), renderObject);
    } else {
	m_rejectedObjectIds.insert (object.id);
    }

    return renderObject;
}

const Object* CScene::findObjectData (int id) const {
    const auto authored = std::ranges::find_if (getScene ().objects,
        [id] (const auto& candidate) { return candidate->id == id; });
    if (authored != getScene ().objects.end ()) return authored->get ();
    const auto dynamic = m_scriptObjectData.find (id);
    return dynamic == m_scriptObjectData.end () ? nullptr : dynamic->second.get ();
}

Render::CObject* CScene::dispatchObjectType (const Object& object) {
    Render::CObject* renderObject = nullptr;

    try {
	if (object.is<Image> ()) {
	    renderObject = new Objects::CImage (*this, *object.as<Image> ());
	} else if (object.is<SceneModel> ()) {
	    renderObject = new Objects::CModel (*this, *object.as<SceneModel> ());
	} else if (object.is<SceneCamera> ()) {
	    // The camera participates in scene selection but has no draw call.
	    renderObject = new CObject (*this, object);
        } else if (object.is<SceneSpotLight> ()) {
            renderObject = new CSpotLight (*this, *object.as<SceneSpotLight> ());
	} else if (object.is<ScenePointLight> ()) {
	    renderObject = new CPointLight (*this, *object.as<ScenePointLight> ());
	} else if (object.is<Sound> ()) {
	    renderObject = new Objects::CSound (*this, *object.as<Sound> ());
	} else if (object.is<Text> ()) {
	    renderObject = new Objects::CText (*this, *object.as<Text> ());
	} else if (object.is<Particle> ()) {
	    const auto& particleData = *object.as<Particle> ();

	    if (this->getContext ().getApp ().getContext ().settings.general.disableParticles == true) {
		sLog.debug ("Ignoring particle system (disabled in settings): ", particleData.name);
		return nullptr;
	    }
	    if (!particleData.material || !particleData.material->material
	        || particleData.material->material->passes.empty ()) {
		sLog.error ("Skipping particle system without a renderable material: ", particleData.name,
	                    " (object ", particleData.id, ")");
		return nullptr;
	    }

	    renderObject = new Objects::CParticle (*this, particleData);
	} else {
	    renderObject = new CTransformObject (*this, object);
	}
	renderObject->setup ();
    } catch (const std::exception& e) {
	sLog.error ("Failed to create or setup object ", object.id, ": ", e.what ());
	delete renderObject;
	renderObject = nullptr;
    }

    return renderObject;
}

void CScene::addObjectToRenderOrder (const Object& object) {
    const auto obj = this->m_objects.find (object.id);

    // ignores not created objects like particle systems
    if (obj == this->m_objects.end ()) {
	return;
    }

    // take into account any dependency first
    for (const auto& dep : object.dependencies) {
	// self-dependency is possible
	if (dep == object.id) {
	    continue;
	}

	// add the dependency to the list if it's created
	auto depIt = std::ranges::find_if (this->getScene ().objects, [&dep] (const auto& o) { return o->id == dep; });

	if (depIt != this->getScene ().objects.end ()) {
	    this->addObjectToRenderOrder (**depIt);
	} else {
	    sLog.error ("Cannot find dependency ", dep, " for object ", object.id);
	}
    }

    // ensure we're added only once to the render list
    const auto renderIt = std::ranges::find_if (this->m_objectsByRenderOrder, [&object] (const auto& o) {
	return o->getId () == object.id;
    });

    if (renderIt == this->m_objectsByRenderOrder.end ()) {
	this->m_objectsByRenderOrder.emplace_back (obj->second);
    }
}

ScriptEngine& CScene::getScriptEngine () const { return *this->m_scriptEngine; }
Camera& CScene::getCamera () const { return *this->m_camera; }
std::shared_ptr<const CFBO> CScene::getActiveRenderTarget () const {
    return m_activeRenderTarget ? m_activeRenderTarget : getFBO ();
}
const glm::mat4& CScene::getActiveRenderProjection () const { return m_activeRenderProjection; }
bool CScene::isChildCompositionScope () const { return m_childCompositionScope; }
bool CScene::isMaxAlphaCompositionScope () const { return m_maxAlphaCompositionScope; }
const Audio::Drivers::Recorders::StereoSpectrum::Bands& CScene::getAudioSpectrum () const {
    return this->m_audioSpectrum.bands ();
}

void CScene::renderFrame (const glm::ivec4& viewport) {
    m_particleFrameDurations.publish (getDeltaTime (), getContext ().getDriver ().getFrameCounter ());
    // Native WM_SIZE -> 14017f1b0 resizes root/auxiliary targets in both
    // camera modes. Authored orthographic dimensions remain scene units;
    // 140183a70 applies the output crop through projection instead.
    if (viewport.z > 1 && viewport.w > 1) {
        resizeSceneTargets (viewport.z, viewport.w);
        auto presentation = getState ();
        presentation.updateState (viewport, false, getWidth (), getHeight ());
        const auto uv = presentation.getTextureUVs ();
        m_rootRenderClipTransform = scenePresentationClipTransform (
            {uv.ustart, uv.uend, uv.vstart, uv.vend});
        m_presentationTextureSize = glm::vec2 (viewport.z, viewport.w);
    }
    // Native 14017fa70:217–225 advances this scene clock before its particle
    // tree ticks. The float sent to the particle context wraps after 432000 s.
    m_particleSceneTime = advanceParticleSceneClock (
        m_particleSceneTimeAccumulator, getDeltaTime ());
    // A scene owns its filter history. Repeated viewports at the same scene
    // time consume one published frame; a paused scene retains its last one.
    const auto& recorder = this->getAudioContext ().getRecorder ();
    Audio::Drivers::Recorders::StereoSpectrum::Bands raw;
    std::copy_n (recorder.audio64RawLeft, 64, raw.audio64[0].begin ());
    std::copy_n (recorder.audio64RawRight, 64, raw.audio64[1].begin ());
    // Native rate comes from the host's wproperties, whose Linux owner is
    // not mapped to project.json custom properties. Both rate and visibility
    // fade use their neutral visible-state value until that mapping is proven.
    m_audioSpectrum.advance (raw, this->getContext ().getDriver ().getFrameCounter (),
                             std::chrono::steady_clock::now (), 1.0f, 1.0f);

    // ensure the virtual mouse position is up to date
    this->updateMouse (viewport);

    // update the parallax position if required
    if (this->getScene ().camera.parallax.enabled->value->getBool ()
	&& !this->getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	const float influence = this->getScene ().camera.parallax.mouseInfluence->value->getFloat ();
	const float amount = this->getScene ().camera.parallax.amount->value->getFloat ();
	const float delay = sceneParallaxDelayWeight (
	    this->getScene ().camera.parallax.delay->value->getFloat (), g_Time - g_TimeLast);

	const glm::vec2 centeredMouse = this->m_mousePosition - glm::vec2 (0.5f, 0.5f);
	this->m_parallaxDisplacement
	    = glm::mix (this->m_parallaxDisplacement, (centeredMouse * amount) * influence, delay);
    }

    // Native updates image controls before SceneScript callbacks (1401891a0
    // before 140171440 in 14017fa70). Joined handles observe the cached
    // texture's last sampled cursor without advancing it.
    for (const auto& object : m_objectsByRenderOrder)
        if (object->is<Objects::CImage> ())
            object->as<Objects::CImage> ()->advanceTextureAnimation (
                getDeltaTime (), getContext ().getDriver ().getFrameCounter ());
    // Particle outer updates publish geometry before script callbacks. API
    // emissions and inner-only warmup after this point retain that publication.
    for (const auto& object : m_objectsByRenderOrder)
        if (object->is<Objects::CParticle> ())
            object->as<Objects::CParticle> ()->advanceFrame ();
    // Native cursor dispatch (140189e10) precedes SceneScript update (140171440).
    dispatchCursorEvents ();
    // run a tick in the javascript logic
    this->getScriptEngine ().tick ();
    if (m_hdrPostprocessing) {
	const auto& bloom = this->getScene ().camera.bloom;
	updateHdrBloomSettings ({
	    .strength = bloom.hdrStrength->value->getFloat (),
	    .threshold = bloom.hdrThreshold->value->getFloat (),
	    .feather = bloom.hdrFeather->value->getFloat (),
	    .scatter = bloom.hdrScatter->value->getFloat (),
	    .iterations = bloom.hdrIterations->value->getInt (),
	    .tint = bloom.tint->value->getVec3 (),
	});
    }
    flushDestroyedScriptLayers ();
    refreshPointLights ();
    refreshSpotLights ();

    // Sound scheduling follows scene time even when a sound layer is hidden
    // by a render-only debug filter. Playback itself runs in the audio driver.
    for (const auto& object : this->m_objectsByRenderOrder) {
	if (object->is<Objects::CSound> ()) object->as<Objects::CSound> ()->tick (this->getDeltaTime ());
    }

    // update main textures for images
    for (const auto& cur : this->m_objectsByRenderOrder) {
	if (!cur->is<Objects::CImage> ()) {
	    continue;
	}

	const Objects::CImage* image = cur->as<Objects::CImage> ();

#if !NDEBUG
	const std::string message = "Updating texture " + image->getImage ().model->filename;

	glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, message.c_str ());
#endif

	image->getTexture ()->update ();

#if !NDEBUG
	glPopDebugGroup ();
#endif
    }

    // bind the vertex array
    glBindVertexArray (this->m_vaoBuffer);
    // use the scene's framebuffer by default
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_sceneFBO->getFramebuffer ());
    // ensure we render over the whole framebuffer
    glViewport (0, 0, this->m_sceneFBO->getRealWidth (), this->m_sceneFBO->getRealHeight ());

    // A final root image pass may mask alpha writes. Start each scene frame
    // with all color channels enabled so earlier particle layers write alpha.
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glDepthMask (GL_TRUE);
    glClearDepth (1.0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_activeRenderTarget = getFBO ();
    m_activeRenderProjection = m_rootRenderClipTransform
        * getCamera ().getProjection () * getCamera ().getRenderLookAt ();
    m_childCompositionScope = false;
    m_maxAlphaCompositionScope = false;

    const auto& debug = this->getContext ().getApp ().getContext ().settings.render.debug;
    if (debug.objectFilter.has_value () || !debug.skipObjects.empty ()) {
	const auto scheduled = m_objectsByRenderOrder;
	for (auto* cur : scheduled) {
	    if (debug.objectFilter.has_value () && cur->getId () != debug.objectFilter.value ()) continue;
	    if (std::ranges::find (debug.skipObjects, cur->getId ()) != debug.skipObjects.end ()) continue;
	    cur->render ();
	}
	return;
    }

    std::unordered_map<int, CObject*> present;
    for (auto* object : m_objectsByRenderOrder) present.emplace (object->getId (), object);
    std::unordered_map<int, std::vector<CObject*>> children;
    std::vector<CObject*> roots;
    // Keep ordinary objects in the authored/dependency render order. Only a
    // passthrough image with an effect owns an offscreen child draw scope.
    // Its descendants draw at that scope's position, even across ordinary
    // transform-only ancestors; nested scopes recurse at their own position.
    for (auto* object : m_objectsByRenderOrder) {
	std::optional<int> owner;
	auto parent = object->getObject ().parent;
	while (parent) {
	    const auto ancestor = present.find (*parent);
	    if (ancestor == present.end ()) break;
	    if (ancestor->second->is<Objects::CImage> ()
		&& ancestor->second->as<Objects::CImage> ()->canComposeChildren ()) {
		owner = *parent;
		break;
	    }
	    parent = ancestor->second->getObject ().parent;
	}
	if (owner) children[*owner].push_back (object);
	else roots.push_back (object);
    }

    std::function<void (CObject*)> renderNode = [&] (CObject* object) {
	const auto childIt = children.find (object->getId ());
	if (childIt == children.end () || childIt->second.empty () || !object->is<Objects::CImage> ()
	    || !object->as<Objects::CImage> ()->canComposeChildren ()) {
	    object->render ();
	    if (childIt != children.end ()) for (auto* child : childIt->second) renderNode (child);
	    return;
	}
	auto* image = object->as<Objects::CImage> ();
	image->renderWithChildren ([&] (std::shared_ptr<const CFBO> target) {
	    const auto transform = resolveSceneTransform (image->getImage (), [this] (int parentId) -> const Object* {
		const auto* parent = getObject (parentId);
		return parent ? &parent->getObject () : nullptr;
	    }, [this] (const Object& parent, const std::string& name) {
		return getPuppetAttachmentTransform (parent.id, name);
	    });
	    const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
	    const glm::mat4 world = getCamera ().isOrthogonal ()
		? glm::translate (
		      glm::mat4 (1.0f), glm::vec3 (-getWidth () * 0.5f, getHeight () * 0.5f, 0.0f))
		      * flip * transform.authoredMatrix
		: transform.authoredMatrix;
	    const auto inverse = inverseFiniteTransform (world);
	    if (!inverse) return;
	    const float width = static_cast<float> (target->getRealWidth ());
	    const float height = static_cast<float> (target->getRealHeight ());
	    const glm::mat4 localFlip = getCamera ().isOrthogonal () ? flip : glm::mat4 (1.0f);
	    const glm::mat4 projection = glm::ortho (-width * 0.5f, width * 0.5f,
		-height * 0.5f, height * 0.5f, -1000.0f, 1000.0f) * localFlip * *inverse;
	    const auto previousTarget = m_activeRenderTarget;
	    const auto previousProjection = m_activeRenderProjection;
	    const bool previousScope = m_childCompositionScope;
	    const bool previousMaxAlpha = m_maxAlphaCompositionScope;
	    m_activeRenderTarget = target;
	    m_activeRenderProjection = projection;
	    m_childCompositionScope = true;
	    m_maxAlphaCompositionScope = previousMaxAlpha
		|| !image->getImage ().copyBackground->value->getBool ();
	    auto restore = [&] {
		m_activeRenderTarget = previousTarget;
		m_activeRenderProjection = previousProjection;
		m_childCompositionScope = previousScope;
		m_maxAlphaCompositionScope = previousMaxAlpha;
	    };
	    try {
		for (auto* child : childIt->second) renderNode (child);
	    } catch (...) {
		restore ();
		throw;
	    }
	    restore ();
	});
    };
    for (auto* root : roots) renderNode (root);
}

void CScene::resizeSceneTargets (int width, int height) {
    if (!m_camera->isOrthogonal ()) {
        const bool changed = getWidth () != width || getHeight () != height;
        if (changed) {
            m_camera->setPerspectiveProjection (width, height);
            if (m_scriptEngine) m_scriptEngine->notifyScreenResize (width, height);
        }
    }
    if (m_sceneFBO->getRealWidth () == static_cast<uint32_t> (width)
        && m_sceneFBO->getRealHeight () == static_cast<uint32_t> (height)) return;
    const auto resize = [] (const std::shared_ptr<CFBO>& target, int w, int h) {
        if (target) target->resize (w, h, w, h);
    };
    // Retain CFBO objects (and therefore references held by effect passes).
    // CFBO::resize also replaces the root D16 attachment and texture storage.
    resize (m_sceneFBO, width, height);
    resize (_rt_shadowAtlas, width, height);
    resize (_rt_4FrameBuffer, std::max (1, width / 4), std::max (1, height / 4));
    resize (_rt_8FrameBuffer, std::max (1, width / 8), std::max (1, height / 8));
    resize (_rt_Bloom, std::max (1, width / 8), std::max (1, height / 8));
    resizeHdrPresentation (width, height);
}

void CScene::updateMouse (const glm::ivec4& viewport) {
    // update virtual mouse position first
    const glm::dvec2 position = this->getContext ().getInputContext ().getMouseInput ().position ();
    m_mouseScreenPosition = cursorScreenPosition (position, viewport);
    m_mouseLeftDown = getContext ().getInputContext ().getMouseInput ().leftClick ()
        == Input::MouseClickStatus::Clicked;

    // rollover the position to the last
    this->m_mousePositionLast = this->m_mousePosition;

    // calculate the current position of the mouse in viewport space [0, 1]
    double mouseX = glm::clamp ((position.x - viewport.x) / viewport.z, 0.0, 1.0);
    // Normalize Y coordinate (OpenGL convention: 0=bottom, 1=top)
    // Particle code expects this convention: 0=bottom results in negative Y (down), 1=top results in positive Y (up)
    double normalizedMouseY = glm::clamp ((position.y - viewport.y) / viewport.w, 0.0, 1.0);

    // Account for UV cropping when using fill/fit scaling modes
    // The scene may be rendered larger than viewport and cropped via UVs
    const auto uvs = this->getState ().getTextureUVs ();

    // Map mouse position from viewport space to scene UV space
    // UVs define what portion of the scene texture is visible
    this->m_mousePositionNormalized.x = uvs.ustart + mouseX * (uvs.uend - uvs.ustart);
    this->m_mousePositionNormalized.y = uvs.vstart + normalizedMouseY * (uvs.vend - uvs.vstart);

    // Invert previous normalization of Y to match what the shader expects
    double mouseY = 1.0 - normalizedMouseY;

    this->m_mousePosition.x = this->m_mousePositionNormalized.x;
    this->m_mousePosition.y = uvs.vstart + mouseY * (uvs.vend - uvs.vstart);
}

void CScene::dispatchCursorEvents () {
    const auto world = getMouseWorldPosition ();
    if (!world) return; // Cursor events are documented for 2D scene layers.
    const bool moved = !m_cursorInputInitialized
        || m_previousCursorScreenPosition != m_mouseScreenPosition;
    m_cursorInputInitialized = true;
    m_previousCursorScreenPosition = m_mouseScreenPosition;
    m_cursorState.beginFrame (moved, m_mouseLeftDown);

    struct LayerLocation { glm::vec3 local; bool inside; bool visible; };
    auto layerLocation = [this, &world] (const CObject& object)
        -> std::optional<LayerLocation> {
        glm::vec2 size {};
        glm::vec2 alignment {};
        if (object.is<Objects::CImage> ()) {
            const auto* image = object.as<Objects::CImage> ();
            size = image->getSize ();
            const auto& authored = image->getImage ().alignment;
            if (authored.find ("top") != std::string::npos) alignment.y = -size.y * 0.5f;
            else if (authored.find ("bottom") != std::string::npos) alignment.y = size.y * 0.5f;
            if (authored.find ("left") != std::string::npos) alignment.x = size.x * 0.5f;
            else if (authored.find ("right") != std::string::npos) alignment.x = -size.x * 0.5f;
        } else if (object.is<Objects::CText> ()) {
            const auto* text = object.as<Objects::CText> ();
            size = text->getLayoutSize ();
            alignment = text->getLayoutOffset ();
        }
        else return std::nullopt;
        const auto transform = resolveSceneTransform (object.getObject (),
            [this] (int parentId) -> const Object* {
                const auto* parent = getObject (parentId);
                return parent ? &parent->getObject () : nullptr;
            }, [this] (const Object& parent, const std::string& name) {
                return getPuppetAttachmentTransform (parent.id, name);
            });
        const auto local = cursorLocalPosition (*world, transform.authoredMatrix, size, alignment);
        if (!local) return std::nullopt;
        const bool inside = local->x >= 0.0f && local->x <= size.x
            && local->y >= 0.0f && local->y <= size.y;
        return LayerLocation {*local, inside, transform.visible};
    };
    // Native visits each solid hit from front to back. A propagation blocker
    // stops later candidates only when its hit is visible through its parents.
    // Keep the existing geometry; alpha masks and puppet hit boxes are separate.
    // Cursor callbacks can create layers and reallocate the draw list. Bound
    // this dispatch to its initial identities and resolve each live layer again.
    // Native keeps pending removals eligible for this frame's input; physical
    // deletion happens after the subsequent script tick and clears their state.
    std::vector<int> candidates;
    candidates.reserve (m_objectsByRenderOrder.size ());
    for (const auto* object : m_objectsByRenderOrder) candidates.push_back (object->getId ());
    for (auto it = candidates.rbegin (); it != candidates.rend (); ++it) {
        const int id = *it;
        auto* object = getObject (id);
        if (!object) {
            m_cursorState.forget (id);
            continue;
        }
        if (!object->is<Scripting::ScriptableObject> ()) continue;
        auto* scriptable = object->as<Scripting::ScriptableObject> ();
        if (!scriptable->isSolid ()) continue;
        const auto location = layerLocation (*object);
        if (!location) continue;
        m_cursorState.visit (id, location->inside, [&] (const char* event) {
            const auto* target = getObject (id);
            if (!target || !target->is<Scripting::ScriptableObject> ()) return;
            m_scriptEngine->dispatchCursorEvent (*target->as<Scripting::ScriptableObject> (),
                                                 event, *world, location->local);
        });
        object = getObject (id);
        if (!object) continue;
        scriptable = object->as<Scripting::ScriptableObject> ();
        if (!m_cursorState.hasButtonCapture () && location->inside
            && scriptable->disablesCursorPropagation ()) {
            const auto currentLocation = layerLocation (*object);
            if (currentLocation && currentLocation->visible) break;
        }
    }
    m_cursorState.finishFrame ();
}

const Scene& CScene::getScene () const { return *this->getWallpaperData ().as<Scene> (); }

int CScene::getWidth () const { return this->m_camera->getWidth (); }

int CScene::getHeight () const { return this->m_camera->getHeight (); }

float CScene::getTime () const { return g_Time; }

float CScene::getDeltaTime () const { return g_Time - g_TimeLast; }

float CScene::getParticleSceneTime () const { return m_particleSceneTime; }

float CScene::getFps () const {
    const float dt = g_Time - g_TimeLast;
    // Guard against the first frame (where g_TimeLast is 0 so dt == g_Time)
    // and division by zero on the very first call.
    if (dt <= 1e-6f) {
	return 60.0f;
    }
    return 1.0f / dt;
}

const glm::vec2* CScene::getMousePosition () const { return &this->m_mousePosition; }

const glm::vec2* CScene::getMousePositionLast () const { return &this->m_mousePositionLast; }

const glm::vec2* CScene::getMousePositionNormalized () const { return &this->m_mousePositionNormalized; }

const glm::vec2& CScene::getMouseScreenPosition () const { return m_mouseScreenPosition; }

std::optional<glm::vec3> CScene::getMouseWorldPosition () const {
    if (!m_camera->isOrthogonal ()) return std::nullopt;
    return cursorWorldPosition (m_mousePositionNormalized, m_camera->getProjection (),
                                m_camera->getWidth (), m_camera->getHeight ());
}

bool CScene::isMouseLeftDown () const { return m_mouseLeftDown; }

const glm::vec2* CScene::getParallaxDisplacement () const { return &this->m_parallaxDisplacement; }

glm::vec3 CScene::getLayerParallaxOffset (const glm::vec2& depth) const {
    if (!getScene ().camera.parallax.enabled->value->getBool ()
        || getContext ().getApp ().getContext ().settings.mouse.disableparallax)
        return {};
    return sceneParallaxOffset (depth, m_parallaxDisplacement, static_cast<float> (getWidth ()),
                                m_camera->isOrthogonal ());
}
glm::vec3 CScene::getLayerParallaxOffset (const glm::vec3& origin,
                                         const glm::vec2& depth) const {
    if (!getScene ().camera.parallax.enabled->value->getBool ()
        || getContext ().getApp ().getContext ().settings.mouse.disableparallax)
        return {};
    return sceneParticleParallaxOffset (
        origin, m_camera->getEye (), depth, m_parallaxDisplacement,
        static_cast<float> (getWidth ()), static_cast<float> (getHeight ()),
        getScene ().camera.parallax.amount->value->getFloat (), m_camera->isOrthogonal ());
}

const std::vector<CObject*>& CScene::getObjectsByRenderOrder () const { return this->m_objectsByRenderOrder; }

bool CScene::isScriptCreatedLayer (const CObject& object) const {
    return m_scriptObjectData.contains (object.getId ());
}

std::vector<CObject*> CScene::getScriptLayers () const {
    std::vector<CObject*> layers;
    layers.reserve (m_objectsByRenderOrder.size ());
    for (auto* object : m_objectsByRenderOrder)
        if (object != m_bloomObject
            && !m_pendingScriptLayerDestroy.contains (object->getId ())
            && !m_destroyingScriptLayerIds.contains (object->getId ())) layers.push_back (object);
    return layers;
}

std::mt19937& CScene::getParticleRandom () { return m_particleRandom; }

const CObject* CScene::getObject (int id) const {
    const auto object = this->m_objects.find (id);
    return object == this->m_objects.end () ? nullptr : object->second;
}

int CScene::getPointLightCount () const { return int (m_pointLightColors.size ()); }

const glm::vec4* CScene::getPointLightColors () const { return m_pointLightColors.data (); }

const glm::vec4* CScene::getPointLightOrigins () const { return m_pointLightOrigins.data (); }

int CScene::getSpotLightCount () const { return int (m_spotLightColors.size ()); }
const glm::vec4* CScene::getSpotLightColors () const { return m_spotLightColors.data (); }
const glm::vec4* CScene::getSpotLightOrigins () const { return m_spotLightOrigins.data (); }
const glm::vec4* CScene::getSpotLightDirections () const { return m_spotLightDirections.data (); }
const glm::vec4* CScene::getSpotLightExponents () const { return m_spotLightExponents.data (); }

const glm::vec3* CScene::getLegacyLightPositions () const { return m_legacyLightPositions.data (); }

const glm::vec4* CScene::getLegacyLightColors () const { return m_legacyLightColors.data (); }

const glm::vec4* CScene::getLegacyLightPremultipliedColors () const {
    return m_legacyLightPremultipliedColors.data ();
}

void CScene::assignLegacyLightSlot (const ScenePointLight* light) {
    if (m_legacyLightSlots.contains (light)) return;
    size_t slot = 0;
    for (; slot < 4; ++slot) {
        const bool occupied = std::any_of (m_legacyLightSlots.begin (), m_legacyLightSlots.end (),
            [slot] (const auto& entry) { return entry.second == slot; });
        if (!occupied) break;
    }
    // The native constructor falls back to slot zero when all four are used.
    m_legacyLightSlots.emplace (light, slot == 4 ? 0 : slot);
}

void CScene::refreshPointLights () {
    std::fill (m_pointLightColors.begin (), m_pointLightColors.end (), glm::vec4 (0.0f));
    std::fill (m_pointLightOrigins.begin (), m_pointLightOrigins.end (), glm::vec4 (0.0f));
    m_legacyLightPositions.fill (glm::vec3 (0.0f));
    m_legacyLightColors.fill (glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f));
    size_t modernSlot = 0;
    for (const ScenePointLight* lightPointer : m_pointLightObjects) {
	const ScenePointLight& light = *lightPointer;
        const auto transform = resolveSceneTransform (light,
            [this] (int parentId) -> const Object* { return findObjectData (parentId); },
            [this] (const Object& parent, const std::string& name) {
                return getPuppetAttachmentTransform (parent.id, name);
            });
        const glm::vec3 color = light.color->value->getVec3 ();
        const float intensity = light.intensity->value->getFloat ();
        const float radius = light.radius->value->getFloat ();
        const float exponent = light.exponent->value->getFloat ();
        const glm::vec3 origin = glm::vec3 (transform.authoredMatrix[3]);
        if (!std::isfinite (intensity) || !std::isfinite (radius) || !std::isfinite (exponent)
            || !std::isfinite (color.x) || !std::isfinite (color.y) || !std::isfinite (color.z)
            || !std::isfinite (origin.x) || !std::isfinite (origin.y) || !std::isfinite (origin.z))
            continue;
        // Legacy slots are assigned when the object is created. Removing
        // another light does not renumber a surviving object's slot.
        const size_t legacySlot = m_legacyLightSlots.at (lightPointer);
        if (!light.lightingV1) {
            m_legacyLightPositions[legacySlot] = origin;
            if (transform.visible)
                m_legacyLightColors[legacySlot] = glm::vec4 (color * intensity, radius);
            else
                m_legacyLightColors[legacySlot] = glm::vec4 (0.0f, 0.0f, 0.0f, 1.0f);
        }
        // LightingV1 uses the separate serialized slot count. The current
        // point producer compacts visible objects into that array.
        if (light.lightingV1 && transform.visible && modernSlot < m_pointLightColors.size ()) {
            m_pointLightColors[modernSlot] = glm::vec4 (color * intensity, radius);
            m_pointLightOrigins[modernSlot] = glm::vec4 (origin, exponent);
            ++modernSlot;
        }
    }
    m_legacyLightPremultipliedColors = legacyLightPremultipliedColors (m_legacyLightColors);
}

void CScene::refreshSpotLights () {
    std::fill (m_spotLightColors.begin (), m_spotLightColors.end (), glm::vec4 (0.0f));
    std::fill (m_spotLightOrigins.begin (), m_spotLightOrigins.end (), glm::vec4 (0.0f));
    std::fill (m_spotLightDirections.begin (), m_spotLightDirections.end (), glm::vec4 (0.0f));
    std::fill (m_spotLightExponents.begin (), m_spotLightExponents.end (), glm::vec4 (0.0f));
    size_t slot = 0;
    for (const SceneSpotLight* light : m_spotLightObjects) {
        if (slot >= m_spotLightColors.size ()) break;
        // Shadow/cookie atlases have a separate native producer and shader
        // branch. Keep unsupported objects out of this plain-cone producer.
        if (light->castShadow || light->useCookie) continue;
        const auto transform = resolveSceneTransform (*light,
            [this] (int parentId) -> const Object* { return findObjectData (parentId); },
            [this] (const Object& parent, const std::string& name) {
                return getPuppetAttachmentTransform (parent.id, name);
            });
        if (!transform.visible) continue;
        const auto packed = packSpotLightUniforms (
            transform.authoredMatrix, light->color->value->getVec3 (),
            light->intensity->value->getFloat (), light->radius->value->getFloat (),
            light->exponent->value->getFloat (), light->innerCone->value->getFloat (),
            light->outerCone->value->getFloat ());
        const auto finite = [] (const glm::vec4& value) {
            return std::isfinite (value.x) && std::isfinite (value.y)
                && std::isfinite (value.z) && std::isfinite (value.w);
        };
        if (!finite (packed.color) || !finite (packed.origin)
            || !finite (packed.direction) || !finite (packed.exponent)) continue;
        m_spotLightColors[slot] = packed.color;
        m_spotLightOrigins[slot] = packed.origin;
        m_spotLightDirections[slot] = packed.direction;
        m_spotLightExponents[slot] = packed.exponent;
        ++slot;
    }
}

std::optional<glm::mat4> CScene::getPuppetAttachmentTransform (
    int parentId, const std::string& name
) const {
    const auto* parent = getObject (parentId);
    if (!parent || !parent->is<Objects::CImage> ()) return std::nullopt;
    return parent->as<Objects::CImage> ()->puppetAttachmentTransform (name);
}

CObject* CScene::createScriptLayer (const std::string& configurationJson,
                                    std::shared_ptr<DynamicModelData> dynamicModel) {
    if (m_shuttingDown) throw std::runtime_error ("Cannot create a layer during scene shutdown");
    auto config = Data::JSON::JSON::parse (configurationJson);
    if (!config.is_object ()) throw std::invalid_argument ("Layer configuration must be an object");
    // SceneScript can create a plain colored rectangle from size/color/alpha
    // without naming an image. Native scenes use this for progress bars; the
    // built-in solid-layer model is the same resource used by authored bars.
    if (config.contains ("size") && config.contains ("color")
        && !config.contains ("image") && !config.contains ("text")
        && !config.contains ("particle") && !config.contains ("sound")
        && !config.contains ("model") && !config.contains ("light"))
        config["image"] = "models/util/solidlayer.json";
    if (m_nextScriptObjectId <= 0) throw std::overflow_error ("SceneScript layer IDs exhausted");
    while (findObjectData (m_nextScriptObjectId) || m_objects.contains (m_nextScriptObjectId)
           || m_creatingObjects.contains (m_nextScriptObjectId)
           || m_rejectedObjectIds.contains (m_nextScriptObjectId)) {
        if (m_nextScriptObjectId == std::numeric_limits<int>::max ())
            throw std::overflow_error ("SceneScript layer IDs exhausted");
        ++m_nextScriptObjectId;
    }
    const int id = m_nextScriptObjectId;
    m_nextScriptObjectId = id == std::numeric_limits<int>::max () ? 0 : id + 1;
    config["id"] = id;
    if (!config.contains ("name") || !config["name"].is_string ())
        config["name"] = "Script Layer " + std::to_string (id);
    auto model = ObjectParser::parse (config, getScene ().project);
    if (dynamicModel && model && model->is<SceneModel> ())
        model->as<SceneModel> ()->dynamic = std::move (dynamicModel);
    if (!model || (!model->is<Text> () && !model->is<Image> ()
                   && !model->is<Particle> () && !model->is<Sound> ()
                   && !model->is<ScenePointLight> () && !model->is<SceneSpotLight> ()
                   && !model->is<SceneModel> ()))
        throw std::invalid_argument ("Unsupported SceneScript layer configuration");
    // Destroy hooks run while their layers are still addressable. A new child
    // attached to one of those layers would outlive the parent after flush.
    std::set<int> ancestors;
    auto parent = model->parent;
    while (parent && ancestors.insert (*parent).second) {
        if (m_pendingScriptLayerDestroy.contains (*parent)
            || m_destroyingScriptLayerIds.contains (*parent))
            throw std::invalid_argument ("Cannot attach a layer to a pending destroyed parent");
        const auto* parentData = findObjectData (*parent);
        parent = parentData ? parentData->parent : std::nullopt;
    }
    const auto inserted = m_scriptObjectData.emplace (id, std::move (model));
    CObject* result = nullptr;
    try {
        result = createObject (*inserted.first->second);
    } catch (...) {
        m_scriptObjectData.erase (id);
        throw;
    }
    if (!result) {
        m_scriptObjectData.erase (id);
        throw std::runtime_error ("SceneScript layer could not be initialized");
    }
    if (inserted.first->second->is<ScenePointLight> ()) {
	auto* light = inserted.first->second->as<ScenePointLight> ();
	assignLegacyLightSlot (light);
	m_pointLightObjects.push_back (light);
    }
    if (inserted.first->second->is<SceneSpotLight> ())
        m_spotLightObjects.push_back (inserted.first->second->as<SceneSpotLight> ());
    m_objectsByRenderOrder.push_back (result);
    return result;
}

bool CScene::destroyScriptLayer (const CObject* object) {
    if (m_shuttingDown || !object) return false;
    const auto found = m_objects.find (object->getId ());
    if (found == m_objects.end () || found->second != object) return false;
    m_pendingScriptLayerDestroy.insert (object->getId ());
    return true;
}

void CScene::flushDestroyedScriptLayers () {
    while (!m_pendingScriptLayerDestroy.empty ()) {
        std::set<int> ids;
        ids.swap (m_pendingScriptLayerDestroy);
        // A child cannot keep a dangling parent transform after its parent is
        // removed. Expand the pending set before invoking destroy callbacks.
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& [id, object] : m_objects) {
                const auto parent = object->getObject ().parent;
                if (parent && ids.contains (*parent) && ids.insert (id).second)
                    changed = true;
            }
        }
        m_destroyingScriptLayerIds = ids;
        Data::Utils::ScopeGuard destroyingGuard ([this] { m_destroyingScriptLayerIds.clear (); });
        for (const int id : ids) m_cursorState.forget (id);
        std::vector<CObject*> removed;
        for (const auto id : ids)
            if (const auto found = m_objects.find (id); found != m_objects.end ())
                removed.push_back (found->second);
        for (auto* object : removed)
            if (auto* scriptable = dynamic_cast<Scripting::ScriptableObject*> (object))
                m_scriptEngine->destroyObjectModules (*scriptable);
        std::erase_if (m_objectsByRenderOrder, [&] (const CObject* object) {
            return ids.contains (object->getId ());
        });
        for (auto* object : removed) {
            const int id = object->getId ();
            if (object->getObject ().is<ScenePointLight> ()) {
		auto* light = object->getObject ().as<ScenePointLight> ();
		std::erase (m_pointLightObjects, light);
		m_legacyLightSlots.erase (light);
	    }
            if (object->getObject ().is<SceneSpotLight> ())
                std::erase (m_spotLightObjects, object->getObject ().as<SceneSpotLight> ());
            m_objects.erase (id);
            delete object;
            m_scriptObjectData.erase (id);
            m_rejectedObjectIds.insert (id);
        }
    }
}

int CScene::getScriptLayerIndex (const CObject* object) const {
    const auto found = std::find (m_objectsByRenderOrder.begin (), m_objectsByRenderOrder.end (), object);
    return found == m_objectsByRenderOrder.end () ? -1
        : static_cast<int> (found - m_objectsByRenderOrder.begin ());
}

bool CScene::sortScriptLayer (const CObject* object, int index) {
    const int oldIndex = getScriptLayerIndex (object);
    if (oldIndex < 0 || index < 0 || index >= static_cast<int> (m_objectsByRenderOrder.size ()))
        return false;
    auto* moved = m_objectsByRenderOrder[oldIndex];
    m_objectsByRenderOrder.erase (m_objectsByRenderOrder.begin () + oldIndex);
    m_objectsByRenderOrder.insert (m_objectsByRenderOrder.begin () + index, moved);
    return true;
}
