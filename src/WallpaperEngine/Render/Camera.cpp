#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "Camera.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera,
                const SceneCamera* activeObject) :
    m_width (0), m_height (0),
    m_pose (poseForRootCamera (camera)),
    m_camera (camera), m_activeObject (activeObject), m_scene (scene) {
    if (m_activeObject) m_pose = poseForSceneCamera (*m_activeObject);
    m_lookat = glm::lookAt (m_pose.eye, m_pose.center, m_pose.up);
}

Camera::~Camera () = default;

Camera::Pose Camera::poseForRootCamera (const SceneData::Camera& camera) {
    // Native 140186c90 LAB_140188648 resets orthographic root cameras when
    // their parsed path list is empty. Active camera objects override below.
    if (camera.projection.isOrthogonal && !camera.configuration.hasPaths)
        return {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    return {camera.configuration.eye, camera.configuration.center, camera.configuration.up};
}

const SceneCamera* Camera::selectActiveSceneCamera (const ObjectList& objects) {
    // Native selects the last active camera object before consulting scene
    // camera paths. This also lets a hidden camera leave the root pose active.
    for (auto it = objects.rbegin (); it != objects.rend (); ++it) {
        if (!(*it)->is<SceneCamera> ()) continue;
        const auto* camera = (*it)->as<SceneCamera> ();
        if (camera->groupVisible && camera->groupVisible->value
            && !camera->groupVisible->value->getBool ()) continue;
        return camera;
    }
    return nullptr;
}

Camera::Pose Camera::poseForSceneCamera (const SceneCamera& camera) {
    const glm::vec3 eye = camera.origin->value->getVec3 ();
    const glm::vec3 angles = camera.groupAngles->value->getVec3 ();
    // Native object transform uses authored radians. The camera looks down
    // local -Z and takes local +Y as up after the object's rotation.
    const glm::mat4 rotation =
        glm::rotate (glm::mat4 (1.0f), angles.z, glm::vec3 (0.0f, 0.0f, 1.0f)) *
        glm::rotate (glm::mat4 (1.0f), angles.y, glm::vec3 (0.0f, 1.0f, 0.0f)) *
        glm::rotate (glm::mat4 (1.0f), angles.x, glm::vec3 (1.0f, 0.0f, 0.0f));
    return {eye, eye - glm::vec3 (rotation[2]), glm::vec3 (rotation[1])};
}

const glm::vec3& Camera::getCenter () const { return m_pose.center; }

const glm::vec3& Camera::getEye () const { return m_pose.eye; }

const glm::vec3& Camera::getUp () const { return m_pose.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const {
    return m_activeObject ? m_activeObject->fov->value->getFloat ()
                          : m_camera.projection.fov->value->getFloat ();
}

float Camera::getPerspectiveOverrideFov () const {
    return this->m_camera.projection.perspectiveOverrideFov->value->getFloat ();
}

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    this->m_projection = makeOrthogonalProjectionForScene (
        width, height, getNearZ (), getFarZ (), getEye ());
    this->m_isOrthogonal = true;
}

void Camera::setPerspectiveProjection (float width, float height) {
    m_width = width;
    m_height = height;
    m_projection = makePerspectiveProjectionForScene (
        width, height, getFov (), getNearZ (), getFarZ ());
    m_isOrthogonal = false;
}

glm::mat4 Camera::makePerspectiveProjectionForScene (
    float width, float height, float authoredFov, float authoredNearZ, float authoredFarZ
) {
    if (!std::isfinite (width) || !std::isfinite (height) || width <= 0.0f || height <= 0.0f ||
        !std::isfinite (authoredFov) || authoredFov <= 0.0f || authoredFov >= 180.0f ||
        !std::isfinite (authoredNearZ) || authoredNearZ <= 0.0f ||
        !std::isfinite (authoredFarZ) || authoredFarZ <= authoredNearZ)
        throw std::invalid_argument ("Invalid perspective scene projection");
    // CWallpaper's OpenGL final blit inverts the scene texture vertically.
    // The native D3D final triangle uses the unflipped UV branch (the initial
    // host flags contain bit 0), so compensate once at the perspective scene
    // projection. Child composition targets use their own orthographic scope.
    glm::mat4 presentationCorrection (1.0f);
    presentationCorrection[1][1] = -1.0f;
    return presentationCorrection * glm::perspective (
        glm::radians (authoredFov), width / height, authoredNearZ, authoredFarZ);
}

glm::mat4 Camera::makeOrthogonalProjectionForScene (
    float width, float height, float authoredNearZ, float authoredFarZ, const glm::vec3& eye) {
    // Authored nearz is retained for perspective consumers (notably particles).
    // The orthographic 2D scene draws image quads on z=0, which a positive near
    // plane clips. Keep that plane inside the projection until perspective scene
    // rendering has its own validated camera/depth contract.
    const float nearZ = std::isfinite (authoredNearZ) ? std::min (authoredNearZ, 0.0f) : 0.0f;
    const float farZ = std::isfinite (authoredFarZ) && authoredFarZ > 0.0f ? authoredFarZ : 1000.0f;
    auto projection = glm::ortho<float> (
        -width / 2.0f, width / 2.0f, -height / 2.0f, height / 2.0f, nearZ, farZ);
    return glm::translate (projection, eye);
}
