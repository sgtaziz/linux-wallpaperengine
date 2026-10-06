#pragma once

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"
#include "ModelPuppetPose.h"
#include "ParticleModelCollision.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render {
class DepthTarget;
}
namespace WallpaperEngine::Render::Wallpapers {
struct ShadowAtlasTile;
}

namespace WallpaperEngine::Render::Objects {

/** Draws a scene MDLV resource with its per-mesh authored materials. */
class CModel final : public Scripting::ScriptableObject {
public:
    CModel (Wallpapers::CScene& scene, const Data::Model::SceneModel& model);
    ~CModel () override;

    void setup () override;
    void render () override;
    /** Draw this model's casters into one spot-light atlas tile (native shadow stage 2). */
    void renderShadowCaster (const DepthTarget& atlas, const Wallpapers::ShadowAtlasTile& tile,
                             const glm::mat4& viewProjection);
    /** Refresh material structure before rebuilding the secondary draw list. */
    [[nodiscard]] bool requiresSceneReflection ();
    [[nodiscard]] std::vector<ParticleCore::CollisionCapsule> collisionCapsules (
        const glm::mat4& inverseInvocation) const;

private:
    void rebuildDynamic ();
    void updateDynamic ();
    void preparePuppetPose ();
    [[nodiscard]] glm::mat4 authoredWorld () const;
    [[nodiscard]] bool castsShadow () const;
    struct MeshDraw;
    const Data::Model::SceneModel& m_model;
    std::vector<Data::Model::MaterialUniquePtr> m_materials;
    std::vector<std::unique_ptr<MeshDraw>> m_meshes;
    uint64_t m_dynamicRevision = 0;
    uint64_t m_dynamicStructureRevision = 0;
    glm::mat4 m_world {1.0f};
    glm::vec3 m_eyePosition {0.0f};
    glm::mat4 m_shadowWorld {1.0f};
    glm::mat4 m_viewProjection {1.0f};
    glm::mat4 m_mvp {1.0f};
    glm::mat4 m_mvpInverse {1.0f};
    glm::mat3 m_normalModel {1.0f};
    std::optional<PuppetSkeletonData> m_skeleton;
    std::vector<glm::mat4> m_currentGlobals;
    /** Present only when the object authors animationlayers and the mdl has clips. */
    std::optional<PuppetAnimationHeader> m_animation;
    ModelPuppetLayers m_layers;
    uint32_t m_poseFrame = UINT32_MAX;
    std::optional<PuppetSkeletonData::EmissionBound> m_collisionFallbackBound;
};

} // namespace WallpaperEngine::Render::Objects
