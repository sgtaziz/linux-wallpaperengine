#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "Camera.h"
#include "CObject.h"
#include "Wallpapers/SceneTransform.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;

Camera::Camera (Wallpapers::CScene& scene, const SceneData::Camera& camera,
                const SceneCamera* activeObject) :
    m_width (0), m_height (0),
    m_pose (poseForRootCamera (camera)), m_scriptTransforms {m_pose},
    m_camera (camera), m_activeObject (activeObject), m_scene (scene) {
    if (m_activeObject) m_pose = poseForSceneCamera (*m_activeObject);
    m_frameFov = m_activeObject ? m_activeObject->fov->value->getFloat ()
                              : m_camera.projection.fov->value->getFloat ();
    m_frameZoom = m_activeObject ? m_activeObject->zoom->value->getFloat () : 1.0f;
    m_runtimeOverride = m_activeObject || (!m_camera.configuration.paths.empty ()
        && !m_camera.configuration.paths.front ().samples.empty ());
    m_lookat = glm::lookAt (m_pose.eye, m_pose.center, m_pose.up);
    m_renderLookat = m_lookat;
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

Camera::Pose Camera::poseForWorldCamera (const glm::mat4& world) {
    // Native virtual+80 is the full current authored world matrix.
    const glm::vec3 eye (world[3]);
    return {eye, eye - glm::vec3 (world[2]), glm::vec3 (world[1])};
}

void Camera::advanceFrame (float dt, const std::vector<const SceneCamera*>& cameras) {
    m_activeObject = nullptr;
    m_frameFov = m_camera.projection.fov->value->getFloat ();
    m_frameZoom = m_scriptTransforms.zoom;
    m_pose = m_scriptTransforms.pose;
    m_runtimeOverride = false;
    for (auto it = cameras.rbegin (); it != cameras.rend (); ++it) {
        const auto* camera = *it;
        const auto resolved = Wallpapers::resolveSceneTransform (*camera,
            [this] (int id) -> const Object* {
                const auto* parent = m_scene.getObject (id);
                return parent ? &parent->getObject () : nullptr;
            }, [this] (const Object& parent, const std::string& attachment) {
                return m_scene.getPuppetAttachmentTransform (parent.id, attachment);
            });
        if (!resolved.visible) continue;
        m_activeObject = camera;
        m_runtimeOverride = true;
        m_pose = poseForWorldCamera (resolved.authoredMatrix);
        m_frameFov = camera->fov->value->getFloat ();
        m_frameZoom = camera->zoom->value->getFloat ();
        break;
    }
    if (!m_activeObject) {
        if (const auto sample = advanceNativeCameraPath (m_camera.configuration.paths, m_pathCursor, dt)) {
            m_runtimeOverride = true;
            m_pose = {sample->eye, sample->center, sample->up};
            m_frameZoom = sample->zoom;
        }
    }
    m_runtimeTransforms = m_runtimeOverride;
    m_lookat = glm::lookAt (m_pose.eye, m_pose.center, m_pose.up);
    if (m_width > 0 && m_height > 0) {
        if (m_isOrthogonal) setOrthogonalProjection (m_width, m_height);
        else setPerspectiveProjection (m_width, m_height);
    }
}

void Camera::forgetCamera (const SceneCamera& camera) {
    // Destruction after scripts must not invalidate this frame's cached pose,
    // FOV or zoom. Admission is reconsidered at the next outer update.
    if (m_activeObject == &camera) m_activeObject = nullptr;
}

const glm::vec3& Camera::getCenter () const { return m_pose.center; }

const glm::vec3& Camera::getEye () const { return m_pose.eye; }

const glm::vec3& Camera::getUp () const { return m_pose.up; }

const glm::mat4& Camera::getProjection () const { return this->m_projection; }

const glm::mat4& Camera::getLookAt () const { return this->m_lookat; }

const glm::mat4& Camera::getRenderLookAt () const { return m_renderLookat; }

Camera::Transforms Camera::updatedTransforms (
    Transforms current, const glm::vec3* eye, const glm::vec3* center,
    const glm::vec3* up, const float* zoom) {
    // Native 2.8.42 14018da90 copies each supplied pointer independently.
    if (eye) current.pose.eye = *eye;
    if (center) current.pose.center = *center;
    if (up) current.pose.up = *up;
    if (zoom) current.zoom = *zoom;
    return current;
}

glm::mat4 Camera::renderLookAtForTransforms (const Pose& pose, bool orthogonal, float width, float height) {
    const auto nativeView = glm::lookAt (pose.eye, pose.center, pose.up);
    if (!orthogonal) return nativeView;
    // Native 17fa70 applies P*T(canvasHalf)*zoom*T(-canvasHalf) before
    // the authored view. Our objects already use the centered GL basis,
    // so conjugate that entire basis, including its canvas translation.
    const auto bridge = Wallpapers::sceneAuthoredToCamera (width, height, true);
    return bridge * nativeView * glm::inverse (bridge);
}

glm::mat4 Camera::makeScriptOrthogonalProjection (float width, float height, float zoom) {
    // Native 140183a70 uses fixed orthographic depth -2000/+2000.
    // 14017fa70 scales XY about the authored canvas center by camera zoom;
    // our scene positions are centered already. Perspective ignores zoom.
    return glm::ortho (-width * 0.5f, width * 0.5f, -height * 0.5f, height * 0.5f,
                       -2000.0f, 2000.0f)
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (zoom, zoom, 1));
}

glm::mat4 Camera::makeProjectionForTransforms (
    float width, float height, float fov, float nearZ, float farZ,
    const Transforms& transforms, bool orthogonal, float authoredZoom) {
    return orthogonal ? makeScriptOrthogonalProjection (width, height, transforms.zoom * authoredZoom)
        : makePerspectiveProjectionForScene (width, height, fov, nearZ, farZ);
}

void Camera::setTransforms (const glm::vec3* eye, const glm::vec3* center,
                            const glm::vec3* up, const float* zoom) {
    if (!eye && !center && !up && !zoom) return;
    m_scriptTransforms = updatedTransforms (m_scriptTransforms, eye, center, up, zoom);
    // Native 1401891a0 chooses an active camera object, then camera paths,
    // before the stored root transforms. Paths are not animated here yet.
    if (m_runtimeOverride) return;
    m_hasScriptTransforms = true;
    m_pose = m_scriptTransforms.pose;
    m_frameZoom = m_scriptTransforms.zoom;
    m_lookat = glm::lookAt (m_pose.eye, m_pose.center, m_pose.up);
    m_renderLookat = renderLookAtForTransforms (m_pose, m_isOrthogonal, m_width, m_height);
    if (m_width > 0 && m_height > 0) {
        if (m_isOrthogonal) setOrthogonalProjection (m_width, m_height);
        else setPerspectiveProjection (m_width, m_height);
    }
}

bool Camera::isOrthogonal () const { return this->m_isOrthogonal; }

Wallpapers::CScene& Camera::getScene () const { return this->m_scene; }

float Camera::getWidth () const { return this->m_width; }

float Camera::getHeight () const { return this->m_height; }

float Camera::getFov () const {
    return std::clamp (m_frameFov, 0.1f, 179.9f);
}

float Camera::getPerspectiveOverrideFov () const {
    return this->m_camera.projection.perspectiveOverrideFov->value->getFloat ();
}

float Camera::getNearZ () const { return this->m_camera.projection.nearz->value->getFloat (); }

float Camera::getFarZ () const { return this->m_camera.projection.farz->value->getFloat (); }

void Camera::setOrthogonalProjection (const float width, const float height) {
    this->m_width = width;
    this->m_height = height;

    this->m_projection = (m_hasScriptTransforms || m_runtimeTransforms)
        ? makeProjectionForTransforms (width, height, getFov (), getNearZ (), getFarZ (),
                                       Transforms {m_pose, m_frameZoom}, true,
                                       m_camera.projection.zoom ? m_camera.projection.zoom->value->getFloat () : 1.0f)
        : makeOrthogonalProjectionForScene (width, height, getNearZ (), getFarZ (), getEye ());
    this->m_isOrthogonal = true;
    m_renderLookat = (m_hasScriptTransforms || m_runtimeTransforms)
        ? renderLookAtForTransforms (m_pose, true, width, height) : m_lookat;
}

void Camera::setPerspectiveProjection (float width, float height) {
    m_width = width;
    m_height = height;
    m_projection = m_hasScriptTransforms
        ? makeProjectionForTransforms (width, height, getFov (), getNearZ (), getFarZ (),
                                       m_scriptTransforms, false)
        : makePerspectiveProjectionForScene (width, height, getFov (), getNearZ (), getFarZ ());
    m_isOrthogonal = false;
    m_renderLookat = m_lookat;
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
    float width, float height, float, float, const glm::vec3& eye) {
    // Native 140183a70 uses fixed -2000/+2000 planes when scene bit8 is
    // orthographic. Authored nearz/farz remain available to perspective
    // consumers; applying them here clips XYZ-rotated quads crossing z=0.
    auto projection = glm::ortho<float> (
        -width / 2.0f, width / 2.0f, -height / 2.0f, height / 2.0f, -2000.0f, 2000.0f);
    return glm::translate (projection, eye);
}
