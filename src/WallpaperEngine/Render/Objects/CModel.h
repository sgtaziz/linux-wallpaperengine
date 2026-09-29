#pragma once

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace WallpaperEngine::Render::Objects {

/** Draws a scene MDLV resource with its per-mesh authored materials. */
class CModel final : public Scripting::ScriptableObject {
public:
    CModel (Wallpapers::CScene& scene, const Data::Model::SceneModel& model);
    ~CModel () override;

    void setup () override;
    void render () override;

private:
    void rebuildDynamic ();
    void updateDynamic ();
    struct MeshDraw;
    const Data::Model::SceneModel& m_model;
    std::vector<Data::Model::MaterialUniquePtr> m_materials;
    std::vector<std::unique_ptr<MeshDraw>> m_meshes;
    uint64_t m_dynamicRevision = 0;
    uint64_t m_dynamicStructureRevision = 0;
    glm::mat4 m_world {1.0f};
    glm::mat4 m_viewProjection {1.0f};
    glm::mat4 m_mvp {1.0f};
    glm::mat4 m_mvpInverse {1.0f};
    glm::mat3 m_normalModel {1.0f};
};

} // namespace WallpaperEngine::Render::Objects
