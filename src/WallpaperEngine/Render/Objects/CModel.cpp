#include "CModel.h"

#include "CRenderable.h"
#include "ModelNormalMatrix.h"
#include "PuppetMeshParser.h"
#include "StaticModelTail.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <iterator>
#include <stdexcept>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Data::Model;

namespace {
template <typename T> GLuint uploadBuffer (GLenum target, const std::vector<T>& values) {
    if (values.empty ()) return GL_NONE;
    GLuint buffer = GL_NONE;
    glGenBuffers (1, &buffer);
    glBindBuffer (target, buffer);
    glBufferData (target, GLsizeiptr (values.size () * sizeof (T)), values.data (), GL_STATIC_DRAW);
    return buffer;
}

template <typename T> void refreshBuffer (GLuint buffer, const std::vector<T>& values) {
    if (buffer == GL_NONE) return;
    // GL_ELEMENT_ARRAY_BUFFER changes the current VAO. COPY_WRITE_BUFFER is
    // neutral for vertex-array state, including when no VAO is bound.
    GLint previous = GL_NONE;
    glGetIntegerv (GL_COPY_WRITE_BUFFER_BINDING, &previous);
    glBindBuffer (GL_COPY_WRITE_BUFFER, buffer);
    glBufferSubData (GL_COPY_WRITE_BUFFER, 0,
                     GLsizeiptr (values.size () * sizeof (T)), values.data ());
    glBindBuffer (GL_COPY_WRITE_BUFFER, GLuint (previous));
}

void bindFloatAttribute (GLuint program, const char* name, GLuint buffer, GLint width) {
    if (buffer == GL_NONE) return;
    const GLint location = glGetAttribLocation (program, name);
    if (location < 0) return;
    glEnableVertexAttribArray (GLuint (location));
    glBindBuffer (GL_ARRAY_BUFFER, buffer);
    glVertexAttribPointer (GLuint (location), width, GL_FLOAT, GL_FALSE, 0, nullptr);
}

void disableAttribute (GLuint program, const char* name, GLuint buffer) {
    if (buffer == GL_NONE) return;
    const GLint location = glGetAttribLocation (program, name);
    if (location >= 0) glDisableVertexAttribArray (GLuint (location));
}

struct PresentationFrontFace {
    GLint previous = GL_CCW;
    bool reversed = false;
    explicit PresentationFrontFace (bool reverse) : reversed (reverse) {
        if (!reversed) return;
        glGetIntegerv (GL_FRONT_FACE, &previous);
        glFrontFace (previous == GL_CCW ? GL_CW : GL_CCW);
    }
    ~PresentationFrontFace () { if (reversed) glFrontFace (GLenum (previous)); }
};

}

struct CModel::MeshDraw final : CRenderable {
    MeshDraw (Wallpapers::CScene& scene, const SceneModel& object, const Material& material,
              const PuppetMeshData& mesh, const std::vector<uint32_t>* wideIndices = nullptr,
              size_t shapeIndex = 0) :
        CObject (scene, object), CRenderable (scene, object, material) {
        m_shapeIndex = shapeIndex;
        try {
        const size_t count = wideIndices ? wideIndices->size () : mesh.indices.size ();
        if (count > INT_MAX) throw std::runtime_error ("Model index count exceeds GLsizei");
        m_indexCount = GLsizei (count);
        m_position = uploadBuffer (GL_ARRAY_BUFFER, mesh.positions);
        if (mesh.vertexMask & 2) m_normal = uploadBuffer (GL_ARRAY_BUFFER, mesh.normals);
        if (mesh.vertexMask & 4) m_tangent = uploadBuffer (GL_ARRAY_BUFFER, mesh.tangents);
        if (mesh.vertexMask & 8) m_texcoord = uploadBuffer (GL_ARRAY_BUFFER, mesh.texcoords);
        if (mesh.vertexMask & 0x20) m_texcoordFull = uploadBuffer (GL_ARRAY_BUFFER, mesh.texcoordsFull);
        if (wideIndices) {
            m_index = uploadBuffer (GL_ELEMENT_ARRAY_BUFFER, *wideIndices);
            m_indexType = GL_UNSIGNED_INT;
        } else m_index = uploadBuffer (GL_ELEMENT_ARRAY_BUFFER, mesh.indices);

        detectTexture ();
        // CPass needs an input provider even for shader passes with no sampled
        // texture. Their authored sampler map remains empty; this placeholder
        // only satisfies that render-loop precondition.
        if (!m_texture) m_texture = getContext ().resolveTexture ("util/white");
        CRenderable::setup ();
        for (const auto& authoredPass : material.passes) {
            auto pass = std::make_unique<Effects::CPass> (
                *this, std::make_shared<FBOProvider> (this), *authoredPass,
                std::nullopt, std::nullopt, std::nullopt);
            auto* const passPointer = pass.get ();
            pass->setGeometryCallback (
                [this, passPointer] () {
                    const GLuint program = passPointer->getProgramID ();
                    bindFloatAttribute (program, "a_Position", m_position, 3);
                    bindFloatAttribute (program, "a_Normal", m_normal, 3);
                    bindFloatAttribute (program, "a_Tangent4", m_tangent, 4);
                    bindFloatAttribute (program, "a_TexCoord", m_texcoord, 2);
                    bindFloatAttribute (program, "a_TexCoordVec4", m_texcoordFull, 4);
                },
                [this] () {
                    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_index);
                    glDrawElements (GL_TRIANGLES, m_indexCount, m_indexType, nullptr);
                },
                [this, passPointer] () {
                    const GLuint program = passPointer->getProgramID ();
                    disableAttribute (program, "a_Position", m_position);
                    disableAttribute (program, "a_Normal", m_normal);
                    disableAttribute (program, "a_Tangent4", m_tangent);
                    disableAttribute (program, "a_TexCoord", m_texcoord);
                    disableAttribute (program, "a_TexCoordVec4", m_texcoordFull);
                });
            m_passes.push_back (std::move (pass));
        }
        } catch (...) {
            for (GLuint buffer : {m_position, m_normal, m_tangent, m_texcoord, m_texcoordFull, m_index})
                if (buffer != GL_NONE) glDeleteBuffers (1, &buffer);
            throw;
        }
    }

    ~MeshDraw () override {
        m_passes.clear ();
        for (GLuint buffer : {m_position, m_normal, m_tangent, m_texcoord, m_texcoordFull, m_index})
            if (buffer != GL_NONE) glDeleteBuffers (1, &buffer);
    }

    [[nodiscard]] const float& getBrightness () const override { return m_one; }
    [[nodiscard]] const float& getUserAlpha () const override { return m_one; }
    [[nodiscard]] const float& getAlpha () const override { return m_one; }
    [[nodiscard]] const glm::vec3& getColor () const override { return m_white; }
    [[nodiscard]] const glm::vec4& getColor4 () const override { return m_white4; }
    [[nodiscard]] const glm::vec3& getCompositeColor () const override { return m_white; }

    [[nodiscard]] bool requiresSceneReflection () const {
        return std::ranges::any_of (m_passes, [] (const auto& pass) { return pass->requiresSceneReflection (); });
    }

    [[nodiscard]] bool depthFirst () const {
        if (m_passes.empty ()) return true;
        const auto mode = m_passes.front ()->getBlendingMode ();
        return mode != BlendingMode_Translucent && mode != BlendingMode_Additive;
    }

    void update (const DynamicModelShape& shape) {
        refreshBuffer (m_position, shape.positions);
        refreshBuffer (m_normal, shape.normals);
        refreshBuffer (m_tangent, shape.tangents);
        refreshBuffer (m_texcoord, shape.texcoords);
        if (!shape.indices.empty ()) refreshBuffer (m_index, shape.indices);
    }

    void draw (CModel& model) {
        // Root presentation reverses clip-space winding. Orthographic child
        // composition also carries a local Y reflection in its projection.
        PresentationFrontFace frontFace (
            Wallpapers::modelPresentationReversesWinding (
                model.getScene ().getCamera ().isOrthogonal (), model.getScene ().isChildCompositionScope ()));
        for (const auto& pass : m_passes) {
            pass->setDestination (model.getScene ().getActiveRenderTarget ());
            pass->setInput (m_texture);
            pass->setModelMatrix (&model.m_world);
            pass->setNormalModelMatrix (&model.m_normalModel);
            pass->setViewProjectionMatrix (&model.m_viewProjection);
            pass->setModelViewProjectionMatrix (&model.m_mvp);
            pass->setModelViewProjectionMatrixInverse (&model.m_mvpInverse);
            pass->render ();
        }
    }

    float m_one = 1.0f;
    glm::vec3 m_white {1.0f};
    glm::vec4 m_white4 {1.0f};
    GLuint m_position = GL_NONE;
    GLuint m_normal = GL_NONE;
    GLuint m_tangent = GL_NONE;
    GLuint m_texcoord = GL_NONE;
    GLuint m_texcoordFull = GL_NONE;
    GLuint m_index = GL_NONE;
    GLsizei m_indexCount = 0;
    GLenum m_indexType = GL_UNSIGNED_SHORT;
    size_t m_shapeIndex = 0;
    std::vector<std::unique_ptr<Effects::CPass>> m_passes;
};

CModel::CModel (Wallpapers::CScene& scene, const SceneModel& model) :
    CObject (scene, model), Scripting::ScriptableObject (scene, model), m_model (model) { }

CModel::~CModel () = default;

void CModel::setup () {
    CObject::setup ();
    if (m_model.dynamic) {
        rebuildDynamic ();
        return;
    }
    const auto stream = getAssetLocator ().read (m_model.path);
    std::vector<char> data {std::istreambuf_iterator<char> (*stream), std::istreambuf_iterator<char> ()};
    const auto meshes = parsePuppetMeshes ({reinterpret_cast<const uint8_t*> (data.data ()), data.size ()});
    if (!staticModelHasOnlyPadding (
            {reinterpret_cast<const uint8_t*> (data.data ()), data.size ()}, meshes.sectionEndOffset))
        throw std::runtime_error ("Static MDLV has unsupported skeletal or tail sections");
    m_materials.reserve (meshes.meshes.size ());
    m_meshes.reserve (meshes.meshes.size ());
    for (const auto& mesh : meshes.meshes) {
        if ((mesh.vertexMask & 0x01800000) != 0)
            throw std::runtime_error ("Static MDLV skin/channel attributes require a separate runtime path");
        if (mesh.materials.empty ()) throw std::runtime_error ("Static MDLV mesh has no materials");
        const auto selected = std::min<size_t> (size_t (m_model.skin), mesh.materials.size () - 1);
        m_materials.push_back (Data::Parsers::MaterialParser::load (
            getScene ().getScene ().project, mesh.materials[selected]));
        if (m_materials.back ()->passes.empty ()) throw std::runtime_error ("Static MDLV material has no passes");
        m_meshes.push_back (std::make_unique<MeshDraw> (
            getScene (), m_model, *m_materials.back (), mesh));
    }
    // Native 14021a620 groups normal/alpha-to-coverage material categories
    // before translucent/additive. Linux keeps source order within a group;
    // native equal-category ordering has not been established.
    std::stable_sort (m_meshes.begin (), m_meshes.end (),
        [] (const auto& left, const auto& right) { return left->depthFirst () && !right->depthFirst (); });
}

void CModel::rebuildDynamic () {
    if (!m_model.dynamic || m_dynamicRevision == m_model.dynamic->revision) return;
    std::vector<Data::Model::MaterialUniquePtr> materials;
    std::vector<std::unique_ptr<MeshDraw>> draws;
    for (size_t shapeIndex = 0; shapeIndex < m_model.dynamic->shapes.size (); ++shapeIndex) {
        const auto& shape = m_model.dynamic->shapes[shapeIndex];
        if (shape.positions.empty () || shape.material.empty ()) continue;
        PuppetMeshData mesh;
        mesh.positions = shape.positions;
        mesh.normals = shape.normals;
        mesh.tangents = shape.tangents;
        mesh.texcoords = shape.texcoords;
        if (!mesh.normals.empty ()) mesh.vertexMask |= 2;
        if (!mesh.tangents.empty ()) mesh.vertexMask |= 4;
        if (!mesh.texcoords.empty ()) mesh.vertexMask |= 8;
        std::vector<uint32_t> generated;
        const std::vector<uint32_t>* wide = &shape.indices;
        if (shape.indices.empty ()) {
            for (size_t i = 0; i < shape.positions.size (); ++i) {
                generated.push_back (uint32_t (i));
            }
            wide = &generated;
        }
        materials.push_back (Data::Parsers::MaterialParser::load (
            getScene ().getScene ().project, shape.material));
        if (materials.back ()->passes.empty ()) throw std::runtime_error ("Dynamic model material has no passes");
        draws.push_back (std::make_unique<MeshDraw> (getScene (), m_model, *materials.back (), mesh, wide, shapeIndex));
    }
    m_meshes = std::move (draws);
    m_materials = std::move (materials);
    m_dynamicRevision = m_model.dynamic->revision;
    m_dynamicStructureRevision = m_model.dynamic->structureRevision;
    std::stable_sort (m_meshes.begin (), m_meshes.end (),
        [] (const auto& left, const auto& right) { return left->depthFirst () && !right->depthFirst (); });
}

void CModel::updateDynamic () {
    if (!m_model.dynamic || m_dynamicRevision == m_model.dynamic->revision) return;
    for (const auto& mesh : m_meshes) mesh->update (m_model.dynamic->shapes.at (mesh->m_shapeIndex));
    m_dynamicRevision = m_model.dynamic->revision;
}

bool CModel::requiresSceneReflection () {
    if (m_model.dynamic && m_dynamicStructureRevision != m_model.dynamic->structureRevision)
        rebuildDynamic ();
    return std::ranges::any_of (m_meshes, [] (const auto& mesh) { return mesh->requiresSceneReflection (); });
}

void CModel::render () {
    if (m_model.dynamic && m_dynamicStructureRevision != m_model.dynamic->structureRevision)
        rebuildDynamic ();
    else if (m_model.dynamic && m_dynamicRevision != m_model.dynamic->revision)
        updateDynamic ();
    const auto transform = Wallpapers::resolveSceneTransform (
        m_model,
        [this] (int parentId) -> const Object* {
            const auto* parent = getScene ().getObject (parentId);
            return parent ? &parent->getObject () : nullptr;
        },
        [this] (const Object& parent, const std::string& name) {
            return getScene ().getPuppetAttachmentTransform (parent.id, name);
        });
    if (!transform.visible) return;
    m_world = transform.authoredMatrix;
    m_viewProjection = getScene ().getActiveRenderProjection ()
        * Wallpapers::sceneAuthoredToCamera (
            getScene ().getWidth (), getScene ().getHeight (), getScene ().getCamera ().isOrthogonal ());
    m_mvp = m_viewProjection * m_world;
    const auto normal = modelNormalMatrix (m_world);
    if (!normal) return;
    m_normalModel = *normal;
    const float mvpDeterminant = glm::determinant (m_mvp);
    m_mvpInverse = mvpDeterminant == 0.0f ? glm::mat4 (1.0f) : glm::inverse (m_mvp);
    for (const auto& mesh : m_meshes) mesh->draw (*this);
}
