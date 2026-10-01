#include "CImage.h"

#include "CRenderable.h"
#include "ImageDeviceColor.h"
#include "ImageDimensions.h"
#include "ImagePrelighting.h"
#include "ModelNormalMatrix.h"
#include "PuppetMeshParser.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/rotate_vector.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Render/EffectClearAction.h"
#include "WallpaperEngine/Render/CTexture.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Data::Utils;

namespace {
void releaseImageCompositeMappings (FBOProvider& provider, int imageId,
                                    const std::shared_ptr<CFBO>& main,
                                    const std::shared_ptr<CFBO>& sub) {
    const std::string prefix = "_rt_imageLayerComposite_" + std::to_string (imageId);
    // A logical swap may exchange the two names while retaining the physical
    // targets. Do not erase a replacement owned by another image lifetime.
    for (const auto* suffix : {"_a", "_b"}) {
        const auto name = prefix + suffix;
        if (!provider.eraseIfMappedTo (name, main))
            provider.eraseIfMappedTo (name, sub);
    }
}

struct CompositeMappingRollback {
    FBOProvider& provider;
    int imageId;
    const std::shared_ptr<CFBO>& main;
    const std::shared_ptr<CFBO>& sub;
    bool active = true;

    ~CompositeMappingRollback () {
        if (active) releaseImageCompositeMappings (provider, imageId, main, sub);
    }
};

}

CImage::ResolvedTransform CImage::resolveTransform (const Object& object) const {
    return Wallpapers::resolveSceneTransform (object, [this] (int parentId) -> const Object* {
        const auto* parent = this->getScene ().getObject (parentId);
        return parent ? &parent->getObject () : nullptr;
    }, [this] (const Object& parent, const std::string& name) {
        return getScene ().getPuppetAttachmentTransform (parent.id, name);
    });
}

std::optional<glm::mat4> CImage::puppetAttachmentTransform (const std::string& name) const {
    if (!m_puppetAnimation) return std::nullopt;
    const_cast<CImage*> (this)->preparePuppetAnimation ();
    if (!m_puppetAnimation) return std::nullopt;
    const auto& attachments = m_puppetAnimation->attachments;
    const auto attachment = std::find_if (attachments.begin (), attachments.end (),
        [&name] (const PuppetAnimationHeader::Attachment& candidate) {
            return candidate.name == name;
        });
    if (attachment == attachments.end () || attachment->rawIndex >= m_puppetCurrentGlobals.size ())
        return std::nullopt;
    return m_puppetCurrentGlobals[attachment->rawIndex]
        * glm::make_mat4 (attachment->matrix.data ());
}

std::optional<glm::mat4> CImage::puppetEmissionBoneTransform (uint8_t boneIndex) const {
    if (boneIndex == 0xff || !m_puppetSkeleton) return std::nullopt;
    const_cast<CImage*> (this)->preparePuppetAnimation ();
    return puppetEmissionBoneMatrix (m_puppetCurrentGlobals, m_puppetInverseBind, boneIndex);
}

bool CImage::hasPuppetEmissionDeformation () const { return m_hasPuppetMesh; }

std::shared_ptr<ImageTextureAnimation> CImage::getTextureAnimation () {
    // Native 14020e670 uses the initial image texture's animated flag. It
    // caches one control state per layer, independent of cached texture data.
    if (!m_textureAnimation && m_texture && m_texture->isAnimated ()) {
        const auto* texture = dynamic_cast<const CTexture*> (m_texture.get ());
        if (texture)
            m_textureAnimation = std::make_shared<ImageTextureAnimation> (
                m_texture->getFrames (), texture->getAnimationPlayback ());
    }
    return m_textureAnimation;
}

void CImage::advanceTextureAnimation (float delta, uint32_t frame) {
    if (!m_textureAnimation || m_textureAnimationTick == frame) return;
    m_textureAnimationTick = frame;
    m_textureAnimation->advance (delta);
}

std::optional<uint32_t> CImage::textureAnimationFrameOverride () const {
    // Native 140206430 forwards the image frame to every animated texture
    // sampled during this image's material/effect passes. Negative signed
    // seeks disable that renderer override (14015f0d0), while retaining the
    // value on the script control itself.
    if (!m_textureAnimation || !m_textureAnimation->hasOverride ()) return std::nullopt;
    const uint32_t frame = m_textureAnimation->getFrame ();
    return std::bit_cast<int32_t> (frame) >= 0 ? std::optional<uint32_t> (frame) : std::nullopt;
}

void CImage::preparePuppetAnimation () {
    if (!m_hasPuppetMesh || !m_puppetAnimation || m_image.animationLayers.empty ()) return;
    const uint32_t frame = getScene ().getContext ().getDriver ().getFrameCounter ();
    if (m_puppetPoseFrame == frame) return;
    m_puppetPoseFrame = frame;
    updatePuppetAnimation ();
}

CImage::CImage (Wallpapers::CScene& scene, const Image& image) :
    CObject (scene, image), CRenderable (scene, image, *image.model->material), ScriptableObject (scene, image),
    m_sceneSpacePosition (GL_NONE), m_copySpacePosition (GL_NONE), m_passSpacePosition (GL_NONE),
    m_texcoordCopy (GL_NONE), m_texcoordPass (GL_NONE), m_modelViewProjectionScreen (),
    m_modelViewProjectionPass (glm::mat4 (1.0)), m_modelViewProjectionCopy (), m_modelViewProjectionScreenInverse (),
    m_modelViewProjectionPassInverse (glm::inverse (m_modelViewProjectionPass)), m_modelViewProjectionCopyInverse (),
    m_modelMatrix (), m_viewProjectionMatrix (), m_image (image), m_alignment (image.alignment), m_pos (), m_initialized (false) {
    for (const auto& binding : Scripting::scriptPropertyBindings (image))
	this->registerProperty (binding.name, binding.value);
    // Each animation layer owns separate UserSettings. Run their scripts with
    // this image as thisLayer while keeping the layer fields out of its public
    // script-property namespace.
    for (size_t layerIndex = 0; layerIndex < image.animationLayers.size (); ++layerIndex) {
	const auto& layer = image.animationLayers[layerIndex];
	const std::string prefix = "imageAnimationLayer" + std::to_string (layerIndex)
	    + "_object" + std::to_string (getId ()) + "_";
	auto queue = [this, &prefix] (const char* suffix, const UserSettingUniquePtr& setting) {
	    if (setting && setting->value)
		getScene ().getScriptEngine ().queueScript (prefix + suffix, *setting->value, *this);
	};
	queue ("rate", layer->rate);
	queue ("visible", layer->visible);
	queue ("blend", layer->blend);
	queue ("animation", layer->animation);
    }
    // Image effect settings also retain their own DynamicValues. Queue their
    // scripts with this image as thisLayer so instance-specific constants can
    // change after CPass setup without synthetic public script properties.
    for (size_t effectIndex = 0; effectIndex < image.effects.size (); ++effectIndex) {
	const auto& effect = image.effects[effectIndex];
	const std::string prefix = "imageEffect" + std::to_string (effectIndex)
	    + "_object" + std::to_string (getId ()) + "_";
	auto queue = [this, &prefix, effectIndex] (const std::string& suffix, const UserSettingUniquePtr& setting,
	                                         bool effectOwner = false,
	                                         const ShaderConstantMap* materialOwner = nullptr) {
	    if (setting && setting->value)
		getScene ().getScriptEngine ().queueScript (
		    prefix + suffix, *setting->value, *this,
		    effectOwner ? std::optional<size_t> (effectIndex) : std::nullopt, materialOwner);
	};
	queue ("visible", effect->visible, true);
	for (size_t passIndex = 0; passIndex < effect->effect->passes.size (); ++passIndex) {
	    const auto& pass = effect->effect->passes[passIndex];
	    if (!pass->material) continue;
	    for (size_t materialIndex = 0; materialIndex < (*pass->material)->passes.size (); ++materialIndex) {
		for (const auto& [name, setting] : (*pass->material)->passes[materialIndex]->constants)
		    queue ("pass" + std::to_string (passIndex) + "_material"
		           + std::to_string (materialIndex) + "_constant_" + name, setting, false,
		           &(*pass->material)->passes[materialIndex]->constants);
	    }
	}
	for (size_t passIndex = 0; passIndex < effect->passOverrides.size (); ++passIndex) {
	    for (const auto& [name, setting] : effect->passOverrides[passIndex]->constants)
		queue ("override" + std::to_string (passIndex) + "_constant_" + name, setting, false,
		       &effect->passOverrides[passIndex]->constants);
	}
    }

    // get scene width and height to calculate positions
    auto scene_width = static_cast<float> (scene.getWidth ());
    auto scene_height = static_cast<float> (scene.getHeight ());

    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    glm::vec2 size = this->getSize ();
    glm::vec3 scale = transform.scale;

    this->detectTexture ();

    m_hasSourceTexture = this->m_texture != nullptr;
    const auto dimensions = loadedImageDimensions (
        size, {.autosize = m_image.model->autosize,
               .fullscreen = m_image.model->fullscreen,
               .solidlayer = m_image.model->solidlayer,
               .passthrough = m_image.model->passthrough,
               .animated = m_hasSourceTexture && m_texture->isAnimated (),
               .projectlayer = m_image.model->projectlayer},
        m_hasSourceTexture ? std::optional<glm::vec2> (glm::vec2 (
            m_texture->getRealWidth (), m_texture->getRealHeight ())) : std::nullopt,
        {scene_width, scene_height});
    if (dimensions.logical != size)
        m_image.size->value->update (dimensions.logical, DynamicValue::Script);
    m_loadedTargetSize = dimensions.backing;
    m_loadedLogicalSize = dimensions.logical;
    if (m_hasSourceTexture)
        m_loadedSourceSize = {m_texture->getRealWidth (), m_texture->getRealHeight ()};
    size = dimensions.geometry;

    // detect texture (if any)
    if (this->m_texture == nullptr) {
	// if (this->m_image->isSolid ()) // layer receives cursor events:
	// https://docs.wallpaperengine.io/en/scene/scenescript/reference/event/cursor.html same applies to effects
	// TODO: create a dummy texture of correct size, fbo constructors should be enough, but this should be properly
	// handled
	const glm::vec2 backing = imageBackingDimensions (size);
	this->m_texture = std::make_shared<CFBO> (
	    "", TextureFormat_ARGB8888, TextureFlags_NoFlags, 1,
            backing.x, backing.y, backing.x, backing.y);
    }

    // fullscreen layers should use the whole projection's size
    if (this->getImage ().model->fullscreen && m_hasSourceTexture) {
	origin = { scene_width / 2, scene_height / 2, 0 };

	// TODO: CHANGE ALIGNMENT TOO?
    }
    this->m_size = size;
    const glm::vec2 backingSize = m_loadedTargetSize;
    const glm::vec2 quadProjectionSize = imageBackingDimensions (size);

    glm::vec2 scaledSize = size * glm::vec2 (scale);

    // calculate the center and shift from there
    this->m_pos.x = origin.x - (scaledSize.x / 2);
    this->m_pos.w = origin.y + (scaledSize.y / 2);
    this->m_pos.z = origin.x + (scaledSize.x / 2);
    this->m_pos.y = origin.y - (scaledSize.y / 2);

    if (m_alignment.find ("top") != std::string::npos) {
	this->m_pos.y -= scaledSize.y / 2;
	this->m_pos.w -= scaledSize.y / 2;
    } else if (m_alignment.find ("bottom") != std::string::npos) {
	this->m_pos.y += scaledSize.y / 2;
	this->m_pos.w += scaledSize.y / 2;
    }

    if (m_alignment.find ("left") != std::string::npos) {
	this->m_pos.x += scaledSize.x / 2;
	this->m_pos.z += scaledSize.x / 2;
    } else if (m_alignment.find ("right") != std::string::npos) {
	this->m_pos.x -= scaledSize.x / 2;
	this->m_pos.z -= scaledSize.x / 2;
    }

    // wallpaper engine
    this->m_pos.x -= scene_width / 2;
    this->m_pos.y = scene_height / 2 - this->m_pos.y;
    this->m_pos.z -= scene_width / 2;
    this->m_pos.w = scene_height / 2 - this->m_pos.w;

    // register both FBOs into the scene
    std::ostringstream nameA, nameB;

    // TODO: determine when _rt_imageLayerComposite and _rt_imageLayerAlbedo is used
    nameA << "_rt_imageLayerComposite_" << this->getImage ().id << "_a";
    nameB << "_rt_imageLayerComposite_" << this->getImage ().id << "_b";
    CompositeMappingRollback compositeRollback {scene, this->getImage ().id, m_mainFBO, m_subFBO};
    const auto compositeFormat = scene.getFBO ()->getFormat ();

    this->m_currentMainFBO = this->m_mainFBO = scene.create (
	nameA.str (), compositeFormat, this->m_texture->getFlags (), 1, backingSize, backingSize
    );
    this->m_currentSubFBO = this->m_subFBO = scene.create (
	nameB.str (), compositeFormat, this->m_texture->getFlags (), 1, backingSize, backingSize
    );
    m_targetBaseSize = backingSize;

    // build a list of vertices, these might need some change later (or maybe invert the camera)
    GLfloat sceneSpacePosition[] = { this->m_pos.x, this->m_pos.y, 0.0f, this->m_pos.x, this->m_pos.w, 0.0f,
				     this->m_pos.z, this->m_pos.y, 0.0f, this->m_pos.z, this->m_pos.y, 0.0f,
				     this->m_pos.x, this->m_pos.w, 0.0f, this->m_pos.z, this->m_pos.w, 0.0f };

    float width = 1.0f;
    float height = 1.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animated images use different coordinates as they're essentially a texture atlas
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }
    // calculate the correct texCoord limits for the texture based on the texture screen size and real size
    else if (
	this->getTexture () != nullptr
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())
    ) {
	// Account for padding in non-power-of-two textures: clamp UVs to the real content
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    // TODO: RECALCULATE THESE POSITIONS FOR PASSTHROUGH SO THEY TAKE THE RIGHT PART OF THE TEXTURE
    float x = 0.0f;
    float y = 0.0f;

    if (this->getTexture ()->isAnimated ()) {
	// animations should be copied completely
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
    }

    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0;
    GLfloat realY = 0.0;

    if (this->getImage ().model->passthrough) {
	// Passthrough shaders fill the destination FBO from texcoords and sample the scene using positions.
	// Keep the destination quad full-screen in local FBO space, but pass scene-space positions through.
	x = 0.0f;
	y = 0.0f;
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

    }
    // Native fullscreen geometry builder 1402066a0 uses its centered branch
    // independent of passthrough. Fullscreen vertex shaders such as cloudsbg
    // consume these clip-space positions directly.
    if (this->getImage ().model->fullscreen) {
	realX = -1.0f;
	realY = -1.0f;
	realWidth = 1.0f;
	realHeight = 1.0f;
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };
    m_texcoordCopyTopV = height;
    m_texcoordCopyBottomV = y;

    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };
    GLfloat texcoordPass[] = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    GLfloat texcoordPassPresented[] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                       1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

    GLfloat passSpacePosition[]
	= { -1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, 1.0, 0.0f, 1.0, 1.0, 0.0f, -1.0, -1.0, 0.0f, 1.0, -1.0, 0.0f };

    // bind vertex list to the openGL buffers
    glGenBuffers (1, &this->m_sceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_copySpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_STATIC_DRAW);

    // bind pass' vertex list to the openGL buffers
    glGenBuffers (1, &this->m_passSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_passSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (passSpacePosition), passSpacePosition, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordCopy);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPass);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPass);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPass), texcoordPass, GL_STATIC_DRAW);

    glGenBuffers (1, &this->m_texcoordPassPresented);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordPassPresented);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordPassPresented), texcoordPassPresented, GL_STATIC_DRAW);

    this->m_hasPuppetMesh = this->loadPuppetMesh (size);

    // compute the center of the image in scene space for rotation
    this->m_sceneCenter
	= glm::vec3 ((this->m_pos.x + this->m_pos.z) / 2.0f, (this->m_pos.y + this->m_pos.w) / 2.0f, 0.0f);

    this->m_modelViewProjectionScreen
	= this->getScene ().getCamera ().getProjection () * this->getScene ().getCamera ().getRenderLookAt ();

    if (this->getImage ().model->passthrough) {
	this->m_modelViewProjectionCopy = this->m_modelViewProjectionScreen;
    } else if (this->getImage ().model->fullscreen) {
	this->m_modelViewProjectionCopy = glm::mat4 (1.0f);
    } else {
	this->m_modelViewProjectionCopy = glm::ortho<float> (0.0, quadProjectionSize.x, 0.0, quadProjectionSize.y);
    }
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, quadProjectionSize.x, 0.0, quadProjectionSize.y);
    this->m_viewProjectionMatrix = glm::mat4 (1.0);

    // ensure the input texture is marked as used
    // this makes video playback start if it's not already
    this->m_texture->incrementUsageCount ();
    compositeRollback.active = false;
}

CImage::~CImage () {
    this->m_texture->decrementUsageCount ();

    // Script-created images can be destroyed while the scene remains alive.
    // The scene provider must release their named composite mappings, while
    // any external alias still retains its own shared target reference.
    releaseImageCompositeMappings (getScene (), getId (), m_mainFBO, m_subFBO);

    delete m_puppetChannelBasePass;
    delete m_puppetChannelPass;
    if (m_puppetChannelPosition != GL_NONE) glDeleteBuffers (1, &m_puppetChannelPosition);
    if (m_puppetChannelTexcoord != GL_NONE) glDeleteBuffers (1, &m_puppetChannelTexcoord);
    if (m_puppetChannelBlendIndices != GL_NONE) glDeleteBuffers (1, &m_puppetChannelBlendIndices);
    if (m_puppetChannelIndices != GL_NONE) glDeleteBuffers (1, &m_puppetChannelIndices);

    // delete passes first as they depend on the image's data
    for (auto* pass : this->m_passes) {
	delete pass;
    }

    this->m_passes.clear ();

    // free any gl resources
    glDeleteBuffers (1, &this->m_sceneSpacePosition);
    if (m_lightingLocalPosition != GL_NONE) glDeleteBuffers (1, &m_lightingLocalPosition);
    if (m_prelightingLocalPosition != GL_NONE) glDeleteBuffers (1, &m_prelightingLocalPosition);
    glDeleteBuffers (1, &this->m_copySpacePosition);
    glDeleteBuffers (1, &this->m_passSpacePosition);
    glDeleteBuffers (1, &this->m_texcoordCopy);
    glDeleteBuffers (1, &this->m_texcoordPass);
    glDeleteBuffers (1, &this->m_texcoordPassPresented);
    if (this->m_puppetSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSpacePosition);
    }
    if (this->m_puppetSceneSpacePosition != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetSceneSpacePosition);
    }
    if (this->m_puppetTexCoord != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoord);
    }
    if (this->m_puppetTexCoordFull != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetTexCoordFull);
    }
    if (this->m_puppetBlendIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetBlendIndices);
    }
    if (this->m_puppetIndices != GL_NONE) {
	glDeleteBuffers (1, &this->m_puppetIndices);
    }
}

bool CImage::loadPuppetMesh (const glm::vec2& size) {
    if (!this->getImage ().model->puppet.has_value ()) {
	return false;
    }

    try {
	const auto stream = this->getScene ().getScene ().project.assetLocator->read (*this->getImage ().model->puppet);
	std::vector<char> data { std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> () };

	const std::span<const uint8_t> bytes {
	    reinterpret_cast<const uint8_t*> (data.data ()), data.size ()};
	const auto model = parsePuppetMeshes (bytes);
	if (model.meshes.size () != 1 && model.meshes.size () != 2)
	    throw std::runtime_error ("Unsupported puppet mesh count");
	if (model.meshes.size () == 2 &&
	    ((model.meshes[0].meshFlags & 2) != 0 ||
	     (model.meshes[1].meshFlags & 2) == 0 ||
	     model.meshes[1].vertexMask != 0x00800021))
	    throw std::runtime_error ("Unsupported puppet channel mesh selection or layout");
	const auto& mesh = model.meshes.front ();
	m_puppetMesh = mesh;
	{
	    try {
		auto skeleton = parsePuppetSkeleton (bytes, model);
		m_puppetInverseBind = puppetInverseBindMatrices (skeleton);
		if (!m_image.animationLayers.empty () &&
		    std::any_of (skeleton.bones.begin (), skeleton.bones.end (),
		                 [] (const PuppetBoneRecord& bone) { return (bone.rawFlags & 2) != 0; }) &&
		    !skeleton.boneMappingKnown)
		    throw std::runtime_error ("Unresolved flag-2 puppet bone mapping table");
		if (!m_image.animationLayers.empty () && skeleton.boneMappingKnown) {
		    for (size_t i = 0; i < skeleton.bones.size (); ++i)
			if ((skeleton.bones[i].rawFlags & 2) != 0 &&
			    skeleton.mappingRecordByBone[i] >= 0)
			    throw std::runtime_error ("Unsupported mapped flag-2 puppet bone constraints");
		}
		auto animation = parsePuppetFirstAnimationHeader (bytes, skeleton);
		if (!m_image.animationLayers.empty ()) {
		    if (animation.clips.empty ()) throw std::runtime_error ("No MDLA clips");
		    m_puppetReferencePose = puppetReferencePoseSamples (skeleton);
		    m_puppetLayerStates.resize (m_image.animationLayers.size ());
		    m_puppetLayerClipIds.assign (m_image.animationLayers.size (), UINT64_MAX);
		    m_puppetLayerBlendInActive.resize (m_image.animationLayers.size ());
		    for (size_t i = 0; i < m_image.animationLayers.size (); ++i)
			m_puppetLayerBlendInActive[i] = m_image.animationLayers[i]->blendIn;
		}
		m_puppetCurrentGlobals = puppetGlobalMatrices (
		    skeleton, puppetUnanimatedLocalMatrices (skeleton));
		m_puppetSkeleton = std::move (skeleton);
		m_puppetAnimation = std::move (animation);
	    } catch (const std::exception& error) {
		sLog.error ("Puppet animation unavailable for ", *m_image.model->puppet, ": ", error.what ());
	    }
	}
	std::vector<GLfloat> texcoords;
	std::vector<GLfloat> texcoordsFull;
	std::vector<GLuint> blendIndices;
	std::vector<GLushort> indices;
	this->m_puppetRawPositions.clear ();
	this->m_puppetRawPositions.reserve (mesh.positions.size () * 3);
	texcoords.reserve (mesh.texcoords.size () * 2);
	texcoordsFull.reserve (mesh.texcoordsFull.size () * 4);
	blendIndices.reserve (mesh.blendIndices.size () * 4);
	indices.reserve (mesh.indices.size ());
	for (const auto& position : mesh.positions) {
	    this->m_puppetRawPositions.insert (this->m_puppetRawPositions.end (), position.begin (), position.end ());
	}
	for (const auto& uv : mesh.texcoords) {
	    texcoords.insert (texcoords.end (), uv.begin (), uv.end ());
	}
	for (const auto& uv : mesh.texcoordsFull)
	    texcoordsFull.insert (texcoordsFull.end (), uv.begin (), uv.end ());
	for (const auto& influences : mesh.blendIndices)
	    blendIndices.insert (blendIndices.end (), influences.begin (), influences.end ());
	indices.insert (indices.end (), mesh.indices.begin (), mesh.indices.end ());

	this->updatePuppetPositionBuffer (
	    size, resolveTransform (m_image), float (getScene ().getWidth ()), float (getScene ().getHeight ()));

	glGenBuffers (1, &this->m_puppetTexCoord);
	glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
	glBufferData (GL_ARRAY_BUFFER, texcoords.size () * sizeof (GLfloat), texcoords.data (), GL_STATIC_DRAW);
	if (mesh.vertexMask & 0x20) {
	    glGenBuffers (1, &this->m_puppetTexCoordFull);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoordFull);
	    glBufferData (GL_ARRAY_BUFFER, texcoordsFull.size () * sizeof (GLfloat), texcoordsFull.data (), GL_STATIC_DRAW);
	    const uint32_t highestIndex = std::accumulate (
	        mesh.blendIndices.begin (), mesh.blendIndices.end (), uint32_t (0),
	        [] (uint32_t value, const std::array<uint32_t, 4>& entry) { return std::max (value, entry[0]); });
	    // Native puppet+0x350 holds 16 floats before the selected-mesh field
	    // at +0x390; g_BlendMap therefore spans at most four vec4 rows.
	    if (mesh.blendRowCount == 0 || mesh.blendRowCount > 4 ||
	        highestIndex >= mesh.blendRowCount * 4)
		throw std::runtime_error ("Puppet channel-map index exceeds native 16-float storage");
	    m_puppetBlendMap.assign (mesh.blendRowCount, glm::vec4 (0.0f));
	}
	if (mesh.vertexMask & 0x00800000) {
	    glGenBuffers (1, &this->m_puppetBlendIndices);
	    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetBlendIndices);
	    glBufferData (GL_ARRAY_BUFFER, blendIndices.size () * sizeof (GLuint), blendIndices.data (), GL_STATIC_DRAW);
	}

	glGenBuffers (1, &this->m_puppetIndices);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	glBufferData (GL_ELEMENT_ARRAY_BUFFER, indices.size () * sizeof (GLushort), indices.data (), GL_STATIC_DRAW);

	this->m_puppetIndexCount = static_cast<GLsizei> (indices.size ());
	if (model.meshes.size () == 2) {
	    m_puppetChannelMesh = model.meshes[1];
	    // Native material flag 0x10 comes from the compiled shader's LIGHTING
	    // combo; flag 0x08 comes from an active _rt_MipMappedFrameBuffer sampler.
	    // genericimage2's hidden sampler is compiled only for REFLECTION and
	    // NORMALMAP together. These two flags select the albedo prepass.
	    m_puppetChannelOffscreen = false;
	    for (const auto& base : m_image.model->material->passes) {
		const auto combo = [&] (const char* name) {
		    const auto it = base->combos.find (name);
		    return it == base->combos.end () ? 0 : it->second;
		};
		const bool explicitMipSampler = std::any_of (
		    base->textures.begin (), base->textures.end (), [] (const auto& slot) {
			return slot.second == "_rt_MipMappedFrameBuffer";
		    });
		m_puppetChannelOffscreen |= combo ("LIGHTING") != 0 || explicitMipSampler
		    || (base->shader == "genericimage2" && combo ("REFLECTION") != 0
		        && combo ("NORMALMAP") != 0);
	    }
	    m_puppetChannelMaterial = MaterialParser::load (
	        getScene ().getScene ().project, m_puppetChannelMesh->material);
	    if (m_puppetChannelMaterial->passes.size () != 1 ||
	        m_puppetChannelMaterial->passes.front ()->shader.find ("puppettexturechannels") == std::string::npos)
	        throw std::runtime_error ("Unsupported puppet channel material");
	    const auto& channel = *m_puppetChannelMesh;
	    std::vector<GLfloat> channelPositions;
	    std::vector<GLfloat> channelUv;
	    std::vector<GLuint> channelIndices;
	    for (const auto& position : channel.positions)
	        channelPositions.insert (channelPositions.end (), position.begin (), position.end ());
	    for (const auto& uv : channel.texcoordsFull)
	        channelUv.insert (channelUv.end (), uv.begin (), uv.end ());
	    for (const auto& lanes : channel.blendIndices)
	        channelIndices.insert (channelIndices.end (), lanes.begin (), lanes.end ());
	    glGenBuffers (1, &m_puppetChannelPosition);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelPosition);
	    glBufferData (GL_ARRAY_BUFFER, channelPositions.size () * sizeof (GLfloat),
	                  channelPositions.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelTexcoord);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelTexcoord);
	    glBufferData (GL_ARRAY_BUFFER, channelUv.size () * sizeof (GLfloat), channelUv.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelBlendIndices);
	    glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelBlendIndices);
	    glBufferData (GL_ARRAY_BUFFER, channelIndices.size () * sizeof (GLuint),
	                  channelIndices.data (), GL_STATIC_DRAW);
	    glGenBuffers (1, &m_puppetChannelIndices);
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_puppetChannelIndices);
	    glBufferData (GL_ELEMENT_ARRAY_BUFFER, channel.indices.size () * sizeof (GLushort),
	                  channel.indices.data (), GL_STATIC_DRAW);
	    m_puppetChannelIndexCount = static_cast<GLsizei> (channel.indices.size ());
	    const uint32_t highestIndex = std::accumulate (
	        channel.blendIndices.begin (), channel.blendIndices.end (), uint32_t (0),
	        [] (uint32_t value, const std::array<uint32_t, 4>& lanes) { return std::max (value, lanes[0]); });
	    if (channel.blendRowCount == 0 || channel.blendRowCount > 4 ||
	        highestIndex >= channel.blendRowCount * 4)
	        throw std::runtime_error ("Puppet channel index exceeds native 16-float map");
	    m_puppetBlendMap.assign (channel.blendRowCount, glm::vec4 (0.0f));
	    const auto& combos = m_puppetChannelMaterial->passes.front ()->combos;
	    const auto rows = combos.find ("BLENDROWCOUNT");
	    if (rows == combos.end () || rows->second != int (m_puppetBlendMap.size ()))
	        throw std::runtime_error ("Puppet channel BLENDROWCOUNT mismatch");
	    const uint32_t targetWidth = m_texture->getRealWidth ();
	    const uint32_t targetHeight = m_texture->getRealHeight ();
	    if (targetWidth == 0 || targetHeight == 0)
		throw std::runtime_error ("Puppet channel target has zero dimensions");
	    m_puppetChannelProjection = glm::ortho (
		0.0f, float (targetWidth), float (targetHeight), 0.0f, -1000.0f, 1000.0f);
	    m_puppetChannelProjectionInverse = glm::inverse (m_puppetChannelProjection);
	    if (m_puppetChannelOffscreen) {
		m_puppetChannelFBO = std::make_shared<CFBO> (
		    "_rt_imageLayerAlbedo_" + std::to_string (getId ()), TextureFormat_ARGB8888,
		    m_texture->getFlags (), 1.0f, targetWidth, targetHeight, targetWidth, targetHeight);
	    }
	}
	sLog.out (
	    "Loaded puppet mesh ", *this->getImage ().model->puppet, " version=", mesh.version,
	    " vertices=", mesh.positions.size (), " indices=", this->m_puppetIndexCount
	);

	return true;
    } catch (const std::exception& ex) {
	sLog.error ("Could not load puppet mesh ", *this->getImage ().model->puppet, ": ", ex.what ());
	for (GLuint* handle : {&m_puppetChannelPosition, &m_puppetChannelTexcoord,
	                       &m_puppetChannelBlendIndices, &m_puppetChannelIndices}) {
	    if (*handle != GL_NONE) {
		glDeleteBuffers (1, handle);
		*handle = GL_NONE;
	    }
	}
	m_puppetChannelMesh.reset ();
	m_puppetChannelMaterial.reset ();
	m_puppetChannelFBO.reset ();
	m_puppetChannelOffscreen = false;
	m_puppetMesh.reset ();
	m_puppetSkeleton.reset ();
	m_puppetAnimation.reset ();
	m_puppetCurrentGlobals.clear ();
	m_puppetPoseFrame = UINT32_MAX;
	m_puppetBlendMap.clear ();
	return false;
    }
}

void CImage::updatePuppetAnimation () {
    if (!m_puppetMesh || !m_puppetSkeleton || !m_puppetAnimation || m_image.animationLayers.empty ()) return;
    try {
	auto pose = m_puppetReferencePose;
	// Native keeps puppet+0x350 channel values across frames; unlike other
	// per-frame streams, 1401fdf90 does not clear this array before layers.
	auto blendMap = m_puppetBlendMap;
	bool hasContributingLayer = false;
	const float delta = std::max (0.0f, getScene ().getDeltaTime ());
	for (size_t layerIndex = 0; layerIndex < m_image.animationLayers.size (); ++layerIndex) {
	    const auto& layer = *m_image.animationLayers[layerIndex];
	    if (!layer.visible->value->getBool ()) continue;
	    const int selectedId = layer.animation->value->getInt ();
	    const auto clip = std::find_if (m_puppetAnimation->clips.begin (), m_puppetAnimation->clips.end (),
	                                   [selectedId] (const PuppetClipHeader& candidate) {
	                                       return candidate.rawId == uint64_t (selectedId);
	                                   });
	    if (clip == m_puppetAnimation->clips.end ()) continue;
	    hasContributingLayer = true;
	    if (m_puppetLayerClipIds[layerIndex] != clip->rawId) {
		m_puppetLayerClipIds[layerIndex] = clip->rawId;
		m_puppetLayerStates[layerIndex] = {};
		m_puppetLayerBlendInActive[layerIndex] = layer.blendIn;
	    }
	    puppetAdvancePlayback (*clip, m_puppetLayerStates[layerIndex], delta,
	                           layer.rate->value->getFloat ());
	    const auto frames = puppetSelectFrames (*clip, m_puppetLayerStates[layerIndex].time);
	    bool blendInActive = m_puppetLayerBlendInActive[layerIndex];
	    const float weight = puppetEffectiveLayerWeight (
		*clip, m_puppetLayerStates[layerIndex], layer.blend->value->getFloat (),
		layer.blendTime, blendInActive, layer.blendOut);
	    m_puppetLayerBlendInActive[layerIndex] = blendInActive;
	    pose = puppetApplyClipLayer (*m_puppetSkeleton, *clip, pose, m_puppetReferencePose,
	                                 frames, weight, layer.additive);
	    // Native 1401fdf90 writes clip+0xd8 scalar tracks into the puppet's
	    // +0x350 blend map. 140207740 uploads those rows for the channel mesh.
	    const size_t channels = std::min (clip->extraScalarTracks.size (), blendMap.size () * 4);
	    for (size_t channel = 0; channel < channels; ++channel) {
		const float sampled = puppetScalarAtFrames (clip->extraScalarTracks[channel], frames);
		float& value = blendMap[channel / 4][channel % 4];
		value = puppetApplyScalarLayer (value, sampled, weight, layer.additive);
	    }
	}
	std::copy (blendMap.begin (), blendMap.end (), m_puppetBlendMap.begin ());
	std::vector<glm::mat4> localMatrices;
	if (hasContributingLayer) {
	    localMatrices = puppetPoseMatrices (pose);
	} else {
	    localMatrices = puppetUnanimatedLocalMatrices (*m_puppetSkeleton);
	}
	m_puppetCurrentGlobals = puppetGlobalMatrices (*m_puppetSkeleton, localMatrices);
	const auto palette = puppetSkinPalette (*m_puppetSkeleton, localMatrices, m_puppetInverseBind);
	for (size_t vertex = 0; vertex < m_puppetMesh->positions.size (); ++vertex) {
	    const glm::vec3 transformed = puppetSkinnedPosition (*m_puppetMesh, vertex, palette);
	    const size_t base = vertex * 3;
	    m_puppetRawPositions[base] = transformed.x;
	    m_puppetRawPositions[base + 1] = transformed.y;
	    m_puppetRawPositions[base + 2] = transformed.z;
	}
    } catch (const std::exception& error) {
	sLog.error ("Puppet animation disabled for ", *m_image.model->puppet, ": ", error.what ());
	for (size_t vertex = 0; vertex < m_puppetMesh->positions.size (); ++vertex) {
	    const auto& position = m_puppetMesh->positions[vertex];
	    const size_t base = vertex * 3;
	    m_puppetRawPositions[base] = position[0];
	    m_puppetRawPositions[base + 1] = position[1];
	    m_puppetRawPositions[base + 2] = position[2];
	}
	m_puppetAnimation.reset ();
	m_puppetCurrentGlobals.clear ();
	std::fill (m_puppetBlendMap.begin (), m_puppetBlendMap.end (), glm::vec4 (0.0f));
    }
}

void CImage::updatePuppetPositionBuffer (
    const glm::vec2& size, const ResolvedTransform& transform, float sceneWidth, float sceneHeight
) {
    if (this->m_puppetRawPositions.empty ()) {
	return;
    }

    std::vector<GLfloat> positions;
    std::vector<GLfloat> scenePositions;
    positions.reserve (this->m_puppetRawPositions.size ());
    scenePositions.reserve (this->m_puppetRawPositions.size ());
    glm::vec2 alignment {0.0f};
    if (m_alignment.find ("top") != std::string::npos) alignment.y = -size.y * 0.5f;
    else if (m_alignment.find ("bottom") != std::string::npos) alignment.y = size.y * 0.5f;
    if (m_alignment.find ("left") != std::string::npos) alignment.x = size.x * 0.5f;
    else if (m_alignment.find ("right") != std::string::npos) alignment.x = -size.x * 0.5f;
    for (size_t index = 0; index + 2 < this->m_puppetRawPositions.size (); index += 3) {
	positions.push_back (size.x / 2.0f + this->m_puppetRawPositions[index]);
	positions.push_back (size.y / 2.0f - this->m_puppetRawPositions[index + 1]);
	positions.push_back (this->m_puppetRawPositions[index + 2]);
	const glm::vec4 authored = transform.authoredMatrix * glm::vec4 (
	    this->m_puppetRawPositions[index] + alignment.x,
	    this->m_puppetRawPositions[index + 1] + alignment.y,
	    this->m_puppetRawPositions[index + 2], 1.0f);
	const glm::vec3 world = Wallpapers::scenePointForCamera (
	    glm::vec3 (authored), sceneWidth, sceneHeight,
	    getScene ().getCamera ().isOrthogonal ());
	scenePositions.push_back (world.x);
	scenePositions.push_back (world.y);
	scenePositions.push_back (world.z);
    }

    if (this->m_puppetSpacePosition == GL_NONE) {
	glGenBuffers (1, &this->m_puppetSpacePosition);
    }
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, positions.size () * sizeof (GLfloat), positions.data (), GL_DYNAMIC_DRAW);
    if (this->m_puppetSceneSpacePosition == GL_NONE)
	glGenBuffers (1, &this->m_puppetSceneSpacePosition);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetSceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, scenePositions.size () * sizeof (GLfloat), scenePositions.data (), GL_DYNAMIC_DRAW);



}

void CImage::setupPuppetGeometryCallback (Effects::CPass* pass, bool sceneSpace) const {
    pass->setGeometryCallback (
	[this, pass, sceneSpace] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");
	    const GLint texCoordFull = glGetAttribLocation (pass->getProgramID (), "a_TexCoordVec4");
	    const GLint blendIndices = glGetAttribLocation (pass->getProgramID (), "a_BlendIndices");

	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, sceneSpace ? this->m_puppetSceneSpacePosition : this->m_puppetSpacePosition);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }

	    if (texCoord >= 0) {
		glEnableVertexAttribArray (texCoord);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoord);
		glVertexAttribPointer (texCoord, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (texCoordFull >= 0 && this->m_puppetTexCoordFull != GL_NONE) {
		glEnableVertexAttribArray (texCoordFull);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetTexCoordFull);
		glVertexAttribPointer (texCoordFull, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (blendIndices >= 0 && this->m_puppetBlendIndices != GL_NONE) {
		glEnableVertexAttribArray (blendIndices);
		glBindBuffer (GL_ARRAY_BUFFER, this->m_puppetBlendIndices);
		glVertexAttribIPointer (blendIndices, 4, GL_UNSIGNED_INT, 0, nullptr);
	    }
	},
	[this] () {
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, this->m_puppetIndices);
	    glDrawElements (GL_TRIANGLES, this->m_puppetIndexCount, GL_UNSIGNED_SHORT, nullptr);
	},
	[this, pass] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texCoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoord");
	    const GLint texCoordFull = glGetAttribLocation (pass->getProgramID (), "a_TexCoordVec4");
	    const GLint blendIndices = glGetAttribLocation (pass->getProgramID (), "a_BlendIndices");

	    if (position >= 0) {
		glDisableVertexAttribArray (position);
	    }

	    if (texCoord >= 0) {
		glDisableVertexAttribArray (texCoord);
	    }
	    if (texCoordFull >= 0 && this->m_puppetTexCoordFull != GL_NONE)
		glDisableVertexAttribArray (texCoordFull);
	    if (blendIndices >= 0 && this->m_puppetBlendIndices != GL_NONE)
		glDisableVertexAttribArray (blendIndices);
	}
    );
}

void CImage::setupPuppetChannelGeometryCallback (Effects::CPass* pass, GLuint positionBuffer) const {
    pass->setGeometryCallback (
	[this, pass, positionBuffer] () {
	    const GLint position = glGetAttribLocation (pass->getProgramID (), "a_Position");
	    const GLint texcoord = glGetAttribLocation (pass->getProgramID (), "a_TexCoordVec4");
	    const GLint channel = glGetAttribLocation (pass->getProgramID (), "a_BlendIndices");
	    if (position >= 0) {
		glEnableVertexAttribArray (position);
		glBindBuffer (GL_ARRAY_BUFFER, positionBuffer);
		glVertexAttribPointer (position, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (texcoord >= 0) {
		glEnableVertexAttribArray (texcoord);
		glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelTexcoord);
		glVertexAttribPointer (texcoord, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
	    }
	    if (channel >= 0) {
		glEnableVertexAttribArray (channel);
		glBindBuffer (GL_ARRAY_BUFFER, m_puppetChannelBlendIndices);
		glVertexAttribIPointer (channel, 4, GL_UNSIGNED_INT, 0, nullptr);
	    }
	},
	[this] () {
	    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_puppetChannelIndices);
	    glDrawElements (GL_TRIANGLES, m_puppetChannelIndexCount, GL_UNSIGNED_SHORT, nullptr);
	},
	[pass] () {
	    for (const char* name : {"a_Position", "a_TexCoordVec4", "a_BlendIndices"}) {
		const GLint location = glGetAttribLocation (pass->getProgramID (), name);
		if (location >= 0) glDisableVertexAttribArray (location);
	    }
	});
}

void CImage::renderPuppetChannelPrepass () {
    if (!m_puppetChannelOffscreen || !m_puppetChannelFBO || !m_puppetChannelBasePass || !m_puppetChannelPass) return;
    m_puppetChannelFBO->clear (glm::vec4 (0.0f));
    auto* base = m_puppetChannelBasePass;
    base->setDestination (m_puppetChannelFBO);
    base->setInput (m_texture);
    base->setPreviousInput (nullptr);
    base->setPosition (m_passSpacePosition);
    base->setTexCoord (m_texcoordPass, 1.0f, 0.0f);
    base->setModelMatrix (&m_modelMatrix);
    base->setViewProjectionMatrix (&m_viewProjectionMatrix);
    base->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    base->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    base->render ();

    auto* channel = m_puppetChannelPass;
    channel->setDestination (m_puppetChannelFBO);
    channel->setInput (m_texture);
    channel->setPreviousInput (nullptr);
    channel->setPosition (m_puppetChannelPosition);
    channel->setTexCoord (m_puppetChannelTexcoord);
    channel->setModelMatrix (&m_modelMatrix);
    channel->setViewProjectionMatrix (&m_viewProjectionMatrix);
    channel->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    channel->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    setupPuppetChannelGeometryCallback (channel, m_puppetChannelPosition);
    channel->render ();
}

void CImage::renderPuppetChannelDirect (const std::shared_ptr<const CFBO>& target) {
    if (m_puppetChannelOffscreen || !m_puppetChannelPass) return;
    auto* channel = m_puppetChannelPass;
    channel->setDestination (target);
    channel->setInput (m_texture);
    channel->setPreviousInput (nullptr);
    channel->setPosition (m_puppetChannelPosition);
    channel->setTexCoord (m_puppetChannelTexcoord);
    channel->setModelMatrix (&m_modelMatrix);
    channel->setViewProjectionMatrix (&m_viewProjectionMatrix);
    channel->setModelViewProjectionMatrix (&m_puppetChannelProjection);
    channel->setModelViewProjectionMatrixInverse (&m_puppetChannelProjectionInverse);
    setupPuppetChannelGeometryCallback (channel, m_puppetChannelPosition);
    channel->render ();
}

void CImage::setup () {
    // do not double-init stuff, that's bad!
    if (this->m_initialized) {
	return;
    }
    m_hasCompositeConsumerAtSetup = hasCompositeConsumer ();
    m_prelightingPasses.clear ();
    m_compositePresentationPass = nullptr;
    m_compositeStepEnds.clear ();
    m_compositeMainStepCount = 1;
    m_effectVisibilityAtSetup.clear ();
    m_effectVisibilityAtSetup.reserve (m_image.effects.size ());
    for (const auto& effect : m_image.effects)
	m_effectVisibilityAtSetup.push_back (effect->visible->value->getBool ());
    m_effectProviders.resize (m_image.effects.size ());
    m_effectProviderSizes.resize (m_image.effects.size ());
    this->m_effectActions.clear ();
    this->m_sizedEffectTargets.clear ();

    // TODO: CHECK ORDER OF THINGS, 2419444134'S ID 27 DEPENDS ON 104'S COMPOSITE_A WHEN OUR LAST RENDER IS ON
    // COMPOSITE_B
    if (this->m_image.model->passthrough) {
        // An effectless composition layer can still be an authored snapshot:
        // dependent images sample its _rt_imageLayerComposite_<id>_a target.
        // Ordinary effectless groups have no output to produce.
	if (this->m_image.effects.empty ()) {
	    const bool hasDependent = std::ranges::any_of (
		this->getScene ().getScene ().objects, [id = this->getId ()] (const auto& object) {
		    return std::ranges::find (object->dependencies, id) != object->dependencies.end ();
		});
	    if (!hasDependent && !m_hasCompositeConsumerAtSetup) return;
	}

	// Some have attempted to declare effects with visible set to false.
	bool allEffectsInvisible = true;
	for (const auto& cur : this->m_image.effects) {
	    if (cur->visible->value->getBool ()) {
		allEffectsInvisible = false;
		break;
	    }
	}

	if (!this->m_image.effects.empty () && allEffectsInvisible && !m_hasCompositeConsumerAtSetup) {
	    return;
	}
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;

    const bool ordinaryGeometry = !m_hasPuppetMesh
        && !m_image.model->fullscreen && !m_image.model->passthrough;
    const bool offscreenBase = m_hasCompositeConsumerAtSetup
        || (!debug.baseOnly && std::ranges::any_of (m_image.effects, [] (const auto& effect) {
            return effect->visible->value->getBool ();
        }));

    // Native 140209540 builds a PRELIGHTING variant for a lit image whose
    // base draw precedes effects or a required composite presentation.
    for (const auto& cur : this->getImage ().model->material->passes) {
	std::optional<std::reference_wrapper<const ImageEffectPassOverride>> variant;
	const bool lit = std::ranges::any_of (std::array {"LIGHTING", "REFLECTION"}, [&] (const char* name) {
	    const auto value = cur->combos.find (name);
	    return value != cur->combos.end () && value->second != 0;
	});
	if (ordinaryGeometry && offscreenBase && lit) {
	    auto& override = m_materials.compatibilityOverrides.emplace_back (
	        std::make_unique<ImageEffectPassOverride> ());
	    override->combos.insert_or_assign ("PRELIGHTING", 1);
	    variant = std::cref (*override);
	}
	auto* pass = new CPass (*this, std::make_shared<FBOProvider> (this), *cur, variant, std::nullopt, std::nullopt);
	this->m_passes.push_back (pass);
	// Shader::getCombos exposes authored pass combos, while this variant is
	// compiled from the separate override map. Retain its native draw stage.
	if (variant) m_prelightingPasses.insert (pass);
    }
    m_basePassCount = m_passes.size ();
    m_compositeStepEnds.push_back ({m_basePassCount, m_resourceSwaps.size ()});

    // prepare the passes list
    if (!debug.baseOnly && !this->getImage ().effects.empty ()) {
	// generate the effects used by this material
	for (size_t effectIndex = 0; effectIndex < m_image.effects.size (); ++effectIndex) {
	    const auto& cur = m_image.effects[effectIndex];
	    if (std::find (debug.skipEffects.begin (), debug.skipEffects.end (), static_cast<int> (cur->id))
		!= debug.skipEffects.end ()) {
		continue;
	    }

	    // do not add non-visible effects, this might need some adjustements tho as some effects might not be
	    // visible but affect the output of the image...
	    if (!cur->visible->value->getBool ()) {
		continue;
	    }

	    auto& fboProvider = m_effectProviders[effectIndex];
	    if (!fboProvider) fboProvider = std::make_shared<FBOProvider> (this);
	    const glm::vec2 targetSize = getCompositeTargetSize ();
	    const bool targetSizeChanged = m_effectProviderSizes[effectIndex] != targetSize;

	    // create all the fbos for this effect
	    for (const auto& fbo : cur->effect->fbos) {
		if (targetSizeChanged || !fboProvider->find (fbo->name))
		    fboProvider->create (*fbo, this->m_texture->getFlags (), targetSize);
		m_sizedEffectTargets.push_back ({fbo.get (), fboProvider, effectIndex});
	    }
	    m_effectProviderSizes[effectIndex] = targetSize;
	    if (!cur->effect->clearFunctions.empty ())
		this->m_effectActions.push_back ({cur->effect.get (), fboProvider});

	    // TODO: MAKE USE OF ZIP OPERATOR IN BOOST? WAY OVERKILL JUST FOR THIS...

	    auto curEffect = cur->effect->passes.begin ();
	    auto endEffect = cur->effect->passes.end ();
	    auto curOverride = cur->passOverrides.begin ();
	    auto endOverride = cur->passOverrides.end ();
	    std::vector<ImageCompositeStepEnd> descriptorEnds;

	    for (; curEffect != endEffect; ++curEffect) {
		descriptorEnds.push_back ({m_passes.size (), m_resourceSwaps.size ()});
		if (!(*curEffect)->material.has_value ()) {
		    if (!(*curEffect)->command.has_value ()) {
			sLog.error ("Pass without material and command not supported");
			continue;
		    }

		    if (!(*curEffect)->source.has_value ()) {
			sLog.error ("Pass without material and source not supported");
			continue;
		    }

		    if (!(*curEffect)->target.has_value ()) {
			sLog.error ("Pass without material and target not supported");
			continue;
		    }

		    if ((*curEffect)->command != Command_Copy) {
			this->m_resourceSwaps.push_back ({this->m_passes.size (), fboProvider,
			                                  *(*curEffect)->source, *(*curEffect)->target});
			descriptorEnds.back ().swaps = m_resourceSwaps.size ();
			continue;
		    }

		    auto virtualPass
			= std::make_unique<MaterialPass> (MaterialPass { .blending = BlendingMode_Normal,
									 .cullmode = CullingMode_Disable,
									 .depthtest = DepthtestMode_Disabled,
									 .depthwrite = DepthwriteMode_Disabled,
									 .shader = "commands/copy",
									 .textures = { { 0, *(*curEffect)->source } },
									 .combos = {},
									 .constants = {} });

		    const auto& config = *this->m_virtualPassess.emplace_back (std::move (virtualPass));

		    // build a pass for a copy shader
		    this->m_passes.push_back (new CPass (
			*this, fboProvider, config, std::nullopt, std::nullopt, (*curEffect)->target.value ()
		    ));
		} else {
		    for (auto& pass : (*curEffect)->material.value ()->passes) {
			const auto override = curOverride != endOverride
			    ? **curOverride
			    : std::optional<std::reference_wrapper<const ImageEffectPassOverride>> (std::nullopt);
			const auto target = (*curEffect)->target.has_value ()
			    ? *(*curEffect)->target
			    : std::optional<std::reference_wrapper<std::string>> (std::nullopt);

			this->m_passes.push_back (
			    new CPass (*this, fboProvider, *pass, override, (*curEffect)->binds, target)
			);
		    }

		    if (curOverride != endOverride) {
			++curOverride;
		    }
		}
		descriptorEnds.back () = {m_passes.size (), m_resourceSwaps.size ()};
	    }
	    const auto boundaries = imageEffectCompositeStepEnds (*cur->effect, descriptorEnds);
	    m_compositeStepEnds.insert (m_compositeStepEnds.end (), boundaries.begin (), boundaries.end ());
	    m_compositeMainStepCount += imageEffectMainStepCount (*cur->effect);
	}
    }

    // extra render pass if there's any blending to be done
    if (!debug.baseOnly && this->m_image.colorBlendMode->value->getInt () > 0) {
	this->m_materials.colorBlending.material
	    = MaterialParser::load (this->getScene ().getScene ().project, "materials/util/effectpassthrough.json");
	this->m_materials.colorBlending.override = std::make_unique<ImageEffectPassOverride> (ImageEffectPassOverride {
            .id = -1,
            .combos = {
                {"BLENDMODE", this->m_image.colorBlendMode->value->getInt()},
            },
            .constants = {},
            .textures = {},
        });

	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this), **this->m_materials.colorBlending.material->passes.begin (),
	    *this->m_materials.colorBlending.override, std::nullopt, std::nullopt
	));
	m_compositeStepEnds.push_back ({m_passes.size (), m_resourceSwaps.size ()});
	++m_compositeMainStepCount;
    }

    // Native 1401e8aa0 runs effect steps on quads, then 140208c80 draws the
    // processed texture on the puppet through a separate passthrough material.
    // Keep the authored effect shader off the mesh: its UVs may address an
    // intermediate target rather than the puppet texture atlas.
    if (this->m_hasPuppetMesh &&
	(this->m_passes.size () > m_basePassCount || m_puppetChannelMesh)) {
	this->m_materials.compatibilityMaterials.emplace_back (
	    MaterialParser::load (this->getScene ().getScene ().project,
	                          "materials/util/effectpassthrough.json"));
	this->m_passes.push_back (new CPass (
	    *this, std::make_shared<FBOProvider> (this),
	    **this->m_materials.compatibilityMaterials.back ()->passes.begin (),
	    std::nullopt, std::nullopt, std::nullopt));
    }

    // Native 1401e8aa0 runs every effect step offscreen for required images
    // (flag 0x10), then presents the completed texture with device color unity.
    // The plain passthrough shader preserves straight RGBA and applies neither
    // the image color nor alpha a second time. Ordinary lit base draws use
    // the separate native PRELIGHTING source-pixel and alternate matrix route.
    // Puppet and lit fullscreen/passthrough geometry retain their own routes.
    const bool unlit = std::ranges::none_of (m_passes, [] (const CPass* pass) {
        const auto& combos = pass->getShader ()->getCombos ();
        return std::ranges::any_of (std::array {"LIGHTING", "REFLECTION"}, [&] (const char* name) {
            const auto value = combos.find (name);
            return value != combos.end () && value->second != 0;
        });
    });
    if (m_hasCompositeConsumerAtSetup && !m_hasPuppetMesh && (unlit || ordinaryGeometry) && !m_passes.empty ()) {
        auto material = std::make_unique<MaterialPass> (MaterialPass {
            .blending = BlendingMode_Normal,
            .cullmode = CullingMode_Disable,
            .depthtest = DepthtestMode_Disabled,
            .depthwrite = DepthwriteMode_Disabled,
            .shader = "passthrough",
            .textures = {}, .combos = {{"TRANSFORM", 1}}, .constants = {},
        });
        const auto& retained = *m_virtualPassess.emplace_back (std::move (material));
        m_compositePresentationPass = new CPass (
            *this, std::make_shared<FBOProvider> (this), retained,
            std::nullopt, std::nullopt, std::nullopt);
        m_passes.push_back (m_compositePresentationPass);
    }

    // if there's more than one pass the blendmode has to be moved from the beginning to the end
    if (this->m_passes.size () > 1) {
	const auto first = this->m_passes.begin ();
	const auto last = this->m_passes.rbegin ();

	(*last)->setBlendingMode ((*first)->getBlendingMode ());
	(*first)->setBlendingMode (BlendingMode_Normal);
    }

    if (!m_puppetBlendMap.empty ()) {
	for (auto* pass : m_passes) {
	    if (pass->getPass ().shader.find ("puppettexturechannels") != std::string::npos) {
		const auto rows = pass->getPass ().combos.find ("BLENDROWCOUNT");
		if (rows == pass->getPass ().combos.end () ||
		    rows->second != int (m_puppetBlendMap.size ()))
		    throw std::runtime_error ("Puppet BLENDROWCOUNT does not match decoded channel rows");
	    }
	    pass->addUniform ("g_BlendMap", m_puppetBlendMap.data (), int (m_puppetBlendMap.size ()));
	}
    }

    if (m_puppetChannelMesh) {
	if (m_puppetChannelOffscreen) {
	    m_puppetChannelBaseMaterial = std::make_unique<MaterialPass> (MaterialPass {
		.blending = BlendingMode_Normal,
		.cullmode = CullingMode_Disable,
		.depthtest = DepthtestMode_Disabled,
		.depthwrite = DepthwriteMode_Disabled,
		.shader = "passthrough",
		.textures = {}, .combos = {}, .constants = {}
	    });
	    m_puppetChannelBasePass = new CPass (
		*this, std::make_shared<FBOProvider> (this), *m_puppetChannelBaseMaterial,
		std::nullopt, std::nullopt, std::nullopt);
	}
	m_puppetChannelPass = new CPass (
	    *this, std::make_shared<FBOProvider> (this),
	    **m_puppetChannelMaterial->passes.begin (), std::nullopt, std::nullopt, std::nullopt);
	m_puppetChannelPass->addUniform (
	    "g_BlendMap", m_puppetBlendMap.data (), int (m_puppetBlendMap.size ()));
	// Native 140207740 leaves the prepass device color at unity while the
	// `_rt_imageLayerAlbedo_` route is active. The final image material applies
	// the authored color once after sampling this texture.
	if (m_puppetChannelOffscreen)
	    m_puppetChannelPass->addUniform ("g_Color4", &m_puppetPrepassColor);
    }

    CRenderable::setup ();

    for (auto* pass : m_passes) {
        const auto& combos = pass->getShader ()->getCombos ();
        const auto enabled = [&] (const char* name) {
            const auto entry = combos.find (name);
            return entry != combos.end () && entry->second != 0;
        };
        if (enabled ("LIGHTING") || enabled ("REFLECTION"))
            pass->addUniform ("g_EyePosition", &m_lightingEye);
        if (m_prelightingPasses.contains (pass)) {
            pass->addUniform ("g_AltModelMatrix", &m_prelightingWorld);
            pass->addUniform ("g_AltNormalModelMatrix", &m_prelightingNormal);
            pass->addUniform ("g_AltViewProjectionMatrix", &m_lightingViewProjection);
        }
    }

    this->m_initialized = true;
}

void CImage::setupPasses (const std::function<void (std::shared_ptr<const CFBO>)>& renderChildren) {
    // do a pass on everything and setup proper inputs and values
    this->m_currentMainFBO = this->m_mainFBO;
    this->m_currentSubFBO = this->m_subFBO;
    if (m_compositePresentationPass) {
        // Native counts main steps at compose and visible effect boundaries,
        // including a step whose material draws into an explicit target.
        if (m_compositeMainStepCount % 2 == 0)
            std::swap (m_currentMainFBO, m_currentSubFBO);
    }
    std::shared_ptr<const CFBO> drawTo = this->m_currentMainFBO;
    std::shared_ptr<const TextureProvider> asInput = this->getImage ().model->passthrough
        ? this->getScene ().getActiveRenderTarget ()
        : (m_puppetChannelFBO ? std::static_pointer_cast<const TextureProvider> (m_puppetChannelFBO)
                              : this->getTexture ());
    GLuint texcoord = this->getTexCoordCopy ();

    auto cur = this->m_passes.begin ();
    auto end = this->m_passes.end ();
    bool first = true;
    bool inTargetEffectSequence = false;
    std::shared_ptr<const TextureProvider> effectInput = nullptr;
    auto nextSwap = this->m_resourceSwaps.begin ();
    auto nextCompositeStep = m_compositeStepEnds.begin ();
    const auto advanceCompositeSteps = [&] (size_t completedDraws) {
        if (!m_compositePresentationPass) return;
        const auto completedSwaps = static_cast<size_t> (
            std::distance (m_resourceSwaps.begin (), nextSwap));
        while (nextCompositeStep != m_compositeStepEnds.end ()
               && nextCompositeStep->draws <= completedDraws
               && nextCompositeStep->swaps <= completedSwaps) {
            pinpongFramebuffer (&drawTo, &asInput);
            inTargetEffectSequence = false;
            effectInput = nullptr;
            ++nextCompositeStep;
        }
    };
    size_t passIndex = 0;

    for (; cur != end; ++cur, ++passIndex) {
	if (renderChildren && passIndex == m_basePassCount) {
	    const auto childTarget = std::dynamic_pointer_cast<const CFBO> (asInput);
	    if (childTarget) renderChildren (childTarget);
	}
	while (nextSwap != this->m_resourceSwaps.end ()
	       && nextSwap->beforePass == static_cast<size_t> (std::distance (this->m_passes.begin (), cur))) {
	    nextSwap->provider->swap (nextSwap->source, nextSwap->target);
	    ++nextSwap;
	}
	advanceCompositeSteps (passIndex);
	// TODO: PROPERLY CHECK EFFECT'S VISIBILITY AND TAKE IT INTO ACCOUNT
	// TODO: THIS REQUIRES ON-THE-FLY EVALUATION OF EFFECTS VISIBILITY TO FIGURE OUT
	// TODO: WHICH ONE IS THE LAST + A FEW OTHER THINGS
	Effects::CPass* pass = *cur;
	if (pass == m_compositePresentationPass && !shouldRenderFinalPass (true)) continue;
	if (pass == m_compositePresentationPass) {
	    // Boundary advancement selects the retained main texture even when
	    // the last material drew into an explicit scratch target.
	    inTargetEffectSequence = false;
	    effectInput = nullptr;
	}
	if (this->m_hasPuppetMesh)
	    pass->setGeometryCallback ({}, {}, {});
	std::shared_ptr<const CFBO> prevDrawTo = drawTo;
	bool writesToTarget = false;
	const bool isFirstPass = first;
	GLuint spacePosition = isFirstPass ? this->getCopySpacePosition () : this->getPassSpacePosition ();
	const glm::mat4* projection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	const glm::mat4* inverseProjection
	    = (isFirstPass) ? &this->m_modelViewProjectionCopyInverse : &this->m_modelViewProjectionPassInverse;
	first = false;

	pass->setModelMatrix (&this->m_modelMatrix);
	pass->setNormalModelMatrix (nullptr);
	pass->setViewProjectionMatrix (&this->m_viewProjectionMatrix);

	writesToTarget = this->configurePassTarget (pass, drawTo, asInput, effectInput, inTargetEffectSequence);
	// determine if it's the last element in the list as this is a screen-copy-like process
	// TODO: PROPERLY CHECK IF THIS IS ALL THAT'S NEEDED
	if (!writesToTarget && this->shouldRenderFinalPass (std::next (cur) == end)) {
	    // TODO: PROPERLY CHECK EFFECT'S VISIBILITY AND TAKE IT INTO ACCOUNT
	    spacePosition = this->getSceneSpacePosition ();
	    drawTo = this->getScene ().getActiveRenderTarget ();
	    projection = &this->m_modelViewProjectionScreen;
	    inverseProjection = &this->m_modelViewProjectionScreenInverse;
	    // Fullscreen final effect passes cover the target in clip space. A
	    // multi-pass passthrough layer uses the pass quad here; the native final
	    // blur composite does the same. Its single first/final copy still uses
	    // the separate copy geometry and projection route.
	    if (this->getImage ().model->fullscreen
	        && (!this->getImage ().model->passthrough || !isFirstPass
                    || (!m_composesChildren && !getScene ().isChildCompositionScope ()))) {
	        spacePosition = isFirstPass ? this->getCopySpacePosition () : this->getPassSpacePosition ();
	        projection = isFirstPass ? &this->m_modelViewProjectionCopy : &this->m_modelViewProjectionPass;
	        inverseProjection = isFirstPass ? &this->m_modelViewProjectionCopyInverse
	                                        : &this->m_modelViewProjectionPassInverse;
	    }
	}
	// Native 1401ebf60 draws intermediate effect steps on a quad; 140208c80
	// draws their final texture through the puppet mesh. A single base pass is
	// both first and final, so it also takes this scene-space mesh route.
	if (this->m_hasPuppetMesh && drawTo == this->getScene ().getActiveRenderTarget ()) {
	    this->setupPuppetGeometryCallback (pass, true);
	    spacePosition = this->m_puppetSceneSpacePosition;
	}

        const auto& combos = pass->getShader ()->getCombos ();
        const auto enabled = [&] (const char* name) {
            const auto entry = combos.find (name);
            return entry != combos.end () && entry->second != 0;
        };
        if (passIndex < m_basePassCount && m_prelightingPasses.contains (pass)
            && !m_hasPuppetMesh && !getImage ().model->fullscreen && !getImage ().model->passthrough) {
            // Native 140207b50 rasterizes centered source pixels into the
            // local image target while its alternate matrices retain the
            // authored lighting coordinates. This must not use the scene
            // projection for gl_Position, nor a logical-size quad for a
            // shared material's world-space lighting.
            spacePosition = m_prelightingLocalPosition;
            projection = &m_prelightingProjection;
            inverseProjection = &m_prelightingProjectionInverse;
        } else if (projection == &m_modelViewProjectionScreen && !m_hasPuppetMesh
            && !getImage ().model->fullscreen && !getImage ().model->passthrough
            && (enabled ("LIGHTING") || enabled ("REFLECTION"))) {
            // Native 1401e8aa0 pushes the authored node model before final
            // image draw 140208670; 1401ede30 supplies centered local vertices.
            // Lighting consumes model and VP separately, so a local-FBO ortho
            // matrix cannot stand in for the scene model or be applied to
            // already transformed scene-space vertices.
            spacePosition = m_lightingLocalPosition;
            pass->setModelMatrix (&m_lightingWorld);
            pass->setNormalModelMatrix (&m_lightingNormal);
            pass->setViewProjectionMatrix (&m_lightingViewProjection);
            projection = &m_lightingMvp;
            inverseProjection = &m_lightingMvpInverse;
        }

	pass->setDestination (drawTo);
	pass->setInput (asInput);
	pass->setTexture0Override (
	    m_puppetChannelFBO && passIndex < m_basePassCount
	        ? std::static_pointer_cast<const TextureProvider> (m_puppetChannelFBO)
	        : std::shared_ptr<const TextureProvider> {});
	pass->setPreviousInput (inTargetEffectSequence ? effectInput : nullptr);
	pass->setPosition (spacePosition);
	// Scene-space perspective finals need the intermediate texture's basis
	// converted alongside the camera presentation reflection. Fullscreen copy
	// and effect quads use an identity clip-space projection: native 1401e8aa0
	// and 1402066a0 preserve framebuffer orientation there, so reversing V
	// would mirror the already projected scene a second time.
	if (!isFirstPass
	    && (projection == &m_modelViewProjectionScreen || projection == &m_lightingMvp)
	    && drawTo == this->getScene ().getActiveRenderTarget ()
	    && !this->getScene ().getCamera ().isOrthogonal ()
	    && !this->getScene ().isChildCompositionScope ())
	    texcoord = m_texcoordPassPresented;
	if (texcoord == m_texcoordPass)
	    pass->setTexCoord (texcoord, 1.0f, 0.0f);
	else if (texcoord == m_texcoordCopy)
	    pass->setTexCoord (texcoord, m_texcoordCopyTopV, m_texcoordCopyBottomV);
	else
	    pass->setTexCoord (texcoord);
	pass->setModelViewProjectionMatrix (projection);
	pass->setModelViewProjectionMatrixInverse (inverseProjection);
	if (std::next (cur) == end)
	    glColorMask (true, true, true, this->getScene ().isChildCompositionScope ());
	// Native passthrough layers with copybackground=false clear their base
	// target to transparent black instead of copying the prior scene color.
	// The children then populate that target before the effect passes.
	if (isFirstPass && getImage ().model->passthrough
	    && !getImage ().copyBackground->value->getBool ())
	    drawTo->clear (glm::vec4 (0.0f));
	else {
	    if (isFirstPass && m_hasPuppetMesh &&
	        drawTo != this->getScene ().getActiveRenderTarget ())
		drawTo->clear (glm::vec4 (0.0f));
	    pass->render ();
	}
	// Native 140207b50 draws the selected channel mesh directly into the
	// image base target when material flags do not request an albedo prepass.
	// This occurs before the authored image effects are processed.
	if (m_puppetChannelMesh && !m_puppetChannelOffscreen && passIndex + 1 == m_basePassCount)
	    renderPuppetChannelDirect (drawTo);

	texcoord = this->getTexCoordPass ();

	if (m_compositePresentationPass) {
	    if (pass != m_compositePresentationPass) {
		if (writesToTarget) asInput = drawTo;
		drawTo = prevDrawTo;
		advanceCompositeSteps (passIndex + 1);
	    }
	} else if (writesToTarget) {
	    asInput = drawTo;
	    drawTo = prevDrawTo;
	} else {
	    drawTo = prevDrawTo;
	    this->pinpongFramebuffer (&drawTo, &asInput);
	    inTargetEffectSequence = false;
	    effectInput = nullptr;
	}
    }
    while (nextSwap != this->m_resourceSwaps.end ()) {
	nextSwap->provider->swap (nextSwap->source, nextSwap->target);
	++nextSwap;
    }
    advanceCompositeSteps (m_passes.size ());
}

bool CImage::shouldRenderFinalPass (bool isLastPass) const {
    if (!isLastPass || !this->resolveTransform (this->getImage ()).visible
	|| (this->getImage ().model->passthrough && this->getImage ().effects.empty ())) {
	return false;
    }

    const auto& debug = this->getScene ().getContext ().getApp ().getContext ().settings.render.debug;
    return !(debug.noSolidFinal && this->getImage ().model->solidlayer);
}

bool CImage::configurePassTarget (
    Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo, const std::shared_ptr<const TextureProvider>& asInput,
    std::shared_ptr<const TextureProvider>& effectInput, bool& inTargetEffectSequence
) {
    if (!pass->getTarget ().has_value ()) {
	return false;
    }

    const std::string target = pass->getTarget ().value ();
    std::shared_ptr<const CFBO> resolved = pass->getFBOProvider ()->find (target);
    if (resolved == nullptr) {
	resolved = this->getScene ().findFBO (target);
    }
    if (resolved == nullptr) {
	sLog.error (
	    "Pass target FBO '", target, "' could not be resolved for object ", pass->getRenderable ().getId (),
	    " shader=", pass->getPass ().shader
	);
	return false;
    }

    if (!inTargetEffectSequence) {
	effectInput = asInput;
	inTargetEffectSequence = true;
    }
    drawTo = resolved;
    return true;
}

void CImage::pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput) {
    // temporarily store FBOs used
    std::shared_ptr<const CFBO> currentMainFBO = this->m_currentMainFBO;
    std::shared_ptr<const CFBO> currentSubFBO = this->m_currentSubFBO;

    if (drawTo != nullptr) {
	*drawTo = currentSubFBO;
    }
    if (asInput != nullptr) {
	*asInput = currentMainFBO;
    }

    // swap the FBOs
    this->m_currentMainFBO = currentSubFBO;
    this->m_currentSubFBO = currentMainFBO;
}

void CImage::render () { this->renderWithChildren ({}); }

glm::vec2 CImage::getCompositeTargetSize () const {
    if (m_puppetChannelMesh)
        return glm::vec2 (m_texture->getRealWidth (), m_texture->getRealHeight ());
    if (m_image.model->fullscreen && m_image.model->passthrough && !m_composesChildren
        && !getScene ().isChildCompositionScope ()) {
        const glm::vec2 presentation = getScene ().getPresentationTextureSize ();
        if (presentation.x > 0.0f && presentation.y > 0.0f) return presentation;
    }
    return m_loadedTargetSize;
}

bool CImage::refreshSizeDependentTargets () {
    // Native 1402091e0/1401ea500 allocates a selected puppet channel's
	// composite at source texture dimensions, independent of authored layer
	// size. The final pass maps that texture through the skeletal mesh.
    const glm::vec2 size = getCompositeTargetSize ();
    if (size == m_targetBaseSize) return true;

    GLint hardwareLimit = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &hardwareLimit);
    const auto valid = [hardwareLimit] (float dimension) {
	return std::isfinite (dimension) && dimension > 0.0f
	    && dimension <= static_cast<float> (std::numeric_limits<GLsizei>::max ())
	    && (hardwareLimit <= 0 || dimension <= static_cast<float> (hardwareLimit));
    };
    if (!valid (size.x) || !valid (size.y)) {
	sLog.error ("Image ", getId (), " has invalid live size ", size.x, "x", size.y);
	return false;
    }

    const auto width = std::max (1u, static_cast<uint32_t> (size.x));
    const auto height = std::max (1u, static_cast<uint32_t> (size.y));
    try {
	m_mainFBO->resize (width, height, width, height);
	m_subFBO->resize (width, height, width, height);
	for (const auto& target : m_sizedEffectTargets) {
	    target.provider->create (*target.descriptor, m_texture->getFlags (), size);
	    m_effectProviderSizes[target.effectIndex] = size;
	}
    } catch (const std::exception& error) {
	sLog.error ("Image ", getId (), " could not resize live targets: ", error.what ());
	return false;
    }
    m_targetBaseSize = size;
    return true;
}

void CImage::refreshSourceDimensions () {
    if (!m_hasSourceTexture) return;
    const glm::vec2 source (m_texture->getRealWidth (), m_texture->getRealHeight ());
    if (source == m_loadedSourceSize) return;
    // Source loading/resizing is distinct from a logical layer.size write.
    // Root framebuffers can change extent with the output window; preserve
    // their target and autosize updates without rebuilding from script size.
    const auto dimensions = loadedImageDimensions (
        m_loadedLogicalSize, {.autosize = m_image.model->autosize,
               .fullscreen = m_image.model->fullscreen,
               .solidlayer = m_image.model->solidlayer,
               .passthrough = m_image.model->passthrough,
               .animated = m_texture->isAnimated (),
               .projectlayer = m_image.model->projectlayer}, source,
        {getScene ().getWidth (), getScene ().getHeight ()});
    m_loadedSourceSize = source;
    m_loadedTargetSize = dimensions.backing;
    m_loadedLogicalSize = dimensions.logical;
    m_size = dimensions.geometry;
    if (m_image.model->autosize || m_image.model->fullscreen)
        m_image.size->value->update (dimensions.logical, DynamicValue::Script);
}

bool CImage::hasCompositeConsumer () const {
    return std::ranges::any_of (
        getScene ().getObjectsByRenderOrder (), [id = getId ()] (const CObject* consumer) {
            return consumer->getId () != id
                && std::ranges::find (consumer->getObject ().dependencies, id)
                    != consumer->getObject ().dependencies.end ();
        });
}

void CImage::refreshEffectVisibility () {
    bool changed = m_hasCompositeConsumerAtSetup != hasCompositeConsumer ()
        || m_effectVisibilityAtSetup.size () != m_image.effects.size ();
    if (!changed) {
	for (size_t index = 0; index < m_image.effects.size (); ++index) {
	    if (m_effectVisibilityAtSetup[index] != m_image.effects[index]->visible->value->getBool ()) {
		changed = true;
		break;
	    }
	}
    }
    if (!changed) return;

    // Visibility changes alter the pass graph, its named targets and the
    // choice of the final scene pass. Rebuild from the same per-instance
    // DynamicValues; the script modules and source objects remain alive.
    m_prelightingPasses.clear ();
    for (auto* pass : m_passes) delete pass;
    m_passes.clear ();
    m_compositePresentationPass = nullptr;
    m_compositeStepEnds.clear ();
    m_compositeMainStepCount = 0;
    delete m_puppetChannelBasePass;
    delete m_puppetChannelPass;
    m_puppetChannelBasePass = nullptr;
    m_puppetChannelPass = nullptr;
    m_basePassCount = 0;
    m_resourceSwaps.clear ();
    m_effectActions.clear ();
    m_sizedEffectTargets.clear ();
    m_virtualPassess.clear ();
    m_materials.colorBlending.material.reset ();
    m_materials.colorBlending.override.reset ();
    m_materials.compatibilityMaterials.clear ();
    m_materials.compatibilityOverrides.clear ();
    m_initialized = false;
    setup ();
}

bool CImage::canComposeChildren () {
    refreshEffectVisibility ();
    return m_initialized && m_image.model->passthrough && m_basePassCount > 0
	&& m_passes.size () > m_basePassCount;
}

void CImage::renderWithChildren (const std::function<void (std::shared_ptr<const CFBO>)>& renderChildren) {
    m_composesChildren = static_cast<bool> (renderChildren);
    refreshEffectVisibility ();
    // do not try to render something that did not initialize successfully
    if (!this->m_initialized) {
	return;
    }
    refreshSourceDimensions ();
    // Native size writes change the logical getter, while the ordinary quad
    // and its targets retain the dimensions built at source load. An initial
    // zero axis builds a degenerate quad; a later logical zero does not hide
    // an already built positive quad.
    if (!m_hasPuppetMesh && (m_size.x == 0.0f || m_size.y == 0.0f)) return;

    // A passthrough layer with no runnable effect output has no final
    // composition pass. Leave its descendants in the scene's flat draw path
    // rather than treating the base pass as a scene-wide clear/copy.
    if (this->m_image.model->passthrough && !this->m_image.effects.empty ()
	&& m_passes.size () <= m_basePassCount) return;

    if (!this->resolveTransform (this->getImage ()).visible) {
        // Native 140186c90 marks referenced images with 0x1010; 1401ea2d0
        // keeps those images drawable even while hidden. Their intermediate
        // textures remain inputs to dependent layers, while the existing
        // shouldRenderFinalPass visibility gate suppresses presentation.
        const bool requiredComposite = std::ranges::any_of (
            getScene ().getObjectsByRenderOrder (), [id = getId ()] (const CObject* consumer) {
                return consumer->getId () != id
                    && std::ranges::find (consumer->getObject ().dependencies, id)
                        != consumer->getObject ().dependencies.end ();
            });
        if (!requiredComposite) return;
    }

    if (!refreshSizeDependentTargets ()) return;

    glColorMask (true, true, true, true);

    // Always update screen transform (handles rotation + parallax dynamically)
    this->updateScreenSpacePosition ();
    // Passes retain the address of g_Color4 after setup. Keep its alpha in sync
    // with the separate, scriptable image alpha on every rendered frame.
    this->m_effectiveColor4 = imageDeviceColor (
        this->m_image.color->value->getVec4 (), this->m_image.alpha->value->getFloat (),
        this->m_image.brightness->value->getFloat (), this->getScene ().isHdrPostprocessingActive ());

#if !NDEBUG
    std::string str = "Image ";

    if (this->getScene ().getScene ().camera.bloom.enabled->value->getBool () && this->getId () == -1) {
	str += "bloom";
    } else {
	str += this->getImage ().name + " (" + std::to_string (this->getId ()) + ", "
	    + this->getImage ().model->material->filename + ")";
    }

    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */

    this->renderPuppetChannelPrepass ();
    this->setupPasses (renderChildren);

#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}

const float& CImage::getBrightness () const { return this->m_image.brightness->value->getFloat (); }

const float& CImage::getUserAlpha () const { return this->m_image.alpha->value->getFloat (); }

const float& CImage::getAlpha () const { return this->m_image.alpha->value->getFloat (); }

const glm::vec3& CImage::getColor () const { return this->m_image.color->value->getVec3 (); }

const glm::vec4& CImage::getColor4 () const { return this->m_effectiveColor4; }

const glm::vec3& CImage::getCompositeColor () const { return this->m_image.color->value->getVec3 (); }

glm::vec2 CImage::resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const {
    glm::vec2 size = m_size;

    if (this->getImage ().model->fullscreen && m_hasSourceTexture) {
	size = { sceneWidth, sceneHeight };
	origin = { sceneWidth / 2.0f, sceneHeight / 2.0f, 0.0f };
    }

    return size;
}

void CImage::updateScenePosition (
    const ResolvedTransform& transform, const glm::vec2& size, float sceneWidth, float sceneHeight
) {
    glm::vec2 alignment {0.0f};
    if (m_alignment.find ("top") != std::string::npos) alignment.y = -size.y * 0.5f;
    else if (m_alignment.find ("bottom") != std::string::npos) alignment.y = size.y * 0.5f;
    if (m_alignment.find ("left") != std::string::npos) alignment.x = size.x * 0.5f;
    else if (m_alignment.find ("right") != std::string::npos) alignment.x = -size.x * 0.5f;

    m_lightingWorld = glm::translate (transform.authoredMatrix, glm::vec3 (alignment, 0.0f));
    m_lightingNormal = modelNormalMatrix (m_lightingWorld).value_or (glm::mat3 (1.0f));
    const glm::vec2 sourceSize = m_hasSourceTexture
        ? glm::vec2 (m_texture->getRealWidth (), m_texture->getRealHeight ())
        : imageBackingDimensions (size);
    const auto prelighting = imagePrelightingBasis (
        m_lightingWorld, size, sourceSize, m_image.model->instanced);
    m_prelightingWorld = prelighting.world;
    m_prelightingNormal = modelNormalMatrix (m_prelightingWorld).value_or (glm::mat3 (1.0f));
    m_prelightingProjection = prelighting.targetProjection;
    m_prelightingProjectionInverse = glm::inverse (m_prelightingProjection);

    const glm::vec2 half = size * 0.5f;
    const glm::vec2 corners[4] = {
        alignment + glm::vec2 (-half.x, -half.y),
        alignment + glm::vec2 (-half.x, half.y),
        alignment + glm::vec2 (half.x, -half.y),
        alignment + glm::vec2 (half.x, half.y),
    };
    for (int i = 0; i < 4; ++i) {
	// Perspective presentation is corrected once in the scene projection;
	// keep the authored quad corners and UV order in both camera modes.
	const glm::vec4 authored = transform.authoredMatrix * glm::vec4 (corners[i], 0.0f, 1.0f);
        m_sceneQuad[i] = Wallpapers::scenePointForCamera (
            glm::vec3 (authored), sceneWidth, sceneHeight,
            getScene ().getCamera ().isOrthogonal ());
    }
    const glm::vec4 center = transform.authoredMatrix * glm::vec4 (alignment, 0.0f, 1.0f);
    m_sceneCenter = Wallpapers::scenePointForCamera (
        glm::vec3 (center), sceneWidth, sceneHeight,
        getScene ().getCamera ().isOrthogonal ());
    m_pos = {m_sceneQuad[0].x, m_sceneQuad[0].y, m_sceneQuad[0].x, m_sceneQuad[0].y};
    for (int i = 1; i < 4; ++i) {
        m_pos.x = std::min (m_pos.x, m_sceneQuad[i].x);
        m_pos.y = std::max (m_pos.y, m_sceneQuad[i].y);
        m_pos.z = std::max (m_pos.z, m_sceneQuad[i].x);
        m_pos.w = std::min (m_pos.w, m_sceneQuad[i].y);
    }
}

void CImage::uploadGeometryBuffers (const glm::vec2& size) {
    const glm::vec2 half = size * 0.5f;
    const GLfloat localPosition[] {
        -half.x, -half.y, 0, -half.x, half.y, 0, half.x, -half.y, 0,
        half.x, -half.y, 0, -half.x, half.y, 0, half.x, half.y, 0,
    };
    if (m_lightingLocalPosition == GL_NONE) glGenBuffers (1, &m_lightingLocalPosition);
    glBindBuffer (GL_ARRAY_BUFFER, m_lightingLocalPosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (localPosition), localPosition, GL_DYNAMIC_DRAW);
    const glm::vec2 sourceHalf = (m_hasSourceTexture
        ? glm::vec2 (m_texture->getRealWidth (), m_texture->getRealHeight ())
        : imageBackingDimensions (size)) * 0.5f;
    const GLfloat prelightingPosition[] {
        -sourceHalf.x, -sourceHalf.y, 0, -sourceHalf.x, sourceHalf.y, 0, sourceHalf.x, -sourceHalf.y, 0,
        sourceHalf.x, -sourceHalf.y, 0, -sourceHalf.x, sourceHalf.y, 0, sourceHalf.x, sourceHalf.y, 0,
    };
    if (m_prelightingLocalPosition == GL_NONE) glGenBuffers (1, &m_prelightingLocalPosition);
    glBindBuffer (GL_ARRAY_BUFFER, m_prelightingLocalPosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (prelightingPosition), prelightingPosition, GL_DYNAMIC_DRAW);
    GLfloat sceneSpacePosition[] = {
        m_sceneQuad[0].x, m_sceneQuad[0].y, m_sceneQuad[0].z,
        m_sceneQuad[1].x, m_sceneQuad[1].y, m_sceneQuad[1].z,
        m_sceneQuad[2].x, m_sceneQuad[2].y, m_sceneQuad[2].z,
        m_sceneQuad[2].x, m_sceneQuad[2].y, m_sceneQuad[2].z,
        m_sceneQuad[1].x, m_sceneQuad[1].y, m_sceneQuad[1].z,
        m_sceneQuad[3].x, m_sceneQuad[3].y, m_sceneQuad[3].z,
    };

    float width = 1.0f;
    float height = 1.0f;
    if (this->getTexture () != nullptr && !this->getTexture ()->isAnimated ()
	&& (this->getTexture ()->getTextureWidth (0) != this->getTexture ()->getRealWidth ()
	    || this->getTexture ()->getTextureHeight (0) != this->getTexture ()->getRealHeight ())) {
	width = static_cast<float> (this->getTexture ()->getRealWidth ())
	    / static_cast<float> (this->getTexture ()->getTextureWidth (0));
	height = static_cast<float> (this->getTexture ()->getRealHeight ())
	    / static_cast<float> (this->getTexture ()->getTextureHeight (0));
    }

    float x = 0.0f;
    float y = 0.0f;
    GLfloat realWidth = size.x;
    GLfloat realHeight = size.y;
    GLfloat realX = 0.0f;
    GLfloat realY = 0.0f;

    if (this->getImage ().model->passthrough) {
	width = 1.0f;
	height = 1.0f;
	realX = this->m_pos.x;
	realY = this->m_pos.w;
	realWidth = this->m_pos.z;
	realHeight = this->m_pos.y;

    }
    if (this->getImage ().model->fullscreen) {
	realX = -1.0f;
	realY = -1.0f;
	realWidth = 1.0f;
	realHeight = 1.0f;
    }

    GLfloat texcoordCopy[] = { x, height, x, y, width, height, width, height, x, y, width, y };
    m_texcoordCopyTopV = height;
    m_texcoordCopyBottomV = y;
    GLfloat copySpacePosition[] = { realX,     realHeight, 0.0f, realX, realY, 0.0f, realWidth, realHeight, 0.0f,
				    realWidth, realHeight, 0.0f, realX, realY, 0.0f, realWidth, realY,      0.0f };
    if (this->getImage ().model->passthrough && !this->getImage ().model->fullscreen) {
        const int corners[6] {0, 1, 2, 2, 1, 3};
        for (int vertex = 0; vertex < 6; ++vertex) {
            const auto& corner = m_sceneQuad[corners[vertex]];
            copySpacePosition[vertex * 3] = corner.x;
            copySpacePosition[vertex * 3 + 1] = corner.y;
            copySpacePosition[vertex * 3 + 2] = corner.z;
        }
    }

    glBindBuffer (GL_ARRAY_BUFFER, this->m_sceneSpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (sceneSpacePosition), sceneSpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_copySpacePosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (copySpacePosition), copySpacePosition, GL_DYNAMIC_DRAW);
    glBindBuffer (GL_ARRAY_BUFFER, this->m_texcoordCopy);
    glBufferData (GL_ARRAY_BUFFER, sizeof (texcoordCopy), texcoordCopy, GL_DYNAMIC_DRAW);

    const glm::vec2 projectionSize = imageBackingDimensions (size);
    this->m_modelViewProjectionCopy = this->getImage ().model->passthrough
	? this->m_modelViewProjectionScreen
	: this->getImage ().model->fullscreen ? glm::mat4 (1.0f)
	                                       : glm::ortho<float> (0.0, projectionSize.x, 0.0, projectionSize.y);
    this->m_modelViewProjectionCopyInverse = glm::inverse (this->m_modelViewProjectionCopy);
    this->m_modelMatrix = glm::ortho<float> (0.0, projectionSize.x, 0.0, projectionSize.y);
}

CImage::ResolvedTransform CImage::updateGeometryBuffers () {
    auto sceneWidth = static_cast<float> (this->getScene ().getWidth ());
    auto sceneHeight = static_cast<float> (this->getScene ().getHeight ());
    const auto transform = this->resolveTransform (this->getImage ());
    glm::vec3 origin = transform.origin;
    const glm::vec2 size = this->resolveGeometrySize (sceneWidth, sceneHeight, origin);
    this->m_size = size;
	if (this->m_hasPuppetMesh) {
	preparePuppetAnimation ();
	updatePuppetPositionBuffer (size, transform, sceneWidth, sceneHeight);
    }

    auto geometryTransform = transform;
    if (this->getImage ().model->fullscreen)
        geometryTransform.authoredMatrix[3] = glm::vec4 (origin, 1.0f);
    this->updateScenePosition (geometryTransform, size, sceneWidth, sceneHeight);
    this->uploadGeometryBuffers (size);
    return transform;
}

void CImage::updateScreenSpacePosition () {
    this->updateGeometryBuffers ();

    // Both the scene quad and the scene-space puppet vertices already carry
    // the authored transform. Rotating the projection would apply it twice.
    glm::mat4 mvp = this->getScene ().getActiveRenderProjection ();

    const glm::vec2 depth = getImage ().parallaxDepth->value->getVec2 ();
    // Native 14018aac0 offsets the scene matrix before an unparented object's
    // own transform, using its authored origin and camera-relative cursor.
    // Keep the older grouped path until parent/root inheritance is matched.
    const glm::vec3 parallax = getImage ().parent
        ? getScene ().getLayerParallaxOffset (depth)
        : getScene ().getLayerParallaxOffset (getImage ().origin->value->getVec3 (), depth);
    mvp = glm::translate (mvp, parallax);

    this->m_modelViewProjectionScreen = mvp;
    this->m_modelViewProjectionScreenInverse = glm::inverse (mvp);
    const auto& camera = getScene ().getCamera ();
    glm::mat4 authoredToCamera (1.0f);
    if (camera.isOrthogonal ()) {
        authoredToCamera = glm::translate (authoredToCamera,
            glm::vec3 (-getScene ().getWidth () * 0.5f, getScene ().getHeight () * 0.5f, 0));
        authoredToCamera = glm::scale (authoredToCamera, glm::vec3 (1, -1, 1));
        // Native 1401891a0 retains the authored camera eye and adds the
        // canvas center, with a fixed orthographic eye distance of 2000.
        m_lightingEye = camera.getEye ()
            + glm::vec3 (getScene ().getWidth () * 0.5f, getScene ().getHeight () * 0.5f, 0);
        m_lightingEye.z = 2000.0f;
    } else {
        m_lightingEye = camera.getEye ();
    }
    m_lightingViewProjection = mvp * authoredToCamera;
    m_lightingMvp = m_lightingViewProjection * m_lightingWorld;
    const float determinant = glm::determinant (m_lightingMvp);
    m_lightingMvpInverse = determinant != 0.0f && std::isfinite (determinant)
        ? glm::inverse (m_lightingMvp) : glm::mat4 (1.0f);
    if (this->getImage ().model->passthrough) {
	// A root fullscreen passthrough samples the already cropped output
	// framebuffer. Its copy quad is clip space; applying the authored crop
	// again would stretch/crop postprocessing a second time.
	this->m_modelViewProjectionCopy = this->getImage ().model->fullscreen
            && !m_composesChildren && !getScene ().isChildCompositionScope ()
            ? glm::mat4 (1.0f) : this->m_modelViewProjectionScreen;
	this->m_modelViewProjectionCopyInverse = this->m_modelViewProjectionScreenInverse;
        if (this->getImage ().model->fullscreen
            && !m_composesChildren && !getScene ().isChildCompositionScope ())
            this->m_modelViewProjectionCopyInverse = glm::mat4 (1.0f);
    }
}

const Image& CImage::getImage () const { return this->m_image; }

bool CImage::executeMaterialFunction (const std::string& name) {
    bool executed = false;
    for (const auto& source : this->m_effectActions)
        executed = executeEffectClearAction (*source.effect, *source.provider, name) || executed;
    return executed;
}

glm::vec2 CImage::getSize () const {
    return this->getImage ().size->value->getVec2 ();
}

GLuint CImage::getSceneSpacePosition () const { return this->m_sceneSpacePosition; }

GLuint CImage::getCopySpacePosition () const { return this->m_copySpacePosition; }

GLuint CImage::getPassSpacePosition () const { return this->m_passSpacePosition; }

GLuint CImage::getTexCoordCopy () const { return this->m_texcoordCopy; }

GLuint CImage::getTexCoordPass () const { return this->m_texcoordPass; }
