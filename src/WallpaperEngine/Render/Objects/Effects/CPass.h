#pragma once

#include <functional>
#include <glm/gtc/type_ptr.hpp>
#include <utility>

#include "../../TextureProvider.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"

namespace WallpaperEngine::Render::Objects {
class CRenderable;
}

namespace WallpaperEngine::Render::Objects::Effects {
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Render::Shaders::Variables;
using namespace WallpaperEngine::Data::Model;

class CPass final : public Helpers::ContextAware {
public:
    CPass (
	CRenderable& renderable, std::shared_ptr<const FBOProvider> fboProvider, const MaterialPass& pass,
	std::optional<std::reference_wrapper<const ImageEffectPassOverride>> override,
	std::optional<std::reference_wrapper<const TextureMap>> binds,
	std::optional<std::reference_wrapper<std::string>> target
    );
    ~CPass ();

    void render ();

    void setDestination (std::shared_ptr<const CFBO> drawTo);
    void setInput (std::shared_ptr<const TextureProvider> input);
    void setTexture0Override (std::shared_ptr<const TextureProvider> texture);
    void setTextureFrameOverride (std::optional<uint32_t> frame);
    void setPreviousInput (std::shared_ptr<const TextureProvider> input);
    void setTexCoord (GLuint texcoord, float topV = 0.0f, float bottomV = 1.0f);
    void setPosition (GLuint position);
    void setModelViewProjectionMatrix (const glm::mat4* projection);
    void setModelViewProjectionMatrixInverse (const glm::mat4* projection);
    void setModelMatrix (const glm::mat4* model);
    void setNormalModelMatrix (const glm::mat3* normalModel);
    void setViewProjectionMatrix (const glm::mat4* viewProjection);
    void setBlendingMode (BlendingMode blendingmode);
    [[nodiscard]] BlendingMode getBlendingMode () const;
    [[nodiscard]] std::shared_ptr<const CFBO> resolveFBO (const std::string& name) const;

    [[nodiscard]] std::shared_ptr<const FBOProvider> getFBOProvider () const;
    [[nodiscard]] const CRenderable& getRenderable () const;
    [[nodiscard]] const MaterialPass& getPass () const;
    [[nodiscard]] std::optional<std::reference_wrapper<std::string>> getTarget () const;
    [[nodiscard]] Render::Shaders::Shader* getShader () const;
    [[nodiscard]] GLuint getProgramID () const;

    // Custom geometry rendering support (for particles, etc.)
    using GeometryCallback = std::function<void ()>;
    void
    setGeometryCallback (GeometryCallback setupAttribs, GeometryCallback drawGeometry, GeometryCallback cleanupAttribs);

    // Public uniform setters for external callers (pointer-based, updated per-frame)
    void addUniform (const std::string& name, const float* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec3* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec4* value, int count = 1);
    void addUniform (const std::string& name, const glm::mat4* value, int count = 1);

private:
    std::shared_ptr<const TextureProvider> m_texture0Override;
    std::optional<uint32_t> m_textureFrameOverride;
    struct TextureChainEntry {
	mutable std::shared_ptr<const TextureProvider> texture;
	std::shared_ptr<const TextureProvider> fallbackTexture;
	std::shared_ptr<TextureChainEntry> next;
        std::string resourceName;
        const PropertySceneTexture* selection = nullptr;
        mutable std::string selectedValue;
    };

    enum UniformType {
	Float = 0,
	Matrix3 = 1,
	Matrix4 = 2,
	Integer = 3,
	Vector2 = 4,
	Vector3 = 5,
	Vector4 = 6,
	Double = 7
    };

    class UniformEntry {
    public:
	UniformEntry (const GLint id, std::string name, UniformType type, const void* value, int count) :
	    id (id), name (std::move (name)), type (type), value (value), count (count) { }

	const GLint id;
	std::string name;
	UniformType type;
	const void* value;
	int count;
    };

    class ReferenceUniformEntry {
    public:
	ReferenceUniformEntry (const GLint id, std::string name, UniformType type, const void** value) :
	    id (id), name (std::move (name)), type (type), value (value) { }

	const GLint id;
	std::string name;
	UniformType type;
	const void** value;
    };

    class AttribEntry {
    public:
	AttribEntry (const GLint id, std::string name, GLint type, GLint elements, const GLuint* value) :
	    id (id), name (std::move (name)), type (type), elements (elements), value (value) { }

	const GLint id;
	std::string name;
	GLint type;
	GLint elements;
	const GLuint* value;
    };

    struct TextureAnimationState {
	uint32_t currentTexture = 0;
	glm::vec2 translation = { 0.0f, 0.0f };
	glm::vec4 rotation = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static GLuint compileShader (const char* shader, GLuint type);
    void setupShaders ();
    void setupShaderVariables ();
    void setupUniforms ();
    void setupTextureUniforms ();
    void setupAttributes ();
    void addAttribute (const std::string& name, GLint type, GLint elements, const GLuint* value);
    void addUniform (ShaderVariable* value);
    void addUniform (const ShaderVariable* value, const DynamicValue* setting);
    void addUniform (const std::string& name, int value);
    void addUniform (const std::string& name, double value);
    void addUniform (const std::string& name, float value);
    void addUniform (const std::string& name, glm::vec2 value);
    void addUniform (const std::string& name, glm::vec3 value);
    void addUniform (const std::string& name, glm::vec4 value);
    void addUniform (const std::string& name, const glm::mat3& value);
    void addUniform (const std::string& name, glm::mat4 value);
    void addUniform (const std::string& name, const int* value, int count = 1);
    void addUniform (const std::string& name, const double* value, int count = 1);
    void addUniform (const std::string& name, const glm::vec2* value, int count = 1);
    void addUniform (const std::string& name, const glm::mat3* value, int count = 1);
    void addUniform (const std::string& name, const int** value);
    void addUniform (const std::string& name, const double** value);
    void addUniform (const std::string& name, const float** value);
    void addUniform (const std::string& name, const glm::vec2** value);
    void addUniform (const std::string& name, const glm::vec3** value);
    void addUniform (const std::string& name, const glm::vec4** value);
    void addUniform (const std::string& name, const glm::mat3** value);
    void addUniform (const std::string& name, const glm::mat4** value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T value);
    template <typename T> void addUniform (const std::string& name, UniformType type, T* value, int count = 1);
    template <typename T> void addUniform (const std::string& name, UniformType type, T** value);

    void setupRenderFramebuffer () const;
    void setupRenderTexture ();
    [[nodiscard]] std::shared_ptr<const TextureProvider> resolveTexture0 ();
    [[nodiscard]] TextureAnimationState
    resolveTextureAnimationState (const std::shared_ptr<const TextureProvider>& texture) const;
    void bindTextureUnit (int index, const std::shared_ptr<const TextureProvider>& texture, uint32_t frame) const;
    void bindTextureOverrides (uint32_t currentTexture, std::shared_ptr<const TextureProvider>& texture0);
    void setupRenderUniforms ();
    void setupRenderReferenceUniforms ();
    void setupRenderAttributes () const;
    void renderGeometry () const;
    void cleanupRenderSetup ();

    std::shared_ptr<const TextureProvider> resolveTexture (
	std::shared_ptr<const TextureProvider> expected, int index,
	std::shared_ptr<const TextureProvider> previous = nullptr
    );

    CRenderable& m_renderable;
    std::shared_ptr<const FBOProvider> m_fboProvider;
    const MaterialPass& m_pass;
    const TextureMap& m_binds;
    const ImageEffectPassOverride& m_override;
    std::optional<std::reference_wrapper<std::string>> m_target;
    std::map<int, std::shared_ptr<const CFBO>> m_fbos = {};
    std::map<std::string, int> m_combos = {};
    std::vector<AttribEntry*> m_attribs = {};
    std::map<std::string, UniformEntry*> m_uniforms = {};
    float m_texcoordTopV = 0.0f;
    float m_texcoordBottomV = 1.0f;
    std::map<std::string, ReferenceUniformEntry*> m_referenceUniforms = {};
    BlendingMode m_blendingmode = BlendingMode_Normal;
    const glm::mat4* m_modelViewProjectionMatrix;
    const glm::mat4* m_modelViewProjectionMatrixInverse;
    const glm::mat4* m_modelMatrix;
    glm::mat3 m_identityNormalModelMatrix {1.0f};
    const glm::mat3* m_normalModelMatrix = &m_identityNormalModelMatrix;
    const glm::mat4* m_viewProjectionMatrix;

    /**
     * Contains the final map of textures to be used
     */
    std::map<int, std::shared_ptr<TextureChainEntry>> m_textures = {};
    std::map<int, glm::vec4> m_textureResolutions = {};
    [[nodiscard]] std::shared_ptr<const TextureProvider> resolveChainTexture (const TextureChainEntry& entry) const;

    Render::Shaders::Shader* m_shader = nullptr;

    std::shared_ptr<const CFBO> m_drawTo = nullptr;
    std::shared_ptr<const TextureProvider> m_input = nullptr;
    std::shared_ptr<const TextureProvider> m_previousInput = nullptr;
    glm::vec4 m_texture0Resolution = {};
    glm::vec2 m_sceneTexelSize = {};
    glm::vec2 m_sceneTexelSizeHalf = {};

    GLuint m_programID;

    // shader variables used temporary
    GLint g_Texture0Rotation;
    GLint g_Texture0Translation;
    GLuint a_TexCoord;
    GLuint a_Position;
    GLuint m_vao;

    // Custom geometry callbacks (for particles, etc.)
    GeometryCallback m_setupAttribsCallback;
    GeometryCallback m_drawGeometryCallback;
    GeometryCallback m_cleanupAttribsCallback;
};
} // namespace WallpaperEngine::Render::Objects::Effects
