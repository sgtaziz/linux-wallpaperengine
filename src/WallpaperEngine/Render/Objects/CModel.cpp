#include "CModel.h"

#include "CRenderable.h"
#include "ModelNormalMatrix.h"
#include "PuppetMeshParser.h"
#include "StaticModelTail.h"
#include "PuppetSkinning.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/DepthTarget.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Render/Wallpapers/SpotLightShadow.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <climits>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
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

// A shader names its shadow-stage replacement with `// [PASS] shadow <shader>`
// (foliage4, fur4); every other material uses the shipped shadowcaster.
std::string shadowCasterShader (const Assets::AssetLocator& assets, const std::string& shader) {
    static const std::regex declaration (R"(^\s*//\s*\[PASS\]\s+shadow\s+([A-Za-z0-9_/]+))");
    for (const auto& source : {assets.fragmentShader (shader), assets.vertexShader (shader)}) {
        std::istringstream lines (source);
        std::string line;
        std::smatch match;
        while (std::getline (lines, line))
            if (std::regex_search (line, match, declaration)) return match[1].str ();
    }
    return "shadowcaster";
}

// Native forwards only the combos that the caster declares.
std::set<std::string> declaredCombos (const Assets::AssetLocator& assets, const std::string& shader) {
    static const std::regex combo (R"re(//\s*\[COMBO\].*"combo"\s*:\s*"([^"]+)")re");
    std::set<std::string> result;
    for (const auto& source : {assets.fragmentShader (shader), assets.vertexShader (shader)}) {
        std::istringstream lines (source);
        std::string line;
        std::smatch match;
        while (std::getline (lines, line))
            if (std::regex_search (line, match, combo)) result.insert (match[1].str ());
    }
    return result;
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
              size_t shapeIndex = 0, const std::vector<glm::mat4>& palette = {}) :
        CObject (scene, object), CRenderable (scene, object, material), m_material (material) {
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
        if (!palette.empty () && (mesh.vertexMask & 0x01000000) != 0) {
            m_skinMesh = mesh;
            skin (mesh, palette, true);
        }
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
            attachGeometry (*passPointer);
            m_passes.push_back (std::move (pass));
        }
        } catch (...) {
            for (GLuint buffer : {m_position, m_normal, m_tangent, m_texcoord, m_texcoordFull, m_index,
                                 m_skinnedPosition, m_skinnedNormal, m_skinnedTangent, m_blendIndices, m_blendWeights})
                if (buffer != GL_NONE) glDeleteBuffers (1, &buffer);
            throw;
        }
    }

    /**
     * CPU-skins positions/normals/tangents and publishes the bone matrices for
     * shader skinning. The first call creates the buffers; later calls (animated
     * MODEL pose updates) refresh them in place.
     */
    void skin (const PuppetMeshData& mesh, const std::vector<glm::mat4>& palette, bool create) {
        std::vector<std::array<float, 3>> positions, normals;
        std::vector<std::array<float, 4>> tangents;
        for (size_t vertex = 0; vertex < mesh.positions.size (); ++vertex) {
            const auto position = puppetSkinnedPosition (mesh, vertex, palette);
            positions.push_back ({position.x, position.y, position.z});
            const auto direction = [&] (glm::vec3 value) {
                glm::vec3 result (0);
                for (size_t lane = 0; lane < 4; ++lane) {
                    const auto weight = mesh.blendWeights.at (vertex)[lane];
                    if (weight != 0) result += glm::mat3 (palette.at (mesh.blendIndices.at (vertex)[lane]))
                        * value * weight;
                }
                return result;
            };
            if (mesh.vertexMask & 2) {
                const auto normal = direction (glm::make_vec3 (mesh.normals.at (vertex).data ()));
                normals.push_back ({normal.x, normal.y, normal.z});
            }
            if (mesh.vertexMask & 4) {
                const auto& raw = mesh.tangents.at (vertex);
                const auto tangent = direction (glm::make_vec3 (raw.data ()));
                tangents.push_back ({tangent.x, tangent.y, tangent.z, raw[3]});
            }
        }
        if (create) {
            m_skinnedPosition = uploadBuffer (GL_ARRAY_BUFFER, positions);
            m_skinnedNormal = uploadBuffer (GL_ARRAY_BUFFER, normals);
            m_skinnedTangent = uploadBuffer (GL_ARRAY_BUFFER, tangents);
            m_blendIndices = uploadBuffer (GL_ARRAY_BUFFER, mesh.blendIndices);
            m_blendWeights = uploadBuffer (GL_ARRAY_BUFFER, mesh.blendWeights);
        } else {
            refreshBuffer (m_skinnedPosition, positions);
            refreshBuffer (m_skinnedNormal, normals);
            refreshBuffer (m_skinnedTangent, tangents);
        }
        m_bones.clear ();
        for (const auto& bone : palette)
            for (size_t column = 0; column < 4; ++column)
                for (size_t row = 0; row < 3; ++row) m_bones.push_back (bone[column][row]);
    }

    /** Re-skins this mesh for an animated pose; no-op for unskinned meshes. */
    void updatePalette (const std::vector<glm::mat4>& palette) {
        if (m_skinMesh) skin (*m_skinMesh, palette, false);
    }

    void attachGeometry (Effects::CPass& target) {
        auto* const passPointer = &target;
        target.setGeometryCallback (
            [this, passPointer] () {
                const GLuint program = passPointer->getProgramID ();
                const GLint bones = glGetUniformLocation (program, "g_Bones[0]");
                const bool shaderSkinning = bones >= 0 && !m_bones.empty ();
                if (shaderSkinning) glUniformMatrix4x3fv (bones, GLsizei (m_bones.size () / 12), GL_FALSE, m_bones.data ());
                bindFloatAttribute (program, "a_Position", shaderSkinning || !m_skinnedPosition ? m_position : m_skinnedPosition, 3);
                bindFloatAttribute (program, "a_Normal", shaderSkinning || !m_skinnedNormal ? m_normal : m_skinnedNormal, 3);
                bindFloatAttribute (program, "a_Tangent4", shaderSkinning || !m_skinnedTangent ? m_tangent : m_skinnedTangent, 4);
                bindFloatAttribute (program, "a_BlendWeights", m_blendWeights, 4);
                const GLint indices = glGetAttribLocation (program, "a_BlendIndices");
                if (indices >= 0 && m_blendIndices) {
                    glEnableVertexAttribArray (GLuint (indices));
                    glBindBuffer (GL_ARRAY_BUFFER, m_blendIndices);
                    glVertexAttribIPointer (GLuint (indices), 4, GL_UNSIGNED_INT, 0, nullptr);
                }
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
                disableAttribute (program, "a_BlendWeights", m_blendWeights);
                disableAttribute (program, "a_BlendIndices", m_blendIndices);
            });
    }

    /**
     * Native sets the support bit for normal (0) or alpha-to-coverage (3)
     * materials. Linux parses alphatocoverage as normal blending, so only the
     * normal category is distinguishable here.
     */
    [[nodiscard]] bool supportsShadowCasting () const {
        return !m_material.passes.empty () && m_material.passes.front ()->blending == BlendingMode_Normal;
    }

    [[nodiscard]] bool ensureShadowPass () {
        if (m_shadowPass || m_shadowPassFailed || m_material.passes.empty ()) return m_shadowPass != nullptr;
        try {
            // Native replaces the material in the shadow stage with
            // its caster shader, forwarding the declared combos and textures.
            const auto& original = *m_material.passes.front ();
            const auto shader = shadowCasterShader (getAssetLocator (), original.shader);
            ComboMap combos;
            for (const auto& name : declaredCombos (getAssetLocator (), shader))
                if (const auto found = original.combos.find (name); found != original.combos.end ())
                    combos.emplace (*found);
            m_shadowMaterial = std::make_unique<MaterialPass> ();
            m_shadowMaterial->blending = BlendingMode_Normal;
            m_shadowMaterial->cullmode = CullingMode_Disable;
            m_shadowMaterial->depthtest = DepthtestMode_Enabled;
            m_shadowMaterial->depthwrite = DepthwriteMode_Enabled;
            m_shadowMaterial->shader = shader;
            m_shadowMaterial->textures = original.textures;
            m_shadowMaterial->combos = std::move (combos);
            m_shadowPass = std::make_unique<Effects::CPass> (
                *this, std::make_shared<FBOProvider> (this), *m_shadowMaterial,
                std::nullopt, std::nullopt, std::nullopt);
            attachGeometry (*m_shadowPass);
            m_shadowPass->addUniform ("g_ViewportViewProjectionMatrices", m_shadowViewProjections.data (),
                                      int (m_shadowViewProjections.size ()));
        } catch (const std::exception& e) {
            sLog.error ("Cannot create shadow caster for model ", getId (), ": ", e.what ());
            m_shadowPass.reset ();
            m_shadowPassFailed = true;
        }
        return m_shadowPass != nullptr;
    }

    void drawShadow (CModel& model, const DepthTarget& atlas, const Wallpapers::ShadowAtlasTile& tile,
                     const glm::mat4& viewProjection) {
        if (!ensureShadowPass ()) return;
        // GL 3.3 has no viewport arrays: each atlas viewport is a separate
        // single-instance draw using the shader's first viewport matrix.
        m_shadowViewProjections.fill (viewProjection);
        m_shadowPass->setDepthDestination (Effects::CPass::DepthDestination {
            atlas.getFramebuffer (), glm::ivec4 (tile.x, tile.y, tile.size, tile.size)});
        m_shadowPass->setInput (m_texture);
        m_shadowPass->setModelMatrix (&model.m_shadowWorld);
        m_shadowPass->setNormalModelMatrix (&model.m_normalModel);
        m_shadowPass->setViewProjectionMatrix (&model.m_viewProjection);
        m_shadowPass->setModelViewProjectionMatrix (&model.m_mvp);
        m_shadowPass->setModelViewProjectionMatrixInverse (&model.m_mvpInverse);
        m_shadowPass->render ();
    }

    ~MeshDraw () override {
        m_passes.clear ();
        m_shadowPass.reset ();
        for (GLuint buffer : {m_position, m_normal, m_tangent, m_texcoord, m_texcoordFull, m_index,
                             m_skinnedPosition, m_skinnedNormal, m_skinnedTangent, m_blendIndices, m_blendWeights})
            if (buffer != GL_NONE) glDeleteBuffers (1, &buffer);
    }

    [[nodiscard]] const float& getBrightness () const override { return m_one; }
    [[nodiscard]] const float& getUserAlpha () const override { return m_one; }
    [[nodiscard]] const float& getAlpha () const override { return m_one; }
    [[nodiscard]] const glm::vec3& getColor () const override { return m_white; }
    [[nodiscard]] const glm::vec4& getColor4 () const override { return m_white4; }
    [[nodiscard]] const glm::vec3& getCompositeColor () const override { return m_white; }

    void bindEyePosition (const glm::vec3* eye) {
        for (const auto& pass : m_passes) pass->addUniform ("g_EyePosition", eye);
    }

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
                model.getScene ().getCamera ().isOrthogonal (), model.getScene ().isLocalCompositionScope ()));
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
    GLuint m_skinnedPosition = GL_NONE, m_skinnedNormal = GL_NONE, m_skinnedTangent = GL_NONE;
    GLuint m_blendIndices = GL_NONE, m_blendWeights = GL_NONE;
    std::vector<float> m_bones;
    std::optional<PuppetMeshData> m_skinMesh;
    GLsizei m_indexCount = 0;
    GLenum m_indexType = GL_UNSIGNED_SHORT;
    size_t m_shapeIndex = 0;
    std::vector<std::unique_ptr<Effects::CPass>> m_passes;
    const Material& m_material;
    std::unique_ptr<MaterialPass> m_shadowMaterial;
    std::unique_ptr<Effects::CPass> m_shadowPass;
    bool m_shadowPassFailed = false;
    std::array<glm::mat4, 6> m_shadowViewProjections {};
};

CModel::CModel (Wallpapers::CScene& scene, const SceneModel& model) :
    CObject (scene, model), Scripting::ScriptableObject (scene, model), m_model (model) {
    for (const auto& binding : Scripting::scriptPropertyBindings (model))
        registerProperty (binding.name, binding.value);
}

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
    m_collisionFallbackBound = ParticleCore::modelCollisionFallbackBound (meshes.meshes);
    std::vector<glm::mat4> palette;
    const std::span<const uint8_t> bytes {reinterpret_cast<const uint8_t*> (data.data ()), data.size ()};
    if (!staticModelHasOnlyPadding (bytes, meshes.sectionEndOffset)) {
        auto skeleton = parsePuppetSkeleton (bytes, meshes);
        // The shared parser validates subsequent MDAT/MDLA records. A MODEL
        // without animation layers uses the native unanimated local pose.
        if (!staticModelHasOnlyPadding (bytes, skeleton.sectionEndOffset)) {
            auto animation = parsePuppetFirstAnimationHeader (bytes, skeleton);
            if (!staticModelHasOnlyPadding (bytes, animation.sectionEndOffset))
                throw std::runtime_error ("Model has unsupported trailing records");
            // Native resolves each layer's clip id in the
            // model's clip list; the pose is then evaluated like an image puppet.
            if (!m_model.animationLayers.empty () && !animation.clips.empty ()) {
                bool supported = true;
                if (std::any_of (skeleton.bones.begin (), skeleton.bones.end (),
                                 [] (const PuppetBoneRecord& bone) { return (bone.rawFlags & 2) != 0; })) {
                    if (!skeleton.boneMappingKnown) supported = false;
                    else for (size_t i = 0; i < skeleton.bones.size (); ++i)
                        if ((skeleton.bones[i].rawFlags & 2) != 0 && skeleton.mappingRecordByBone[i] >= 0)
                            supported = false;
                }
                if (supported) {
                    m_layers.reset (skeleton, m_model.animationLayers);
                    m_animation = std::move (animation);
                } else
                    sLog.error ("Model animation unavailable for ", m_model.path,
                                ": unsupported flag-2 bone mapping");
            }
        }
        const auto local = puppetUnanimatedLocalMatrices (skeleton);
        m_currentGlobals = puppetGlobalMatrices (skeleton, local);
        palette = puppetSkinPalette (skeleton, local);
        m_skeleton = std::move (skeleton);
    }
    m_materials.reserve (meshes.meshes.size ());
    m_meshes.reserve (meshes.meshes.size ());
    for (const auto& mesh : meshes.meshes) {
        if ((mesh.vertexMask & 0x01800000) != 0 &&
            (!m_skeleton || (mesh.vertexMask & 0x01800000) != 0x01800000))
            throw std::runtime_error ("Model channel attributes require a separate runtime path");
        if (mesh.materials.empty ()) throw std::runtime_error ("Static MDLV mesh has no materials");
        const auto selected = std::min<size_t> (size_t (m_model.skin), mesh.materials.size () - 1);
        m_materials.push_back (Data::Parsers::MaterialParser::load (
            getScene ().getScene ().project, mesh.materials[selected]));
        if (m_materials.back ()->passes.empty ()) throw std::runtime_error ("Static MDLV material has no passes");
        m_meshes.push_back (std::make_unique<MeshDraw> (
            getScene (), m_model, *m_materials.back (), mesh, nullptr, 0, palette));
    }
    for (const auto& mesh : m_meshes) mesh->bindEyePosition (&m_eyePosition);
    // Native groups normal/alpha-to-coverage material categories
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
    for (const auto& mesh : m_meshes) mesh->bindEyePosition (&m_eyePosition);
    m_dynamicRevision = m_model.dynamic->revision;
    m_dynamicStructureRevision = m_model.dynamic->structureRevision;
    std::stable_sort (m_meshes.begin (), m_meshes.end (),
        [] (const auto& left, const auto& right) { return left->depthFirst () && !right->depthFirst (); });
}

void CModel::preparePuppetPose () {
    if (!m_animation || !m_skeleton) return;
    const uint32_t frame = getScene ().getContext ().getDriver ().getFrameCounter ();
    if (m_poseFrame == frame) return;
    m_poseFrame = frame;
    try {
        const auto local = modelPuppetLocalMatrices (
            *m_skeleton, *m_animation, m_model.animationLayers, m_layers,
            std::max (0.0f, getScene ().getDeltaTime ()));
        m_currentGlobals = puppetGlobalMatrices (*m_skeleton, local);
        const auto palette = puppetSkinPalette (*m_skeleton, local);
        for (const auto& mesh : m_meshes) mesh->updatePalette (palette);
    } catch (const std::exception& error) {
        sLog.error ("Model animation disabled for ", m_model.path, ": ", error.what ());
        m_animation.reset ();
    }
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

glm::mat4 CModel::authoredWorld () const {
    return Wallpapers::resolveSceneTransform (m_model,
        [this] (int id) -> const Object* {
            const auto* parent = getScene ().getObject (id);
            return parent ? &parent->getObject () : nullptr;
        }, [this] (const Object& parent, const std::string& name) {
            return getScene ().getPuppetAttachmentTransform (parent.id, name);
        }).authoredMatrix;
}

std::vector<ParticleCore::CollisionCapsule> CModel::collisionCapsules (const glm::mat4& inverseInvocation) const {
    if ((!m_skeleton || m_skeleton->emissionBounds.empty ()) && m_collisionFallbackBound)
        return ParticleCore::modelCollisionCapsules (std::span (&*m_collisionFallbackBound, 1), {},
            authoredWorld (), inverseInvocation);
    if (!m_skeleton) return {};
    return ParticleCore::modelCollisionCapsules (
        m_skeleton->emissionBounds, m_currentGlobals, authoredWorld (), inverseInvocation);
}

void CModel::render () {
    preparePuppetPose ();
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
    // Native publishes the scene eye to lit materials; orthographic
    // scenes add the canvas center and use a fixed eye distance of 2000.
    const auto& camera = getScene ().getCamera ();
    m_eyePosition = camera.getEye ();
    if (camera.isOrthogonal ()) {
        m_eyePosition += glm::vec3 (getScene ().getWidth () * 0.5f, getScene ().getHeight () * 0.5f, 0);
        m_eyePosition.z = 2000.0f;
    }
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

bool CModel::castsShadow () const {
    // Native defaults the MODEL castshadow bit on; programmatic
    // model data without an authored setting keeps that default.
    return !m_model.castShadow || !m_model.castShadow->value || m_model.castShadow->value->getBool ();
}

void CModel::renderShadowCaster (const DepthTarget& atlas, const Wallpapers::ShadowAtlasTile& tile,
                                 const glm::mat4& viewProjection) {
    if (!castsShadow ()) return;
    preparePuppetPose ();
    if (m_model.dynamic && m_dynamicStructureRevision != m_model.dynamic->structureRevision)
        rebuildDynamic ();
    else if (m_model.dynamic && m_dynamicRevision != m_model.dynamic->revision)
        updateDynamic ();
    // Native admits visible models with the caster support bit.
    if (std::ranges::none_of (m_meshes, [] (const auto& mesh) { return mesh->supportsShadowCasting (); }))
        return;
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
    m_shadowWorld = transform.authoredMatrix;
    // Native flags the shadow submission only around the shadow
    // draw; then draws only meshes whose
    // material category is normal (0) or alpha-to-coverage (3).
    for (const auto& mesh : m_meshes)
        if (mesh->supportsShadowCasting ()) mesh->drawShadow (*this, atlas, tile, viewProjection);
}
