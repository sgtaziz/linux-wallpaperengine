#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "CameraObjectPaths.h"
#include "CameraPath.h"
#include <unordered_map>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class Camera {
public:
    struct Pose {
        glm::vec3 eye;
        glm::vec3 center;
        glm::vec3 up;
    };
    struct Transforms {
        Pose pose;
        float zoom = 1.0f;
    };
    /** Native frame shake; inactive while camerashake is off. */
    struct Shake {
        bool active = false;
        glm::vec3 offset {0.0f};
        [[nodiscard]] static Shake evaluate (const SceneData::Camera& camera, float sceneTime,
                                             bool orthogonal, float orthogonalHeight);
        /** Eye and center move together; up, and every inactive pose, are returned unchanged. */
        [[nodiscard]] Pose apply (Pose pose) const;
        /** The native-canvas world translation expressed in the orthographic render basis. */
        [[nodiscard]] glm::vec3 orthogonalRenderTranslation (float width, float height) const;
    };

    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera,
            const SceneCamera* activeObject = nullptr);
    ~Camera ();

    [[nodiscard]] static const SceneCamera* selectActiveSceneCamera (const ObjectList& objects);
    [[nodiscard]] static Pose poseForSceneCamera (const SceneCamera& camera);
    [[nodiscard]] static Pose poseForRootCamera (const SceneData::Camera& camera);
    void advanceFrame (float dt, const std::vector<const SceneCamera*>& cameras);
    void forgetCamera (const SceneCamera& camera);
    /** Active-path state of a camera object that has ticked at least once (tests). */
    [[nodiscard]] const CameraObjectPathState* objectPathState (const SceneCamera& camera) const;
    [[nodiscard]] static Pose poseForWorldCamera (const glm::mat4& world);
    void setTransforms (const glm::vec3* eye, const glm::vec3* center,
                        const glm::vec3* up, const float* zoom);
    [[nodiscard]] static Transforms updatedTransforms (
        Transforms current, const glm::vec3* eye, const glm::vec3* center,
        const glm::vec3* up, const float* zoom);
    [[nodiscard]] static glm::mat4 renderLookAtForTransforms (
        const Pose& pose, bool orthogonal, float width, float height);
    /** Orthographic render view without script/runtime transforms; pose includes the shake. */
    [[nodiscard]] static glm::mat4 untransformedOrthogonalRenderLookAt (
        const Pose& pose, const Shake& shake, float width, float height);
    /** View that cursor unprojection pairs with the drawn view (pose includes the shake). */
    [[nodiscard]] static glm::mat4 cursorLookAtForShake (
        const Pose& pose, const glm::mat4& lookAt, const Shake& shake, bool orthogonal, float width, float height);
    /** Orthographic flag-4 particle view at the given eye distance, keeping the frame shake. */
    [[nodiscard]] static glm::mat4 flag4OrthogonalView (
        float eyeDistance, const Shake& shake, float width, float height);
    /**
     * Native view-projection for an image/text layer with `perspective`
     * in an orthographic scene, from the flat projection/view it replaces.
     * Uses the scene's perspectiveoverridefov and authored nearz/farz.
     */
    [[nodiscard]] static glm::mat4 orthogonalLayerPerspectiveViewProjection (
        const glm::mat4& projection, const glm::mat4& view, float width, float height,
        const SceneData::Camera& camera);
    [[nodiscard]] static glm::mat4 makeScriptOrthogonalProjection (
        float width, float height, float zoom);
    [[nodiscard]] static glm::mat4 makeProjectionForTransforms (
        float width, float height, float fov, float nearZ, float farZ,
        const Transforms& transforms, bool orthogonal, float authoredZoom = 1.0f);

    void setOrthogonalProjection (const float width, const float height);
    void setPerspectiveProjection (float width, float height);
    [[nodiscard]] static glm::mat4 makePerspectiveProjectionForScene (
        float width, float height, float authoredFov, float authoredNearZ, float authoredFarZ);
    [[nodiscard]] static glm::mat4 makeOrthogonalProjectionForScene (
        float width, float height, float authoredNearZ, float authoredFarZ, const glm::vec3& eye,
        float authoredZoom = 1.0f);

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    [[nodiscard]] const glm::mat4& getLookAt () const;
    /** The root camera-path fade alpha after this frame's path advance. */
    [[nodiscard]] float getPathFadeAlpha () const {
        return nativeCameraFadeAlpha (m_camera.configuration.paths, m_pathCursor);
    }
    [[nodiscard]] const glm::mat4& getRenderLookAt () const;
    [[nodiscard]] glm::mat4 getCursorLookAt () const;
    [[nodiscard]] const Shake& getShake () const;
    [[nodiscard]] Wallpapers::CScene& getScene () const;
    [[nodiscard]] bool isOrthogonal () const;
    [[nodiscard]] float getWidth () const;
    [[nodiscard]] float getHeight () const;
    [[nodiscard]] float getFov () const;
    [[nodiscard]] float getPerspectiveOverrideFov () const;
    [[nodiscard]] float getNearZ () const;
    [[nodiscard]] float getFarZ () const;

private:
    float m_width;
    float m_height;
    bool m_isOrthogonal = false;
    glm::mat4 m_projection = {};
    glm::mat4 m_lookat = {};
    glm::mat4 m_renderLookat = {};
    Pose m_pose;
    Shake m_shake;
    Transforms m_scriptTransforms;
    bool m_hasScriptTransforms = false;
    const SceneData::Camera& m_camera;
    const SceneCamera* m_activeObject;
    CameraPathCursor m_pathCursor;
    /** Native per-object queue state, created at a camera object's first visible tick. */
    std::unordered_map<const SceneCamera*, CameraObjectPathState> m_objectPaths;
    float m_frameFov = 50;
    float m_frameZoom = 1;
    bool m_runtimeOverride = false;
    bool m_runtimeTransforms = false;
    Wallpapers::CScene& m_scene;
};
} // namespace WallpaperEngine::Render
