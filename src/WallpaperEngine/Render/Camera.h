#pragma once

#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "WallpaperEngine/Data/Model/Wallpaper.h"

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

    Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera,
            const SceneCamera* activeObject = nullptr);
    ~Camera ();

    [[nodiscard]] static const SceneCamera* selectActiveSceneCamera (const ObjectList& objects);
    [[nodiscard]] static Pose poseForSceneCamera (const SceneCamera& camera);
    [[nodiscard]] static Pose poseForRootCamera (const SceneData::Camera& camera);
    void setTransforms (const glm::vec3* eye, const glm::vec3* center,
                        const glm::vec3* up, const float* zoom);
    [[nodiscard]] static Transforms updatedTransforms (
        Transforms current, const glm::vec3* eye, const glm::vec3* center,
        const glm::vec3* up, const float* zoom);
    [[nodiscard]] static glm::mat4 renderLookAtForTransforms (
        const Pose& pose, bool orthogonal, float width, float height);
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
        float width, float height, float authoredNearZ, float authoredFarZ, const glm::vec3& eye);

    [[nodiscard]] const glm::vec3& getCenter () const;
    [[nodiscard]] const glm::vec3& getEye () const;
    [[nodiscard]] const glm::vec3& getUp () const;
    [[nodiscard]] const glm::mat4& getProjection () const;
    [[nodiscard]] const glm::mat4& getLookAt () const;
    [[nodiscard]] const glm::mat4& getRenderLookAt () const;
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
    Transforms m_scriptTransforms;
    bool m_hasScriptTransforms = false;
    const SceneData::Camera& m_camera;
    const SceneCamera* m_activeObject;
    Wallpapers::CScene& m_scene;
};
} // namespace WallpaperEngine::Render
