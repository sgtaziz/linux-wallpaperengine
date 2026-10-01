#pragma once

#include "CRenderable.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Objects/PuppetSkinning.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Render/TextureAnimation.h"

#include "WallpaperEngine/Render/Shaders/Shader.h"

#include "../TextureProvider.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/vec3.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Scripting;
namespace WallpaperEngine::Render::Objects::Effects {
class CMaterial;
class CPass;
} // namespace WallpaperEngine::Render::Objects::Effects

namespace WallpaperEngine::Render::Objects {
class CImage final : public CRenderable, public ScriptableObject {
    friend CObject;

public:
    CImage (Wallpapers::CScene& scene, const Image& image);
    ~CImage () override;

    void setup () override;
    void render () override;
    void renderWithChildren (const std::function<void (std::shared_ptr<const CFBO>)>& renderChildren);
    [[nodiscard]] bool canComposeChildren ();

    [[nodiscard]] const Image& getImage () const;
    [[nodiscard]] const std::string& getAlignment () const { return m_alignment; }
    void setAlignment (std::string alignment) { m_alignment = std::move (alignment); }
    [[nodiscard]] std::optional<glm::mat4> puppetAttachmentTransform (const std::string& name) const;
    [[nodiscard]] std::optional<glm::mat4> puppetEmissionBoneTransform (uint8_t boneIndex) const;
    /** Source-pixel image emission needs native puppet bone association. */
    [[nodiscard]] bool hasPuppetEmissionDeformation () const;
    [[nodiscard]] glm::vec2 getSize () const;
    /** Execute a named authored material function on this image's effect targets. */
    bool executeMaterialFunction (const std::string& name);
    [[nodiscard]] std::shared_ptr<ImageTextureAnimation> getTextureAnimation ();
    [[nodiscard]] std::optional<uint32_t> textureAnimationFrameOverride () const;
    void advanceTextureAnimation (float delta, uint32_t frame);

    [[nodiscard]] GLuint getSceneSpacePosition () const;
    [[nodiscard]] GLuint getCopySpacePosition () const;
    [[nodiscard]] GLuint getPassSpacePosition () const;
    [[nodiscard]] GLuint getTexCoordCopy () const;
    [[nodiscard]] GLuint getTexCoordPass () const;

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

    /**
     * Performs a ping-pong on the available framebuffers to be able to continue rendering things to them
     *
     * @param drawTo The framebuffer to use
     * @param asInput The last texture used as output (if needed)
     */
    void pinpongFramebuffer (std::shared_ptr<const CFBO>* drawTo, std::shared_ptr<const TextureProvider>* asInput);

protected:
    void setupPasses (const std::function<void (std::shared_ptr<const CFBO>)>& renderChildren = {});

    void updateScreenSpacePosition ();

    using ResolvedTransform = Wallpapers::ResolvedSceneTransform;

    [[nodiscard]] ResolvedTransform resolveTransform (const WallpaperEngine::Data::Model::Object& object) const;

private:
    bool loadPuppetMesh (const glm::vec2& size);
    void updatePuppetAnimation ();
    void preparePuppetAnimation ();
    void updatePuppetPositionBuffer (
	const glm::vec2& size, const ResolvedTransform& transform, float sceneWidth, float sceneHeight
    );
    void setupPuppetGeometryCallback (Effects::CPass* pass, bool sceneSpace) const;
    void setupPuppetChannelGeometryCallback (Effects::CPass* pass, GLuint positionBuffer) const;
    void renderPuppetChannelPrepass ();
    void renderPuppetChannelDirect (
        const std::shared_ptr<const CFBO>& target
    );
    void refreshEffectVisibility ();
    ResolvedTransform updateGeometryBuffers ();
    bool refreshSizeDependentTargets ();
    [[nodiscard]] glm::vec2 getCompositeTargetSize () const;
    [[nodiscard]] glm::vec2 resolveGeometrySize (float sceneWidth, float sceneHeight, glm::vec3& origin) const;
    void updateScenePosition (
	const ResolvedTransform& transform, const glm::vec2& size, float sceneWidth, float sceneHeight
    );
    void uploadGeometryBuffers (const glm::vec2& size);
    [[nodiscard]] bool shouldRenderFinalPass (bool isLastPass) const;
    bool configurePassTarget (
	Effects::CPass* pass, std::shared_ptr<const CFBO>& drawTo,
	const std::shared_ptr<const TextureProvider>& asInput, std::shared_ptr<const TextureProvider>& effectInput,
	bool& inTargetEffectSequence
    );

    GLuint m_sceneSpacePosition;
    GLuint m_copySpacePosition;
    GLuint m_passSpacePosition;
    GLuint m_texcoordCopy;
    GLuint m_texcoordPass;
    GLuint m_texcoordPassPresented = GL_NONE;
    float m_texcoordCopyTopV = 1.0f;
    float m_texcoordCopyBottomV = 0.0f;
    GLuint m_puppetSpacePosition = GL_NONE;
    GLuint m_puppetSceneSpacePosition = GL_NONE;
    GLuint m_puppetTexCoord = GL_NONE;
    GLuint m_puppetTexCoordFull = GL_NONE;
    GLuint m_puppetBlendIndices = GL_NONE;
    GLuint m_puppetIndices = GL_NONE;
    GLsizei m_puppetIndexCount = 0;
    bool m_hasPuppetMesh = false;
    std::vector<GLfloat> m_puppetRawPositions = {};
    std::vector<glm::vec4> m_puppetBlendMap;
    std::optional<PuppetMeshData> m_puppetMesh;
    std::optional<PuppetMeshData> m_puppetChannelMesh;
    MaterialUniquePtr m_puppetChannelMaterial;
    std::unique_ptr<MaterialPass> m_puppetChannelBaseMaterial;
    Effects::CPass* m_puppetChannelBasePass = nullptr;
    Effects::CPass* m_puppetChannelPass = nullptr;
    std::shared_ptr<CFBO> m_puppetChannelFBO;
    bool m_puppetChannelOffscreen = false;
    glm::vec4 m_puppetPrepassColor {1.0f};
    GLuint m_puppetChannelPosition = GL_NONE;
    GLuint m_puppetChannelTexcoord = GL_NONE;
    GLuint m_puppetChannelBlendIndices = GL_NONE;
    GLuint m_puppetChannelIndices = GL_NONE;
    GLsizei m_puppetChannelIndexCount = 0;
    glm::mat4 m_puppetChannelProjection = glm::mat4 (1.0f);
    glm::mat4 m_puppetChannelProjectionInverse = glm::mat4 (1.0f);
    std::optional<PuppetSkeletonData> m_puppetSkeleton;
    std::optional<PuppetAnimationHeader> m_puppetAnimation;
    std::vector<glm::mat4> m_puppetCurrentGlobals;
    uint32_t m_puppetPoseFrame = UINT32_MAX;
    std::vector<PuppetPoseSample> m_puppetReferencePose;
    std::vector<glm::mat4> m_puppetInverseBind;
    std::vector<PuppetPlaybackState> m_puppetLayerStates;
    std::vector<uint64_t> m_puppetLayerClipIds;
    std::vector<bool> m_puppetLayerBlendInActive;
    std::vector<bool> m_effectVisibilityAtSetup;

    glm::mat4 m_modelViewProjectionScreen = {};
    glm::mat4 m_modelViewProjectionPass = {};
    glm::mat4 m_modelViewProjectionCopy = {};
    glm::mat4 m_modelViewProjectionScreenInverse = {};
    glm::mat4 m_modelViewProjectionPassInverse = {};
    glm::mat4 m_modelViewProjectionCopyInverse = {};

    glm::mat4 m_modelMatrix = {};
    glm::mat4 m_viewProjectionMatrix = {};
    GLuint m_lightingLocalPosition = GL_NONE;
    glm::mat4 m_lightingWorld {1.0f};
    glm::mat3 m_lightingNormal {1.0f};
    glm::mat4 m_lightingViewProjection {1.0f};
    glm::mat4 m_lightingMvp {1.0f};
    glm::mat4 m_lightingMvpInverse {1.0f};
    glm::vec3 m_lightingEye {0.0f};

    std::shared_ptr<CFBO> m_mainFBO = nullptr;
    std::shared_ptr<CFBO> m_subFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentMainFBO = nullptr;
    std::shared_ptr<const CFBO> m_currentSubFBO = nullptr;

    const Image& m_image;
    std::shared_ptr<ImageTextureAnimation> m_textureAnimation;
    std::optional<uint32_t> m_textureAnimationTick;
    std::string m_alignment;
    glm::vec4 m_effectiveColor4 = {};

    std::vector<Effects::CPass*> m_passes = {};
    Effects::CPass* m_compositePresentationPass = nullptr;
    bool m_hasCompositeConsumerAtSetup = false;
    [[nodiscard]] bool hasCompositeConsumer () const;
    size_t m_basePassCount = 0;
    struct ResourceSwap {
        size_t beforePass;
        std::shared_ptr<FBOProvider> provider;
        std::string source;
        std::string target;
    };
    std::vector<ResourceSwap> m_resourceSwaps = {};
    struct EffectActionSource {
        const WallpaperEngine::Data::Model::Effect* effect;
        std::shared_ptr<FBOProvider> provider;
    };
    std::vector<EffectActionSource> m_effectActions = {};
    struct SizedEffectTarget {
	const WallpaperEngine::Data::Model::FBO* descriptor;
	std::shared_ptr<FBOProvider> provider;
	size_t effectIndex;
    };
    std::vector<SizedEffectTarget> m_sizedEffectTargets = {};
    std::vector<std::shared_ptr<FBOProvider>> m_effectProviders = {};
    std::vector<glm::vec2> m_effectProviderSizes = {};
    glm::vec2 m_targetBaseSize = {};
    bool m_composesChildren = false;
    std::vector<MaterialPassUniquePtr> m_virtualPassess = {};

    glm::vec4 m_pos = {};
    glm::vec3 m_sceneQuad[4] = {};
    glm::vec3 m_sceneCenter = {};
    glm::vec2 m_size = {};

    bool m_initialized = false;

    struct {
	struct {
	    MaterialUniquePtr material;
	    ImageEffectPassOverrideUniquePtr override;
	} colorBlending;
	std::vector<MaterialUniquePtr> compatibilityMaterials = {};
	std::vector<ImageEffectPassOverrideUniquePtr> compatibilityOverrides = {};
    } m_materials;
};
} // namespace WallpaperEngine::Render::Objects
