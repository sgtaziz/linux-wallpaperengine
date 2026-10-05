#include "CVolumetrics.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Render/Shaders/GLSLContext.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"

using namespace WallpaperEngine::Render::Wallpapers;
using namespace WallpaperEngine::Data::Model;

namespace {
GLuint compile (const std::string& source, GLenum type) {
    const GLuint shader = glCreateShader (type);
    const char* text = source.c_str ();
    glShaderSource (shader, 1, &text, nullptr);
    glCompileShader (shader);
    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048] = {};
        glGetShaderInfoLog (shader, sizeof (log) - 1, nullptr, log);
        glDeleteShader (shader);
        throw std::runtime_error (std::string ("Volumetric shader compilation failed: ") + log);
    }
    return shader;
}

GLuint link (const std::string& vertex, const std::string& fragment) {
    const GLuint vs = compile (vertex, GL_VERTEX_SHADER);
    const GLuint fs = compile (fragment, GL_FRAGMENT_SHADER);
    const GLuint program = glCreateProgram ();
    glAttachShader (program, vs);
    glAttachShader (program, fs);
    glLinkProgram (program);
    glDeleteShader (vs);
    glDeleteShader (fs);
    GLint status = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[2048] = {};
        glGetProgramInfoLog (program, sizeof (log) - 1, nullptr, log);
        glDeleteProgram (program);
        throw std::runtime_error (std::string ("Volumetric program link failed: ") + log);
    }
    return program;
}

void colorTarget (GLuint framebuffer, GLuint texture, glm::ivec2 size, GLint format, GLenum type, GLint filter) {
    glBindTexture (GL_TEXTURE_2D, texture);
    glTexImage2D (GL_TEXTURE_2D, 0, format, size.x, size.y, 0, format == GL_R32F ? GL_RED : GL_RGBA, type, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
}

// Native preamble definitions the shipped volumetricsfront relies on: the
// non-multisampled texSample2DBackBuffer, texLoad2D
// and the HLSL clip intrinsic (statement form, scalar argument, as the
// shader uses it). Only this shader uses them.
std::string withVolumetricIntrinsics (std::string source) {
    const auto line = source.find ('\n');
    source.insert (line == std::string::npos ? 0 : line + 1,
        "#define texSample2DBackBuffer(s, uv, res) texture(s, uv)\n"
        "#define texLoad2D(s, uv, res) texelFetch(s, ivec2((uv) * (res).xy), 0)\n"
        "#define sampler2DBackBuffer sampler2D\n"
        "#define clip(x) { if ((x) < 0.0) discard; }\n");
    return source;
}

// The shipped volumetricsback.vert transform; the colour output stores the
// normalized device depth that native samples from its depth target.
const char* kBackVertex =
    "#version 330\n"
    "uniform mat4 g_ViewProjectionMatrix;\nuniform mat4 g_AltViewProjectionMatrix;\n"
    "in vec3 a_Position;\n"
    "void main () { gl_Position = g_ViewProjectionMatrix * (g_AltViewProjectionMatrix * vec4 (a_Position, 1.0)); }\n";
const char* kBackFragment =
    "#version 330\nout vec4 out_Depth;\n"
    "void main () { out_Depth = vec4 (gl_FragCoord.z * 2.0 - 1.0); }\n";
// materials/util/volumetrics_combine.json: passthrough of the light buffer, additive.
const char* kCombineVertex =
    "#version 330\nin vec2 a_Position;\nout vec2 v_TexCoord;\n"
    "void main () { v_TexCoord = a_Position * 0.5 + 0.5; gl_Position = vec4 (a_Position, 0.0, 1.0); }\n";
const char* kCombineFragment =
    "#version 330\nuniform sampler2D g_Texture0;\nin vec2 v_TexCoord;\nout vec4 out_FragColor;\n"
    "void main () { out_FragColor = texture (g_Texture0, v_TexCoord); }\n";

// A unit UV sphere standing in for the volume mesh.
constexpr int kRings = 24, kSegments = 48;

// Saves the scene's GL state, leaves every capability off with all write
// masks open (a clear honours the masks and the scissor test), and puts the
// entry state back when the volumetric pass ends.
struct SavedState {
    static constexpr GLenum capabilities[] {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_SCISSOR_TEST,
        GL_CULL_FACE, GL_POLYGON_OFFSET_FILL, GL_FRAMEBUFFER_SRGB, GL_RASTERIZER_DISCARD};
    static constexpr GLuint units[] {0, 1, 3};
    std::array<GLboolean, std::size (capabilities)> enabled {};
    std::array<GLint, std::size (units)> textures {}, samplers {};
    GLint drawFramebuffer = 0, readFramebuffer = 0, program = 0, vao = 0, activeTexture = 0, arrayBuffer = 0;
    GLint viewport[4] = {};
    GLboolean depthMask = GL_TRUE;
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint blendEquationRgb = 0, blendEquationAlpha = 0, depthFunc = 0;
    GLboolean colorMask[4] = {};
    GLfloat clearColor[4] = {};
    GLdouble clearDepth = 1.0;

    SavedState () {
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv (GL_CURRENT_PROGRAM, &program);
        glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv (GL_ACTIVE_TEXTURE, &activeTexture);
        for (size_t i = 0; i < std::size (units); ++i) {
            glActiveTexture (GL_TEXTURE0 + units[i]);
            glGetIntegerv (GL_TEXTURE_BINDING_2D, &textures[i]);
            glGetIntegerv (GL_SAMPLER_BINDING, &samplers[i]);
            glBindSampler (units[i], 0);
        }
        glGetIntegerv (GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv (GL_VIEWPORT, viewport);
        glGetBooleanv (GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv (GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv (GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv (GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv (GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv (GL_BLEND_EQUATION_RGB, &blendEquationRgb);
        glGetIntegerv (GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
        glGetIntegerv (GL_DEPTH_FUNC, &depthFunc);
        glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
        glGetFloatv (GL_COLOR_CLEAR_VALUE, clearColor);
        glGetDoublev (GL_DEPTH_CLEAR_VALUE, &clearDepth);
        for (size_t i = 0; i < enabled.size (); ++i) {
            enabled[i] = glIsEnabled (capabilities[i]);
            glDisable (capabilities[i]);
        }
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask (GL_TRUE);
    }

    ~SavedState () {
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, GLuint (drawFramebuffer));
        glBindFramebuffer (GL_READ_FRAMEBUFFER, GLuint (readFramebuffer));
        glUseProgram (GLuint (program));
        glBindVertexArray (GLuint (vao));
        glBindBuffer (GL_ARRAY_BUFFER, GLuint (arrayBuffer));
        for (size_t i = 0; i < std::size (units); ++i) {
            glActiveTexture (GL_TEXTURE0 + units[i]);
            glBindTexture (GL_TEXTURE_2D, GLuint (textures[i]));
            glBindSampler (units[i], GLuint (samplers[i]));
        }
        glActiveTexture (GLenum (activeTexture));
        glViewport (viewport[0], viewport[1], viewport[2], viewport[3]);
        for (size_t i = 0; i < enabled.size (); ++i)
            enabled[i] ? glEnable (capabilities[i]) : glDisable (capabilities[i]);
        glDepthMask (depthMask);
        glBlendFuncSeparate (GLenum (blendSrcRgb), GLenum (blendDstRgb), GLenum (blendSrcAlpha), GLenum (blendDstAlpha));
        glBlendEquationSeparate (GLenum (blendEquationRgb), GLenum (blendEquationAlpha));
        glDepthFunc (GLenum (depthFunc));
        glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glClearColor (clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glClearDepth (clearDepth);
    }
};

void setMatrix (GLuint program, const char* name, const glm::mat4& value) {
    if (const GLint location = glGetUniformLocation (program, name); location >= 0)
        glUniformMatrix4fv (location, 1, GL_FALSE, glm::value_ptr (value));
}

void setVector (GLuint program, const char* name, const glm::vec4& value) {
    if (const GLint location = glGetUniformLocation (program, name); location >= 0)
        glUniform4fv (location, 1, glm::value_ptr (value));
}

void setSampler (GLuint program, const char* name, int unit) {
    if (const GLint location = glGetUniformLocation (program, name); location >= 0) glUniform1i (location, unit);
}
} // namespace

CVolumetrics::CVolumetrics (const Assets::AssetLocator& assets) : m_assets (assets) {}

CVolumetrics::~CVolumetrics () {
    for (GLuint* framebuffer : {&m_backFramebuffer, &m_limitFramebuffer, &m_lightFramebuffer})
        if (*framebuffer) glDeleteFramebuffers (1, framebuffer);
    for (GLuint* texture : {&m_backTexture, &m_limitTexture, &m_lightTexture})
        if (*texture) glDeleteTextures (1, texture);
    if (m_backDepth) glDeleteRenderbuffers (1, &m_backDepth);
    for (GLuint* buffer : {&m_sphereVertices, &m_sphereIndices, &m_quadVertices})
        if (*buffer) glDeleteBuffers (1, buffer);
    if (m_vao) glDeleteVertexArrays (1, &m_vao);
    for (GLuint program : {m_backProgram, m_frontProgram, m_combineProgram})
        if (program) glDeleteProgram (program);
}

void CVolumetrics::ensurePrograms () {
    if (m_frontProgram) return;
    // the front material's combos for an unshadowed,
    // cookie-less point light at the renderer's volumetric quality.
    const ComboMap combos {{"POINTLIGHT", 1}, {"QUALITY", kVolumetricQuality}};
    const ComboMap overrides;
    const TextureMap textures;
    const ShaderConstantMap constants;
    Shaders::Shader front (m_assets, "volumetricsfront", combos, overrides, textures, textures, constants);
    const auto [vertex, fragment] = Shaders::GLSLContext::get ().toGlsl (
        front.vertex (), withVolumetricIntrinsics (front.fragment ()));
    if (vertex.empty () || fragment.empty ()) throw std::runtime_error ("volumetricsfront did not translate");
    m_frontProgram = link (vertex, fragment);
    m_backProgram = link (kBackVertex, kBackFragment);
    m_combineProgram = link (kCombineVertex, kCombineFragment);

    std::vector<glm::vec3> vertices;
    std::vector<GLuint> indices;
    for (int ring = 0; ring <= kRings; ++ring) {
        const float theta = float (ring) / float (kRings) * glm::pi<float> ();
        for (int segment = 0; segment <= kSegments; ++segment) {
            const float phi = float (segment) / float (kSegments) * glm::two_pi<float> ();
            vertices.emplace_back (std::sin (theta) * std::cos (phi), std::cos (theta), std::sin (theta) * std::sin (phi));
        }
    }
    for (int ring = 0; ring < kRings; ++ring)
        for (int segment = 0; segment < kSegments; ++segment) {
            const GLuint a = GLuint (ring * (kSegments + 1) + segment), b = a + kSegments + 1;
            indices.insert (indices.end (), {a, b, a + 1, a + 1, b, b + 1});
        }
    m_sphereIndexCount = GLsizei (indices.size ());
    glGenVertexArrays (1, &m_vao);
    glBindVertexArray (m_vao);
    glGenBuffers (1, &m_sphereVertices);
    glBindBuffer (GL_ARRAY_BUFFER, m_sphereVertices);
    glBufferData (GL_ARRAY_BUFFER, GLsizeiptr (vertices.size () * sizeof (glm::vec3)), vertices.data (), GL_STATIC_DRAW);
    glGenBuffers (1, &m_sphereIndices);
    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_sphereIndices);
    glBufferData (GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr (indices.size () * sizeof (GLuint)), indices.data (), GL_STATIC_DRAW);
    const float quad[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    glGenBuffers (1, &m_quadVertices);
    glBindBuffer (GL_ARRAY_BUFFER, m_quadVertices);
    glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STATIC_DRAW);
}

void CVolumetrics::ensureTargets (glm::ivec2 targetSize, bool hdr) {
    if (m_lightFramebuffer && targetSize == m_targetSize && hdr == m_hdr) return;
    m_targetSize = targetSize;
    m_hdr = hdr;
    if (!m_lightFramebuffer) {
        glGenFramebuffers (1, &m_backFramebuffer);
        glGenFramebuffers (1, &m_limitFramebuffer);
        glGenFramebuffers (1, &m_lightFramebuffer);
        glGenTextures (1, &m_backTexture);
        glGenTextures (1, &m_limitTexture);
        glGenTextures (1, &m_lightTexture);
        glGenRenderbuffers (1, &m_backDepth);
    }
    // back depth at full size; limit depth and light buffer
    // at the quality divisor; the light buffer is RGBA16F for HDR targets.
    const auto reduced = volumetricBufferSize (targetSize.x, targetSize.y, kVolumetricQuality);
    colorTarget (m_backFramebuffer, m_backTexture, targetSize, GL_R32F, GL_FLOAT, GL_NEAREST);
    glBindRenderbuffer (GL_RENDERBUFFER, m_backDepth);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, targetSize.x, targetSize.y);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_backDepth);
    colorTarget (m_limitFramebuffer, m_limitTexture, reduced, GL_R32F, GL_FLOAT, GL_NEAREST);
    colorTarget (m_lightFramebuffer, m_lightTexture, reduced, hdr ? GL_RGBA16F : GL_RGBA8,
                 hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, GL_LINEAR);
    for (GLuint framebuffer : {m_backFramebuffer, m_limitFramebuffer, m_lightFramebuffer}) {
        glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error ("Volumetric target is incomplete");
    }
}

void CVolumetrics::accumulate (
    const PointVolumeUniforms& volume, const glm::mat4& viewProjection, glm::ivec2 targetSize, bool hdr
) {
    if (targetSize.x <= 0 || targetSize.y <= 0) return;
    SavedState saved;
    ensurePrograms ();
    ensureTargets (targetSize, hdr);
    glBindVertexArray (m_vao);
    const auto reduced = volumetricBufferSize (targetSize.x, targetSize.y, kVolumetricQuality);
    // The shipped front shader reads screen textures at (x, -y) * 0.5 + 0.5;
    // drawing the depth passes with device Y mirrored stores them at that row.
    const glm::mat4 mirrored = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)) * viewProjection;

    // Back pass (volumetrics_back): the far surface of the volume.
    glBindFramebuffer (GL_FRAMEBUFFER, m_backFramebuffer);
    glViewport (0, 0, targetSize.x, targetSize.y);
    glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
    glClearDepth (0.0);
    glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable (GL_DEPTH_TEST);
    glDepthFunc (GL_GREATER);
    glUseProgram (m_backProgram);
    setMatrix (m_backProgram, "g_ViewProjectionMatrix", mirrored);
    setMatrix (m_backProgram, "g_AltViewProjectionMatrix", volume.volume);
    const auto bindSphere = [this] (GLuint program) {
        const GLint position = glGetAttribLocation (program, "a_Position");
        glBindBuffer (GL_ARRAY_BUFFER, m_sphereVertices);
        glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_sphereIndices);
        if (position >= 0) {
            glEnableVertexAttribArray (GLuint (position));
            glVertexAttribPointer (GLuint (position), 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
        return position;
    };
    GLint position = bindSphere (m_backProgram);
    glDrawElements (GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, nullptr);
    if (position >= 0) glDisableVertexAttribArray (GLuint (position));
    glDisable (GL_DEPTH_TEST);

    // _rt_volumetricsSingle: a 2D scene's layers write no depth, so the
    // march limit is the far plane.
    glBindFramebuffer (GL_FRAMEBUFFER, m_limitFramebuffer);
    glViewport (0, 0, reduced.x, reduced.y);
    glClearColor (1.0f, 1.0f, 1.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    // Front pass (volumetrics_front): additive into the batch's light buffer.
    glBindFramebuffer (GL_FRAMEBUFFER, m_lightFramebuffer);
    glViewport (0, 0, reduced.x, reduced.y);
    if (!m_pending) {
        glClearColor (0.0f, 0.0f, 0.0f, 0.0f);
        glClear (GL_COLOR_BUFFER_BIT);
        m_pending = true;
    }
    glEnable (GL_BLEND);
    // Culling stays off: a back-face fragment marches a zero-length ray and
    // adds no colour, but it would add a second alpha of 1. Taking the
    // maximum keeps the native single front-face alpha per volume.
    glBlendEquationSeparate (GL_FUNC_ADD, GL_MAX);
    glBlendFuncSeparate (GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    glUseProgram (m_frontProgram);
    setMatrix (m_frontProgram, "g_ViewProjectionMatrix", viewProjection);
    setMatrix (m_frontProgram, "g_AltViewProjectionMatrix", volume.volume);
    setMatrix (m_frontProgram, "g_EffectModelMatrix", glm::inverse (viewProjection));
    setMatrix (m_frontProgram, "g_AltModelMatrix", glm::mat4 (1.0f));
    setVector (m_frontProgram, "g_RenderVar0", glm::vec4 (0.0f));
    setVector (m_frontProgram, "g_RenderVar1", volume.spotParams);
    setVector (m_frontProgram, "g_RenderVar2", volume.originDensity);
    setVector (m_frontProgram, "g_RenderVar3", glm::vec4 (0.0f));
    setVector (m_frontProgram, "g_RenderVar4", volume.colorExponent);
    setVector (m_frontProgram, "g_Texture1Resolution", glm::vec4 (targetSize, 1.0f / glm::vec2 (targetSize)));
    setVector (m_frontProgram, "g_Texture3Resolution", glm::vec4 (reduced, 1.0f / glm::vec2 (reduced)));
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, m_backTexture);
    glActiveTexture (GL_TEXTURE3);
    glBindTexture (GL_TEXTURE_2D, m_limitTexture);
    setSampler (m_frontProgram, "g_Texture1", 1);
    setSampler (m_frontProgram, "g_Texture3", 3);
    position = bindSphere (m_frontProgram);
    glDrawElements (GL_TRIANGLES, m_sphereIndexCount, GL_UNSIGNED_INT, nullptr);
    if (position >= 0) glDisableVertexAttribArray (GLuint (position));
    glActiveTexture (GL_TEXTURE3);
    glBindTexture (GL_TEXTURE_2D, 0);
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, 0);
}

void CVolumetrics::flush (GLuint framebuffer, glm::ivec2 targetSize) {
    if (!m_pending) return;
    m_pending = false;
    SavedState saved;
    // at quality >= 3: no blur, the reduced buffer is added directly.
    glBindVertexArray (m_vao);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glViewport (0, 0, targetSize.x, targetSize.y);
    glEnable (GL_BLEND);
    glBlendEquation (GL_FUNC_ADD);
    glBlendFuncSeparate (GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    glUseProgram (m_combineProgram);
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_lightTexture);
    setSampler (m_combineProgram, "g_Texture0", 0);
    const GLint position = glGetAttribLocation (m_combineProgram, "a_Position");
    glBindBuffer (GL_ARRAY_BUFFER, m_quadVertices);
    if (position >= 0) {
        glEnableVertexAttribArray (GLuint (position));
        glVertexAttribPointer (GLuint (position), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    glDrawArrays (GL_TRIANGLES, 0, 6);
    if (position >= 0) glDisableVertexAttribArray (GLuint (position));
}
