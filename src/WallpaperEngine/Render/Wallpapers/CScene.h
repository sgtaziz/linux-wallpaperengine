#pragma once

#include "WallpaperEngine/Render/Camera.h"
#include "ParticleSceneClock.h"
#include "SceneCursorState.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/SceneSpectrumState.h"

#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include <array>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>

namespace WallpaperEngine::Render {
class Camera;
class CObject;
}

namespace WallpaperEngine::Render::Wallpapers {
using namespace WallpaperEngine::Data::Model;

class CScene final : public CWallpaper {
public:
    CScene (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    ~CScene () override;

    [[nodiscard]] Scripting::ScriptEngine& getScriptEngine () const;
    [[nodiscard]] Camera& getCamera () const;

    [[nodiscard]] const Scene& getScene () const;
    [[nodiscard]] bool isHdrPostprocessingActive () const { return m_hdrPostprocessing; }
    [[nodiscard]] const Audio::Drivers::Recorders::StereoSpectrum::Bands& getAudioSpectrum () const;

    [[nodiscard]] int getWidth () const override;
    [[nodiscard]] int getHeight () const override;
    [[nodiscard]] glm::vec2 getPresentationTextureSize () const { return m_presentationTextureSize; }

    // Time accessors used by dynamic text layers (CText + ScriptEngine).
    // Read from the application-wide g_Time/g_TimeLast globals that other
    // renderers already consume via extern (e.g. CParticle).
    [[nodiscard]] float getTime () const;
    [[nodiscard]] float getDeltaTime () const;
    [[nodiscard]] float getFps () const;
    [[nodiscard]] float getParticleSceneTime () const;
    [[nodiscard]] float getPreviousParticleSceneDuration () const { return m_particleFrameDurations.previous; }

    const glm::vec2* getMousePosition () const;
    const glm::vec2* getMousePositionLast () const;
    const glm::vec2* getMousePositionNormalized () const;
    [[nodiscard]] const glm::vec2& getMouseScreenPosition () const;
    [[nodiscard]] std::optional<glm::vec3> getMouseWorldPosition () const;
    [[nodiscard]] bool isMouseLeftDown () const;
    const glm::vec2* getParallaxDisplacement () const;
    [[nodiscard]] glm::vec3 getLayerParallaxOffset (const glm::vec2& depth) const;
    [[nodiscard]] glm::vec3 getLayerParallaxOffset (const glm::vec3& origin,
                                                   const glm::vec2& depth) const;

    [[nodiscard]] const std::vector<CObject*>& getObjectsByRenderOrder () const;
    /** Public layer order excludes removals immediately; physical deletion waits for callback completion. */
    [[nodiscard]] std::vector<CObject*> getScriptLayers () const;
    [[nodiscard]] bool isScriptCreatedLayer (const CObject& object) const;
    [[nodiscard]] std::mt19937& getParticleRandom ();
    [[nodiscard]] const CObject* getObject (int id) const;
    [[nodiscard]] int getPointLightCount () const;
    [[nodiscard]] const glm::vec4* getPointLightColors () const;
    [[nodiscard]] const glm::vec4* getPointLightOrigins () const;
    [[nodiscard]] int getSpotLightCount () const;
    [[nodiscard]] const glm::vec4* getSpotLightColors () const;
    [[nodiscard]] const glm::vec4* getSpotLightOrigins () const;
    [[nodiscard]] const glm::vec4* getSpotLightDirections () const;
    [[nodiscard]] const glm::vec4* getSpotLightExponents () const;
    [[nodiscard]] const glm::vec3* getLegacyLightPositions () const;
    [[nodiscard]] const glm::vec4* getLegacyLightColors () const;
    [[nodiscard]] const glm::vec4* getLegacyLightPremultipliedColors () const;
    [[nodiscard]] std::optional<glm::mat4> getPuppetAttachmentTransform (
        int parentId, const std::string& name
    ) const;
    /** Create a runtime layer from a SceneScript object configuration. */
    CObject* createScriptLayer (const std::string& configurationJson,
                                std::shared_ptr<DynamicModelData> dynamicModel = {});
    bool destroyScriptLayer (const CObject* object);
    [[nodiscard]] int getScriptLayerIndex (const CObject* object) const;
    bool sortScriptLayer (const CObject* object, int index);
    [[nodiscard]] std::shared_ptr<const CFBO> getActiveRenderTarget () const;
    [[nodiscard]] const glm::mat4& getActiveRenderProjection () const;
    [[nodiscard]] const glm::mat4& getRootRenderClipTransform () const { return m_rootRenderClipTransform; }
    [[nodiscard]] bool isChildCompositionScope () const;
    [[nodiscard]] bool isMaxAlphaCompositionScope () const;

protected:
    void renderFrame (const glm::ivec4& viewport) override;
    void updateMouse (const glm::ivec4& viewport);
    void dispatchCursorEvents ();

    friend class CWallpaper;

private:
    glm::vec2 m_presentationTextureSize {0.0f};
    [[nodiscard]] bool rendersAtOutputSize () const override { return true; }
    void resizeSceneTargets (int width, int height);
    Render::CObject* createObject (const Object& object);
    [[nodiscard]] const Object* findObjectData (int id) const;
    void flushDestroyedScriptLayers ();
    void refreshPointLights ();
    void refreshSpotLights ();
    void assignLegacyLightSlot (const ScenePointLight* light);
    Render::CObject* dispatchObjectType (const Object& object);
    void addObjectToRenderOrder (const Object& object);

    std::unique_ptr<Scripting::ScriptEngine> m_scriptEngine;
    std::unique_ptr<Camera> m_camera;
    ObjectUniquePtr m_bloomObjectData;
    CObject* m_bloomObject = nullptr;
    bool m_hdrPostprocessing = false;
    std::map<int, CObject*> m_objects = {};
    std::set<int> m_rejectedObjectIds = {};
    std::set<int> m_creatingObjects = {};
    std::vector<CObject*> m_objectsByRenderOrder = {};
    std::map<int, ObjectUniquePtr> m_scriptObjectData = {};
    std::set<int> m_pendingScriptLayerDestroy = {};
    std::set<int> m_destroyingScriptLayerIds = {};
    int m_nextScriptObjectId = 1;
    bool m_shuttingDown = false;
    std::vector<DynamicValue*> m_scriptedValues = {};
    Audio::Drivers::Recorders::SceneSpectrumState m_audioSpectrum;
    std::mt19937 m_particleRandom { 5489u };
    ParticleSceneFrameDurations m_particleFrameDurations;
    double m_particleSceneTimeAccumulator { 0.0 };
    float m_particleSceneTime { 0.0f };
    std::vector<const ScenePointLight*> m_pointLightObjects = {};
    std::vector<const SceneSpotLight*> m_spotLightObjects = {};
    std::vector<glm::vec4> m_spotLightColors = {};
    std::vector<glm::vec4> m_spotLightOrigins = {};
    std::vector<glm::vec4> m_spotLightDirections = {};
    std::vector<glm::vec4> m_spotLightExponents = {};
    std::map<const ScenePointLight*, size_t> m_legacyLightSlots = {};
    std::vector<glm::vec4> m_pointLightColors = {};
    std::vector<glm::vec4> m_pointLightOrigins = {};
    std::array<glm::vec3, 4> m_legacyLightPositions {};
    std::array<glm::vec4, 4> m_legacyLightColors {};
    std::array<glm::vec4, 3> m_legacyLightPremultipliedColors {};
    std::shared_ptr<const CFBO> m_activeRenderTarget = nullptr;
    glm::mat4 m_activeRenderProjection {1.0f};
    glm::mat4 m_rootRenderClipTransform {1.0f};
    bool m_childCompositionScope = false;
    bool m_maxAlphaCompositionScope = false;
    glm::vec2 m_mousePosition = {};
    glm::vec2 m_mousePositionLast = {};
    glm::vec2 m_mousePositionNormalized = {};
    glm::vec2 m_mouseScreenPosition = {};
    bool m_mouseLeftDown = false;
    bool m_cursorInputInitialized = false;
    glm::vec2 m_previousCursorScreenPosition = {};
    SceneCursorState m_cursorState;
    glm::vec2 m_parallaxDisplacement = {};
    std::shared_ptr<CFBO> _rt_4FrameBuffer = nullptr;
    std::shared_ptr<CFBO> _rt_8FrameBuffer = nullptr;
    std::shared_ptr<CFBO> _rt_Bloom = nullptr;
    std::shared_ptr<CFBO> _rt_shadowAtlas = nullptr;
};
} // namespace WallpaperEngine::Render::Wallpaper
