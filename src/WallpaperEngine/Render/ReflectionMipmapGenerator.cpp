#include "ReflectionMipmapGenerator.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace WallpaperEngine::Render {
namespace {
constexpr const char* vertexSource = R"GLSL(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// Original native GenerateMips uses a 64-source-texel tiled reduction. Odd
// intermediate levels interpolate at LOCAL tile coordinates, then add the
// tile's integer origin. A global image resize has a different sampling phase.
constexpr const char* fragmentSource = R"GLSL(#version 330 core
#extension GL_ARB_shading_language_packing : require
uniform sampler2D sourceTexture;
uniform ivec2 rootSize, sourceSize, destinationSize;
uniform int sourceLevel, destinationLevel;
uniform bool normalized;
out vec4 color;
vec4 halfValue(vec4 x) {
    return vec4(unpackHalf2x16(packHalf2x16(x.xy)),
                unpackHalf2x16(packHalf2x16(x.zw)));
}
vec4 loadPixel(ivec2 p) {
    vec4 value = halfValue(texelFetch(sourceTexture, clamp(p, ivec2(0), sourceSize - 1), sourceLevel));
    // Both even shuffles and compact LDS reloads quantize normalized input.
    // The shared tail's first source is the public image, already converted by
    // hardware imageStore; that distinct conversion is not applied twice.
    if (normalized && destinationLevel != 7)
        value = halfValue(roundEven(halfValue(value * 255.0)) / 255.0);
    return value;
}
vec4 halfMix(vec4 a, vec4 b, float weight) {
    float w = unpackHalf2x16(packHalf2x16(vec2(weight, 0.0))).x;
    return halfValue(mix(a, b, w));
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 result;
    if (destinationLevel == 1) {
        result = halfValue(textureLod(sourceTexture,
            (vec2(p) + 0.5) / vec2(destinationSize), 0.0));
    } else if (destinationLevel != 7 && all(equal(sourceSize % 2, ivec2(0)))) {
        ivec2 base = p * 2;
        vec4 top = halfValue(halfValue(loadPixel(base) * 0.25)
                            + halfValue(loadPixel(base + ivec2(1, 0)) * 0.25));
        vec4 bottom = halfValue(halfValue(loadPixel(base + ivec2(0, 1)) * 0.25)
                               + halfValue(loadPixel(base + ivec2(1, 1)) * 0.25));
        result = halfValue(top + bottom);
    } else {
        ivec2 local = p, origin = ivec2(0);
        if (destinationLevel <= 6) {
            int tile = 1 << (6 - destinationLevel);
            ivec2 group = min(p / tile, max(rootSize / 64, ivec2(1)) - 1);
            local = p - group * tile;
            origin = group * (tile * 2);
        }
        vec2 position = (vec2(local) / vec2(destinationSize)) * vec2(sourceSize) + 0.5;
        ivec2 base = origin + ivec2(position);
        vec4 top = loadPixel(base), bottom = loadPixel(base + ivec2(0, 1));
        if (sourceSize.x > 1) {
            top = halfMix(top, loadPixel(base + ivec2(1, 0)), fract(position.x));
            bottom = halfMix(bottom, loadPixel(base + ivec2(1, 1)), fract(position.x));
        }
        result = sourceSize.y > 1 ? halfMix(top, bottom, fract(position.y)) : top;
    }
    // Native publishes the raw half result through hardware format conversion.
    // Its LDS/shuffle quantization occurs only when the next reduction reads.
    color = result;
}
)GLSL";

GLuint compile (GLenum type, const char* source) {
    GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);
    GLint valid = 0;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &valid);
    if (!valid) {
        std::array<char, 4096> log {};
        glGetShaderInfoLog (shader, log.size (), nullptr, log.data ());
        glDeleteShader (shader);
        throw std::runtime_error (std::string ("Reflection reduction shader: ") + log.data ());
    }
    return shader;
}

struct State {
    GLint program, vertexArray, drawFramebuffer, readFramebuffer, activeTexture, texture, sampler;
    GLint viewport[4];
    GLboolean colorMask[4];
    static constexpr GLenum capabilities[] {GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST,
        GL_SCISSOR_TEST, GL_CULL_FACE, GL_FRAMEBUFFER_SRGB, GL_DITHER, GL_RASTERIZER_DISCARD};
    std::array<GLboolean, std::size (capabilities)> enabled;
    State () {
        glGetIntegerv (GL_CURRENT_PROGRAM, &program);
        glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
        glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
        glGetIntegerv (GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv (GL_VIEWPORT, viewport);
        glGetBooleanv (GL_COLOR_WRITEMASK, colorMask);
        glActiveTexture (GL_TEXTURE0);
        glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegeri_v (GL_SAMPLER_BINDING, 0, &sampler);
        glBindSampler (0, 0);
        for (size_t i = 0; i < enabled.size (); ++i) {
            enabled[i] = glIsEnabled (capabilities[i]);
            glDisable (capabilities[i]);
        }
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    ~State () {
        glUseProgram (program);
        glBindVertexArray (vertexArray);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, drawFramebuffer);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, readFramebuffer);
        glViewport (viewport[0], viewport[1], viewport[2], viewport[3]);
        glColorMask (colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glBindTexture (GL_TEXTURE_2D, texture);
        glBindSampler (0, sampler);
        glActiveTexture (activeTexture);
        for (size_t i = 0; i < enabled.size (); ++i)
            if (enabled[i]) glEnable (capabilities[i]);
    }
};
} // namespace

ReflectionMipmapGenerator::ReflectionMipmapGenerator () {
    const auto vertex = compile (GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = 0;
    try {
        fragment = compile (GL_FRAGMENT_SHADER, fragmentSource);
        m_program = glCreateProgram ();
        glAttachShader (m_program, vertex);
        glAttachShader (m_program, fragment);
        glLinkProgram (m_program);
        GLint linked = 0;
        glGetProgramiv (m_program, GL_LINK_STATUS, &linked);
        if (!linked) throw std::runtime_error ("Reflection reduction program failed to link");
    } catch (...) {
        glDeleteShader (vertex);
        if (fragment) glDeleteShader (fragment);
        if (m_program) glDeleteProgram (m_program);
        throw;
    }
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    glGenVertexArrays (1, &m_vertexArray);
    glGenTextures (2, m_scratch);
    glGenFramebuffers (2, m_framebuffers);
}

ReflectionMipmapGenerator::~ReflectionMipmapGenerator () {
    glDeleteProgram (m_program);
    glDeleteVertexArrays (1, &m_vertexArray);
    glDeleteTextures (2, m_scratch);
    glDeleteFramebuffers (2, m_framebuffers);
}

void ReflectionMipmapGenerator::generate (GLuint texture, uint32_t width, uint32_t height,
                                         uint32_t levels, bool normalized) {
    if (levels <= 1) return;
    State restore;
    if (m_width != width || m_height != height || m_levels != levels) {
        for (const auto scratch : m_scratch) {
            glBindTexture (GL_TEXTURE_2D, scratch);
            for (uint32_t level = 0; level < levels; ++level)
                glTexImage2D (GL_TEXTURE_2D, level, GL_RGBA16F, std::max (1u, width >> level),
                              std::max (1u, height >> level), 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        m_width = width; m_height = height; m_levels = levels;
    }
    glUseProgram (m_program);
    glBindVertexArray (m_vertexArray);
    glUniform1i (glGetUniformLocation (m_program, "sourceTexture"), 0);
    glUniform1i (glGetUniformLocation (m_program, "normalized"), normalized);
    glUniform2i (glGetUniformLocation (m_program, "rootSize"), width, height);
    for (uint32_t level = 1; level < levels; ++level) {
        // Native's compact LDS and public UNORM image store have different
        // rounding. Keep raw half results private, and convert on publication.
        // The shared tail starts from public mip6 after its image-store barrier.
        const auto source = level == 1 || level == 7 ? texture : m_scratch[(level - 2) & 1u];
        const auto destination = m_scratch[(level - 1) & 1u];
        glBindTexture (GL_TEXTURE_2D, source);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_framebuffers[0]);
        glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, level);
        if (glCheckFramebufferStatus (GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error ("Reflection reduction framebuffer is incomplete");
        const auto w = std::max (1u, width >> level), h = std::max (1u, height >> level);
        glViewport (0, 0, w, h);
        glUniform2i (glGetUniformLocation (m_program, "sourceSize"),
                     std::max (1u, width >> (level - 1)), std::max (1u, height >> (level - 1)));
        glUniform2i (glGetUniformLocation (m_program, "destinationSize"), w, h);
        glUniform1i (glGetUniformLocation (m_program, "sourceLevel"), level - 1);
        glUniform1i (glGetUniformLocation (m_program, "destinationLevel"), level);
        glDrawArrays (GL_TRIANGLES, 0, 3);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, m_framebuffers[1]);
        glFramebufferTexture2D (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination, level);
        glFramebufferTexture2D (GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
        glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}
} // namespace WallpaperEngine::Render
