#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/FBOProvider.h"
#include "WallpaperEngine/Render/CTexture.h"
#include "WallpaperEngine/Render/EffectClearAction.h"
#include "WallpaperEngine/Render/Objects/Effects/UniformArrayUpload.h"
#include "WallpaperEngine/Render/UserTextureSelection.h"
#include "WallpaperEngine/Data/Parsers/EffectParser.h"
#include "WallpaperEngine/Render/Objects/ImageCompositeSteps.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Model/Material.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <glm/gtc/matrix_transform.hpp>

using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Render;
using WallpaperEngine::Data::Parsers::EffectParser;

namespace {
struct SurfacelessGL {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool ready = false;

    SurfacelessGL () {
        display = eglGetPlatformDisplay (EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize (display, nullptr, nullptr)) return;
        if (!eglBindAPI (EGL_OPENGL_API)) return;
        const EGLint configAttributes[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint configCount = 0;
        if (!eglChooseConfig (display, configAttributes, &config, 1, &configCount) || configCount != 1) return;
        const EGLint pbufferAttributes[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
        surface = eglCreatePbufferSurface (display, config, pbufferAttributes);
        if (surface == EGL_NO_SURFACE) return;
        const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
        context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextAttributes);
        if (context == EGL_NO_CONTEXT || !eglMakeCurrent (display, surface, surface, context)) return;
        glewExperimental = GL_TRUE;
        (void) glewInit ();
        while (glGetError () != GL_NO_ERROR) { }
        ready = glGenFramebuffers != nullptr && glClearBufferfv != nullptr;
    }

    ~SurfacelessGL () {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext (display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface (display, surface);
        eglTerminate (display);
    }
};

GLint internalFormat (const CFBO& fbo) {
    GLint internal = 0;
    glBindTexture (GL_TEXTURE_2D, fbo.getTextureID (0));
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
    return internal;
}

TEST_CASE ("Uniform array upload reaches second bone matrix and blend row", "[render][uniform-array]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    const auto compile = [] (GLenum stage, const char* source) {
        const GLuint shader = glCreateShader (stage);
        glShaderSource (shader, 1, &source, nullptr);
        glCompileShader (shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
        REQUIRE (compiled == GL_TRUE);
        return shader;
    };
    const GLuint vertex = compile (GL_VERTEX_SHADER, R"(
        #version 330 core
        uniform mat4 uBones[2];
        void main() {
            vec2 p[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
            gl_Position = uBones[1] * vec4(p[gl_VertexID], 0, 1);
        })");
    const GLuint fragment = compile (GL_FRAGMENT_SHADER, R"(
        #version 330 core
        uniform vec4 uRows[2];
        out vec4 color;
        void main() { color = uRows[1]; })");
    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    GLint linked = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &linked);
    REQUIRE (linked == GL_TRUE);
    glUseProgram (program);
    const GLint bones = glGetUniformLocation (program, "uBones[0]");
    const GLint bone1 = glGetUniformLocation (program, "uBones[1]");
    const GLint rows = glGetUniformLocation (program, "uRows[0]");
    REQUIRE (bones >= 0);
    REQUIRE (bone1 >= 0);
    REQUIRE (rows >= 0);
    const glm::mat4 matrices[2] = {glm::mat4 (1.0f), glm::scale (glm::mat4 (1.0f), glm::vec3 (0.5f))};
    const glm::vec4 colors[2] = {glm::vec4 (1, 0, 0, 1), glm::vec4 (0, 1, 0, 1)};
    WallpaperEngine::Render::Objects::Effects::uploadMat4Array (bones, matrices, 2);
    WallpaperEngine::Render::Objects::Effects::uploadVec4Array (rows, colors, 2);
    std::array<float, 16> secondMatrix {};
    glGetUniformfv (program, bone1, secondMatrix.data ());
    REQUIRE (secondMatrix[0] == Catch::Approx (0.5f));
    REQUIRE (secondMatrix[5] == Catch::Approx (0.5f));
    GLuint vao = 0;
    glGenVertexArrays (1, &vao);
    glBindVertexArray (vao);
    glViewport (0, 0, 4, 4);
    glClearColor (0, 0, 0, 1);
    glClear (GL_COLOR_BUFFER_BIT);
    glDrawArrays (GL_TRIANGLES, 0, 3);
    std::array<unsigned char, 4> pixel {};
    glReadPixels (2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data ());
    REQUIRE (pixel[0] == 0);
    REQUIRE (pixel[1] == 255);
    REQUIRE (pixel[2] == 0);
    REQUIRE (glGetError () == GL_NO_ERROR);
    glDeleteVertexArrays (1, &vao);
    glDeleteProgram (program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
}

bool framebufferComplete (const CFBO& fbo) {
    glBindFramebuffer (GL_FRAMEBUFFER, fbo.getFramebuffer ());
    return glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

std::array<float, 4> clearAndRead (const CFBO& fbo, GLenum channels,
                                  const std::array<float, 4>& color) {
    glBindFramebuffer (GL_FRAMEBUFFER, fbo.getFramebuffer ());
    glClearBufferfv (GL_COLOR, 0, color.data ());
    std::array<float, 4> pixel {};
    glReadPixels (0, 0, 1, 1, channels, GL_FLOAT, pixel.data ());
    return pixel;
}

float readRed (const CFBO& fbo, GLint x = 0, GLint y = 0) {
    glBindFramebuffer (GL_FRAMEBUFFER, fbo.getFramebuffer ());
    float pixel = 0.0f;
    glReadPixels (x, y, 1, 1, GL_RED, GL_FLOAT, &pixel);
    return pixel;
}

std::array<float, 4> readRGBA (const CFBO& fbo, GLint x = 0, GLint y = 0) {
    glBindFramebuffer (GL_FRAMEBUFFER, fbo.getFramebuffer ());
    std::array<float, 4> pixel {};
    glReadPixels (x, y, 1, 1, GL_RGBA, GL_FLOAT, pixel.data ());
    return pixel;
}
} // namespace

TEST_CASE ("Scene D16 depth target remains complete through resize", "[render][depth]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    CFBO scene ("_rt_FullFrameBuffer", TextureFormat_ARGB8888,
                TextureFlags_ClampUVs, 1.0f, 8, 6, 8, 6);
    CFBO effect ("effect", TextureFormat_ARGB8888,
                 TextureFlags_ClampUVs, 1.0f, 8, 6, 8, 6);
    REQUIRE (scene.getDepthbuffer () == GL_NONE);
    REQUIRE (effect.getDepthbuffer () == GL_NONE);
    scene.attachDepth16 ();
    REQUIRE (scene.getDepthbuffer () != GL_NONE);
    REQUIRE (framebufferComplete (scene));
    glBindRenderbuffer (GL_RENDERBUFFER, scene.getDepthbuffer ());
    GLint format = 0;
    glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
    REQUIRE (format == GL_DEPTH_COMPONENT16);
    glBindFramebuffer (GL_FRAMEBUFFER, scene.getFramebuffer ());
    glDepthMask (GL_TRUE);
    glClearDepth (0.25);
    glClear (GL_DEPTH_BUFFER_BIT);
    float depth = 0;
    glReadPixels (0, 0, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
    REQUIRE (depth == Catch::Approx (0.25f).margin (0.0001f));
    glBindRenderbuffer (GL_RENDERBUFFER, scene.getDepthbuffer ());
    scene.resize (11, 7, 11, 7);
    REQUIRE (framebufferComplete (scene));
    GLint boundRenderbuffer = 0;
    glGetIntegerv (GL_RENDERBUFFER_BINDING, &boundRenderbuffer);
    REQUIRE (boundRenderbuffer == static_cast<GLint> (scene.getDepthbuffer ()));
    glBindRenderbuffer (GL_RENDERBUFFER, scene.getDepthbuffer ());
    GLint width = 0;
    GLint height = 0;
    glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
    glGetRenderbufferParameteriv (GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
    REQUIRE (width == 11);
    REQUIRE (height == 7);
    GLuint unrelated = GL_NONE;
    glGenRenderbuffers (1, &unrelated);
    glBindRenderbuffer (GL_RENDERBUFFER, unrelated);
    scene.resize (13, 9, 13, 9);
    glGetIntegerv (GL_RENDERBUFFER_BINDING, &boundRenderbuffer);
    REQUIRE (boundRenderbuffer == static_cast<GLint> (unrelated));
    REQUIRE (framebufferComplete (scene));
    glDeleteRenderbuffers (1, &unrelated);
    REQUIRE (effect.getDepthbuffer () == GL_NONE);
    REQUIRE (glGetError () == GL_NO_ERROR);
}

TEST_CASE ("HDR sampler anisotropy survives framebuffer resize", "[render][hdr-sampler]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    CFBO target ("hdr-sampler", TextureFormat_RGBA16161616f,
                 TextureFlags_ClampUVs, 1.0f, 4, 4, 4, 4);
    target.setMaxAnisotropy (1.0f);
    const auto measured = [&] {
        GLint previous = 0;
        glGetIntegerv (GL_TEXTURE_BINDING_2D, &previous);
        glBindTexture (GL_TEXTURE_2D, target.getTextureID (0));
        float value = 0.0f;
        glGetTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, &value);
        glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (previous));
        return value;
    };
    REQUIRE (measured () == Catch::Approx (1.0f));
    target.resize (5, 3, 5, 3);
    REQUIRE (measured () == Catch::Approx (1.0f));
}

TEST_CASE ("File texture upload reads odd RG8 rows independently of prior pixel-store state", "[render][texture]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);

    const std::array<unsigned char, 12> pixels = {1, 2, 3, 4, 5, 6, 101, 102, 103, 104, 105, 106};
    Mipmap mipmap;
    mipmap.width = 3;
    mipmap.height = 2;
    mipmap.uncompressedSize = pixels.size ();
    mipmap.uncompressedData = std::make_unique<char[]> (pixels.size ());
    std::memcpy (mipmap.uncompressedData.get (), pixels.data (), pixels.size ());
    Texture rg;
    rg.format = TextureFormat_RG88;

    GLuint textures[4] = {};
    glGenTextures (4, textures);
    GLuint framebuffer = 0;
    glGenFramebuffers (1, &framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);

    const auto uploadAndRead = [&] (GLuint texture) {
	glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 5);
	CTexture::uploadLevel (rg, mipmap, GL_RG8, texture, 0, "odd-rg8.tex");
	GLint alignment = 0, rowLength = 0;
	glGetIntegerv (GL_UNPACK_ALIGNMENT, &alignment);
	glGetIntegerv (GL_UNPACK_ROW_LENGTH, &rowLength);
	REQUIRE (alignment == 4);
	REQUIRE (rowLength == 5);
	glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	REQUIRE (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	std::array<unsigned char, 2> bottom {}, top {};
	glReadPixels (0, 0, 1, 1, GL_RG, GL_UNSIGNED_BYTE, bottom.data ());
	glReadPixels (0, 1, 1, 1, GL_RG, GL_UNSIGNED_BYTE, top.data ());
	REQUIRE (bottom[0] == 1);
	REQUIRE (bottom[1] == 2);
	REQUIRE (top[0] == 101);
	REQUIRE (top[1] == 102);
    };

    uploadAndRead (textures[0]);
    Texture red;
    red.format = TextureFormat_R8;
    CTexture::uploadLevel (red, mipmap, GL_R8, textures[1], 0, "red-before-rg.tex");
    uploadAndRead (textures[2]);

    Texture rgb;
    rgb.format = TextureFormat_RGB888;
    Mipmap oddRGB;
    oddRGB.width = 1;
    oddRGB.height = 2;
    oddRGB.uncompressedSize = 6;
    oddRGB.uncompressedData = std::make_unique<char[]> (6);
    const std::array<unsigned char, 6> rgbPixels = {10, 20, 30, 40, 50, 60};
    std::memcpy (oddRGB.uncompressedData.get (), rgbPixels.data (), rgbPixels.size ());
    CTexture::uploadLevel (rgb, oddRGB, GL_RGB8, textures[3], 0, "odd-rgb.tex");
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[3], 0);
    REQUIRE (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    std::array<unsigned char, 3> rgbBottom {}, rgbTop {};
    glReadPixels (0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgbBottom.data ());
    glReadPixels (0, 1, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, rgbTop.data ());
    REQUIRE (rgbBottom == std::array<unsigned char, 3> {10, 20, 30});
    REQUIRE (rgbTop == std::array<unsigned char, 3> {40, 50, 60});

    Texture encoded;
    encoded.freeImageFormat = FIF_PNG;
    encoded.format = TextureFormat_ARGB8888;
    std::string decodeError;
    try {
        CTexture::uploadLevel (encoded, mipmap, GL_RGBA8, textures[1], 0, "bad-png.tex");
    } catch (const std::exception& error) {
        decodeError = error.what ();
    }
    REQUIRE (decodeError.find ("bad-png.tex") != std::string::npos);
    mipmap.uncompressedSize = 11;
    REQUIRE_THROWS (CTexture::uploadLevel (rg, mipmap, GL_RG8, textures[1], 0, "truncated-rg.tex"));
    REQUIRE (glGetError () == GL_NO_ERROR);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (4, textures);
}

TEST_CASE ("File texture upload preserves half-float values outside normalized range", "[render][texture]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);

    Texture half;
    half.format = TextureFormat_R16f;
    Mipmap mipmap;
    mipmap.width = 2;
    mipmap.height = 1;
    mipmap.uncompressedSize = 4;
    mipmap.uncompressedData = std::make_unique<char[]> (4);
    const std::array<unsigned char, 4> pixels = {0x00, 0xb8, 0x00, 0x40}; // -0.5, 2.0 as IEEE binary16
    std::memcpy (mipmap.uncompressedData.get (), pixels.data (), pixels.size ());

    GLuint texture = 0, framebuffer = 0;
    glGenTextures (1, &texture);
    glGenFramebuffers (1, &framebuffer);
    CTexture::uploadLevel (half, mipmap, GL_R16F, texture, 0, "signed-half.tex");
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    REQUIRE (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    std::array<float, 2> values {};
    glReadPixels (0, 0, 1, 1, GL_RED, GL_FLOAT, &values[0]);
    glReadPixels (1, 0, 1, 1, GL_RED, GL_FLOAT, &values[1]);
    REQUIRE (values[0] == Catch::Approx (-0.5f).margin (0.01f));
    REQUIRE (values[1] == Catch::Approx (2.0f).margin (0.01f));
    REQUIRE (glGetError () == GL_NO_ERROR);
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (1, &texture);
}

TEST_CASE ("Packed 10:10:10:2 file texture preserves channel order and alpha steps", "[render][texture]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    Texture packed;
    packed.format = TextureFormat_RGBa1010102;
    REQUIRE (CTexture::storageFormat (packed) == GL_RGB10_A2);
    const std::array<uint32_t, 4> pixels = {
        1023u | (0u << 10) | (0u << 20) | (0u << 30),
        0u | (1023u << 10) | (0u << 20) | (1u << 30),
        0u | (0u << 10) | (1023u << 20) | (2u << 30),
        341u | (682u << 10) | (170u << 20) | (3u << 30)
    };
    Mipmap mipmap;
    mipmap.width = 2;
    mipmap.height = 2;
    mipmap.uncompressedSize = sizeof (pixels);
    mipmap.uncompressedData = std::make_unique<char[]> (sizeof (pixels));
    std::memcpy (mipmap.uncompressedData.get (), pixels.data (), sizeof (pixels));
    GLuint texture = 0, framebuffer = 0;
    glGenTextures (1, &texture);
    glGenFramebuffers (1, &framebuffer);
    CTexture::uploadLevel (packed, mipmap, CTexture::storageFormat (packed), texture, 0, "packed10.tex");
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    REQUIRE (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    const std::array<std::array<float, 4>, 4> expected = {{
        {1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f / 3.0f},
        {0.0f, 0.0f, 1.0f, 2.0f / 3.0f}, {341.0f / 1023.0f, 682.0f / 1023.0f, 170.0f / 1023.0f, 1.0f}
    }};
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
        std::array<float, 4> actual {};
        glReadPixels (x, y, 1, 1, GL_RGBA, GL_FLOAT, actual.data ());
        for (int channel = 0; channel < 4; ++channel)
            REQUIRE (actual[channel] == Catch::Approx (expected[y * 2 + x][channel]).margin (0.005f));
    }
    REQUIRE (glGetError () == GL_NO_ERROR);
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (1, &texture);
}

TEST_CASE ("BC7 file texture uses exact ceil-block lengths on odd mip sizes", "[render][texture]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    Texture bc7;
    bc7.format = TextureFormat_BC7;
    REQUIRE (CTexture::storageFormat (bc7) == GL_COMPRESSED_RGBA_BPTC_UNORM);
    GLuint texture = 0;
    glGenTextures (1, &texture);
    Mipmap mipmap;
    mipmap.width = 5;
    mipmap.height = 3;
    mipmap.uncompressedSize = 32;
    mipmap.uncompressedData = std::make_unique<char[]> (32);
    // A mode-6 block with equal endpoints decodes to (200, 100, 50, 240).
    // Build its bits explicitly so this tests sampling, not just compressed storage.
    const auto solidBlock = [] {
        std::array<unsigned char, 16> block {};
        unsigned bit = 0;
        const auto put = [&] (unsigned value, unsigned count) {
            for (unsigned i = 0; i < count; ++i, ++bit)
                block[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
        };
        put (1u << 6, 7); // mode 6 selector
        for (unsigned channel : {100u, 50u, 25u, 120u}) {
            put (channel, 7); // endpoint 0
            put (channel, 7); // endpoint 1
        }
        put (0, 1); // endpoint 0 parity bit
        put (0, 1); // endpoint 1 parity bit
        return block;
    } ();
    std::memcpy (mipmap.uncompressedData.get (), solidBlock.data (), 16);
    std::memcpy (mipmap.uncompressedData.get () + 16, solidBlock.data (), 16);
    if (!GLEW_VERSION_4_2 && !GLEW_ARB_texture_compression_bptc) {
        std::string diagnostic;
        try { CTexture::uploadLevel (bc7, mipmap, CTexture::storageFormat (bc7), texture, 0, "bc7.tex"); }
        catch (const std::exception& error) { diagnostic = error.what (); }
        REQUIRE (diagnostic.find ("BC7 compression is unavailable") != std::string::npos);
    } else {
        CTexture::uploadLevel (bc7, mipmap, CTexture::storageFormat (bc7), texture, 0, "bc7.tex");
        GLint internal = 0, compressed = 0, bytes = 0;
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED, &compressed);
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &bytes);
        REQUIRE (internal == GL_COMPRESSED_RGBA_BPTC_UNORM);
        REQUIRE (compressed == GL_TRUE);
        REQUIRE (bytes == 32);
        std::array<unsigned char, 5 * 3 * 4> decoded {};
        glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, decoded.data ());
        for (size_t pixel = 0; pixel < 15; ++pixel) {
            REQUIRE (decoded[pixel * 4 + 0] == 200);
            REQUIRE (decoded[pixel * 4 + 1] == 100);
            REQUIRE (decoded[pixel * 4 + 2] == 50);
            REQUIRE (decoded[pixel * 4 + 3] == 240);
        }
        mipmap.uncompressedSize = 31;
        REQUIRE_THROWS (CTexture::uploadLevel (bc7, mipmap, CTexture::storageFormat (bc7), texture, 1,
                                               "truncated-bc7.tex"));
        mipmap.width = 2;
        mipmap.height = 1;
        mipmap.uncompressedSize = 16;
        CTexture::uploadLevel (bc7, mipmap, CTexture::storageFormat (bc7), texture, 1, "bc7-mip1.tex");
        glGetTexLevelParameteriv (GL_TEXTURE_2D, 1, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, &bytes);
        REQUIRE (bytes == 16);
    }
    REQUIRE (glGetError () == GL_NO_ERROR);
    glDeleteTextures (1, &texture);
}

TEST_CASE ("File texture border clamp samples transparent black outside UV bounds", "[render][texture]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    const auto compile = [] (GLenum type, const char* source) {
        const GLuint shader = glCreateShader (type);
        glShaderSource (shader, 1, &source, nullptr);
        glCompileShader (shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv (shader, GL_COMPILE_STATUS, &compiled);
        REQUIRE (compiled == GL_TRUE);
        return shader;
    };
    const GLuint vertex = compile (GL_VERTEX_SHADER,
        "#version 330 core\n"
        "void main() { vec2 p[3] = vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));"
        " gl_Position = vec4(p[gl_VertexID],0,1); }\n");
    const GLuint fragment = compile (GL_FRAGMENT_SHADER,
        "#version 330 core\n"
        "uniform sampler2D uTexture; out vec4 pixel;"
        " void main() { pixel = texture(uTexture, vec2(-0.5,0.5)); }\n");
    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    GLint linked = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &linked);
    REQUIRE (linked == GL_TRUE);

    GLuint input = 0, output = 0, framebuffer = 0, vertexArray = 0;
    glGenTextures (1, &input);
    glGenTextures (1, &output);
    glGenFramebuffers (1, &framebuffer);
    glGenVertexArrays (1, &vertexArray);
    glBindTexture (GL_TEXTURE_2D, input);
    const std::array<unsigned char, 4> red = {255, 0, 0, 255};
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, red.data ());
    glBindTexture (GL_TEXTURE_2D, output);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0);
    REQUIRE (glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    Texture authored;
    authored.images[0].push_back (std::make_shared<Mipmap> ());
    authored.flags = TextureFlags_ClampUVsBorder | TextureFlags_NoInterpolation;
    const auto sample = [&] () {
        CTexture::configureSampling (authored, 0, input);
        glViewport (0, 0, 1, 1);
        glDisable (GL_BLEND);
        glDisable (GL_DEPTH_TEST);
        glDisable (GL_SCISSOR_TEST);
        glUseProgram (program);
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, input);
        glUniform1i (glGetUniformLocation (program, "uTexture"), 0);
        glBindVertexArray (vertexArray);
        glDrawArrays (GL_TRIANGLES, 0, 3);
        std::array<unsigned char, 4> pixel {};
        glReadPixels (0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data ());
        return pixel;
    };
    REQUIRE (sample () == std::array<unsigned char, 4> {0, 0, 0, 0});
    authored.flags = TextureFlags_ClampUVs | TextureFlags_NoInterpolation;
    REQUIRE (sample () == red);
    REQUIRE (glGetError () == GL_NO_ERROR);

    glDeleteVertexArrays (1, &vertexArray);
    glDeleteProgram (program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    glDeleteFramebuffers (1, &framebuffer);
    glDeleteTextures (1, &input);
    glDeleteTextures (1, &output);
}

TEST_CASE ("Effect FBOs allocate authored channel and floating-point storage", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);

    FBOProvider provider (nullptr);
    const auto make = [&] (const char* name, const char* format) {
        return provider.create (FBO {.name = name, .format = format, .scale = 1.0f, .unique = false},
                                TextureFlags_ClampUVs, {4.0f, 3.0f});
    };

    auto rgba = make ("rgba", "rgba8888");
    REQUIRE (rgba->getFormat () == TextureFormat_ARGB8888);
    REQUIRE (internalFormat (*rgba) == GL_RGBA8);
    REQUIRE (framebufferComplete (*rgba));
    GLint depthAttachmentType = -1;
    glGetFramebufferAttachmentParameteriv (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                           GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthAttachmentType);
    REQUIRE (depthAttachmentType == GL_NONE);
    REQUIRE (rgba->getDepthbuffer () == GL_NONE);
    REQUIRE (rgba->getTextureWidth (0) == 4);
    REQUIRE (rgba->getTextureHeight (0) == 3);
    const auto normalized = clearAndRead (*rgba, GL_RGBA, {-0.5f, 1.5f, 0.25f, 1.0f});
    REQUIRE (normalized[0] == Catch::Approx (0.0f).margin (0.01f));
    REQUIRE (normalized[1] == Catch::Approx (1.0f).margin (0.01f));

    auto r8 = make ("r8", "r8");
    REQUIRE (r8->getFormat () == TextureFormat_R8);
    REQUIRE (internalFormat (*r8) == GL_R8);
    REQUIRE (framebufferComplete (*r8));
    const auto narrow = clearAndRead (*r8, GL_RED, {0.25f, 0.0f, 0.0f, 0.0f});
    REQUIRE (narrow[0] == Catch::Approx (0.25f).margin (0.01f));

    auto rg8 = make ("rg8", "rg88");
    REQUIRE (rg8->getFormat () == TextureFormat_RG88);
    REQUIRE (internalFormat (*rg8) == GL_RG8);
    REQUIRE (framebufferComplete (*rg8));
    const auto narrowRG = clearAndRead (*rg8, GL_RG, {0.25f, 0.75f, 0.0f, 0.0f});
    REQUIRE (narrowRG[0] == Catch::Approx (0.25f).margin (0.01f));
    REQUIRE (narrowRG[1] == Catch::Approx (0.75f).margin (0.01f));

    auto r16 = make ("r16", "r16f");
    REQUIRE (r16->getFormat () == TextureFormat_R16f);
    REQUIRE (internalFormat (*r16) == GL_R16F);
    REQUIRE (framebufferComplete (*r16));
    const auto negative = clearAndRead (*r16, GL_RED, {-0.5f, 0.0f, 0.0f, 0.0f});
    REQUIRE (negative[0] == Catch::Approx (-0.5f).margin (0.01f));

    auto rg16 = make ("rg16", "rg1616f");
    REQUIRE (rg16->getFormat () == TextureFormat_RG1616f);
    REQUIRE (internalFormat (*rg16) == GL_RG16F);
    REQUIRE (framebufferComplete (*rg16));
    const auto signedHDR = clearAndRead (*rg16, GL_RG, {-0.75f, 2.5f, 0.0f, 0.0f});
    REQUIRE (signedHDR[0] == Catch::Approx (-0.75f).margin (0.01f));
    REQUIRE (signedHDR[1] == Catch::Approx (2.5f).margin (0.01f));

    const auto explicitFormat = provider.create (
        "explicit", TextureFormat_R16f, TextureFlags_ClampUVs, 1.0f,
        {4.0f, 3.0f}, {4.0f, 3.0f});
    REQUIRE (explicitFormat->getFormat () == TextureFormat_R16f);
    REQUIRE (internalFormat (*explicitFormat) == GL_R16F);
    REQUIRE (framebufferComplete (*explicitFormat));
    REQUIRE (glGetError () == GL_NO_ERROR);
}

TEST_CASE ("Backbuffer effect format follows the provider output descriptor", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);

    const FBO authored {.name = "backbuffer", .format = "rgba_backbuffer", .scale = 1.0f, .unique = false};
    FBOProvider standardOutput (nullptr);
    const auto standard = standardOutput.create (authored, 0, {2.0f, 2.0f});
    REQUIRE (standard->getFormat () == TextureFormat_ARGB8888);
    REQUIRE (internalFormat (*standard) == GL_RGBA8);
    const auto standardRGB = standardOutput.create (
        FBO {.name = "rgb-backbuffer", .format = "rgb_backbuffer", .scale = 1.0f, .unique = false},
        0, {2.0f, 2.0f});
    REQUIRE (standardRGB->getFormat () == TextureFormat_RGB888);
    REQUIRE (internalFormat (*standardRGB) == GL_RGB8);
    REQUIRE (framebufferComplete (*standardRGB));

    FBOProvider hdrOutput (nullptr, TextureFormat_RGBA16161616f);
    FBOProvider inherited (&hdrOutput);
    const auto hdr = inherited.create (authored, 0, {2.0f, 2.0f});
    REQUIRE (hdr->getFormat () == TextureFormat_RGBA16161616f);
    REQUIRE (internalFormat (*hdr) == GL_RGBA16F);
    REQUIRE (framebufferComplete (*hdr));
    const auto signedHDR = clearAndRead (*hdr, GL_RGBA, {-0.5f, 2.0f, 0.0f, 1.0f});
    REQUIRE (signedHDR[0] == Catch::Approx (-0.5f).margin (0.01f));
    REQUIRE (signedHDR[1] == Catch::Approx (2.0f).margin (0.01f));
    const auto hdrRGB = inherited.create (
        FBO {.name = "rgb-hdr", .format = "rgb_backbuffer", .scale = 1.0f, .unique = false},
        0, {2.0f, 2.0f});
    REQUIRE (hdrRGB->getFormat () == TextureFormat_RGB161616f);
    REQUIRE (internalFormat (*hdrRGB) == GL_RGB16F);
    REQUIRE (framebufferComplete (*hdrRGB));
    const auto signedRGB = clearAndRead (*hdrRGB, GL_RGB, {-0.25f, 1.75f, 0.5f, 0.0f});
    REQUIRE (signedRGB[0] == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE (signedRGB[1] == Catch::Approx (1.75f).margin (0.01f));

    REQUIRE_THROWS_AS (standardOutput.create (
        FBO {.name = "unknown", .format = "unsupported", .scale = 1.0f, .unique = false},
        0, {2.0f, 2.0f}), std::invalid_argument);
}

TEST_CASE ("Authored float clear initializes once and survives unrelated framebuffer binds", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider provider (nullptr);
    const FBO descriptor {.name = "persistent", .format = "rgba16161616f", .scale = 1.0f,
                          .unique = true, .clear = glm::vec4 (-0.25f, 2.5f, 0.5f, 1.0f)};
    glEnable (GL_SCISSOR_TEST);
    glScissor (0, 0, 1, 1);
    glColorMask (GL_TRUE, GL_FALSE, GL_FALSE, GL_FALSE);
    const auto target = provider.create (descriptor, 0, {4.0f, 3.0f});
    REQUIRE (glIsEnabled (GL_SCISSOR_TEST) == GL_TRUE);
    GLboolean restoredMask[4] = {};
    glGetBooleanv (GL_COLOR_WRITEMASK, restoredMask);
    REQUIRE (restoredMask[0] == GL_TRUE);
    REQUIRE (restoredMask[1] == GL_FALSE);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    REQUIRE (framebufferComplete (*target));
    const auto initial = readRGBA (*target, 3, 2);
    REQUIRE (initial[0] == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE (initial[1] == Catch::Approx (2.5f).margin (0.01f));

    const auto other = provider.create ("unrelated", TextureFormat_ARGB8888, 0, 1.0f,
                                        {4.0f, 3.0f}, {4.0f, 3.0f});
    clearAndRead (*other, GL_RGBA, {1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE (readRGBA (*target)[0] == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE (readRGBA (*target)[1] == Catch::Approx (2.5f).margin (0.01f));

    target->clear (glm::vec4 (-0.75f, 3.5f, 0.0f, 1.0f));
    REQUIRE (readRGBA (*target)[0] == Catch::Approx (-0.75f).margin (0.01f));
    REQUIRE (readRGBA (*target)[1] == Catch::Approx (3.5f).margin (0.01f));
    REQUIRE (glGetError () == GL_NO_ERROR);
}

TEST_CASE ("Scene texture selection preserves the authored target across empty and rejected values", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider provider (nullptr);
    const auto authored = provider.create ("authored", TextureFormat_R8, 0, 1.0f, {4, 3}, {4, 3});
    const auto selected = provider.create ("selected", TextureFormat_R8, 0, 1.0f, {7, 5}, {7, 5});
    PropertySceneTexture property (PropertyData {.name = "sceneTexture", .text = "Texture"}, "");
    std::string cachedValue;
    std::shared_ptr<const TextureProvider> cachedTexture;
    int loads = 0;
    const auto resolve = [&] {
        return resolveUserTextureSelection (
            property, cachedValue, cachedTexture,
            [&] (const std::string& name) -> std::shared_ptr<const TextureProvider> {
                ++loads;
                return name == "valid" ? selected : nullptr;
            },
            [&] () -> std::shared_ptr<const TextureProvider> { return authored; }
        );
    };

    REQUIRE (resolve () == authored);
    REQUIRE (loads == 0);
    property.update ("valid", DynamicValue::UpdateSource::User);
    REQUIRE (resolve () == selected);
    REQUIRE (resolve ()->getTextureWidth (0) == 7);
    REQUIRE (resolve ()->getTextureHeight (0) == 5);
    REQUIRE (loads == 1);
    property.update ("", DynamicValue::UpdateSource::User);
    REQUIRE (resolve () == authored);
    REQUIRE (resolve ()->getTextureWidth (0) == 4);
    property.update ("missing", DynamicValue::UpdateSource::User);
    REQUIRE (resolve () == authored);
    REQUIRE (loads == 2);
    property.update ("valid", DynamicValue::UpdateSource::User);
    REQUIRE (resolve () == selected);
    REQUIRE (loads == 3);
}

TEST_CASE ("Target dimensions stay valid at small scales and reject invalid input", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider provider (nullptr);
    const FBO small {.name = "small", .format = "r16f", .scale = 8.0f, .unique = false};
    const auto target = provider.create (small, 0, {4.0f, 3.0f});
    REQUIRE (target->getTextureWidth (0) == 2);
    REQUIRE (target->getTextureHeight (0) == 2);
    REQUIRE (target->getRealWidth () == 1);
    REQUIRE (target->getRealHeight () == 1);
    REQUIRE (framebufferComplete (*target));
    const auto explicitTarget = provider.create (
        FBO {.name = "explicit-size", .format = "r8", .scale = 1.0f, .unique = false,
             .width = 7, .height = 5, .uvs = "repeat"},
        TextureFlags_ClampUVs, {100.0f, 80.0f});
    REQUIRE (explicitTarget->getTextureWidth (0) == 7);
    REQUIRE (explicitTarget->getTextureHeight (0) == 5);
    REQUIRE ((explicitTarget->getFlags () & TextureFlags_ClampUVs) == 0);
    glBindTexture (GL_TEXTURE_2D, explicitTarget->getTextureID (0));
    GLint wrap = 0;
    glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap);
    REQUIRE (wrap == GL_REPEAT);
    const auto original = provider.find ("small");

    REQUIRE_THROWS_AS (provider.create (
        FBO {.name = "small", .format = "r16f", .scale = 0.0f, .unique = false},
        0, {4.0f, 3.0f}), std::invalid_argument);
    REQUIRE_THROWS_AS (provider.create (
        FBO {.name = "small", .format = "r16f", .scale = 1.0f, .unique = false},
        0, {std::numeric_limits<float>::quiet_NaN (), 3.0f}), std::invalid_argument);
    const auto oneAxis = provider.create (
        FBO {.name = "one-axis", .format = "r8", .scale = 1.0f, .unique = false, .width = 7},
        0, {4.0f, 3.0f});
    REQUIRE (oneAxis->getTextureWidth (0) == 7);
    REQUIRE (oneAxis->getTextureHeight (0) == 3);
    const auto fitted = provider.create (
        FBO {.name = "fit", .format = "rg88", .scale = 2.0f, .unique = false, .fit = 256},
        0, {400.0f, 200.0f});
    REQUIRE (fitted->getTextureWidth (0) == 128);
    REQUIRE (fitted->getTextureHeight (0) == 64);
    const auto truncationOrder = provider.create (
        FBO {.name = "fit-before-scale", .format = "r8", .scale = 1.05f, .unique = false, .fit = 4},
        0, {5.0f, 4.0f});
    REQUIRE (truncationOrder->getTextureWidth (0) == 3);
    REQUIRE (truncationOrder->getTextureHeight (0) == 2);
    const auto thin = provider.create (
        FBO {.name = "thin-fit", .format = "r8", .scale = 1.0f, .unique = false, .fit = 256},
        0, {4096.0f, 1.0f});
    REQUIRE (thin->getTextureWidth (0) == 256);
    REQUIRE (thin->getTextureHeight (0) == 2);
    REQUIRE (thin->getRealHeight () == 1);
    REQUIRE (provider.find ("small") == original);
}

TEST_CASE ("Effect parser retains typed target dimensions and rejects unknown commands", "[render][fbo]") {
    Project project {};
    const auto parse = [&] (const char* source) {
        return EffectParser::parse (WallpaperEngine::Data::JSON::parseAuthoringJson (source, "effect fixture"),
                                    project);
    };
    const auto effect = parse (R"({"passes":[{"command":"swap","source":"a","target":"b"}],
        "fbos":[{"name":"a","format":"rg88","width":256,"fit":128,"uvs":"repeat",
                  "clear":"-0.25 2.5 0.5 1"}]})");
    REQUIRE (*effect->passes[0]->command == Command_Swap);
    REQUIRE (effect->fbos[0]->width == 256);
    REQUIRE (effect->fbos[0]->fit == 128);
    REQUIRE (effect->fbos[0]->uvs == "repeat");
    REQUIRE (effect->fbos[0]->clear.has_value ());
    REQUIRE (effect->fbos[0]->clear->r == Catch::Approx (-0.25f));
    REQUIRE (effect->fbos[0]->clear->g == Catch::Approx (2.5f));
    REQUIRE_THROWS_AS (parse (R"({"passes":[{"command":"move","source":"a","target":"b"}]})"),
                       std::invalid_argument);
    REQUIRE_THROWS_AS (parse (R"({"passes":[],"fbos":[{"name":"a","width":-2}]})"),
                       std::invalid_argument);
    REQUIRE_THROWS_AS (parse (R"({"passes":[],"fbos":[{"name":"a","fit":1.5}]})"),
                       std::invalid_argument);
    REQUIRE_THROWS_AS (parse (R"({"passes":[],"fbos":[{"name":"a","clear":"0 0 nope 1"}]})"),
                       std::invalid_argument);
    REQUIRE_THROWS_AS (parse (R"({"passes":[],"fbos":[{"name":"a","clear":"1 1e999"}]})"),
                       std::invalid_argument);
    REQUIRE_THROWS_AS (parse (R"({"passes":[],"fbos":[{"name":"a","clear":"1 2 3 4 5"}]})"),
                       std::invalid_argument);
}

TEST_CASE ("Effect main steps follow compose boundaries rather than target or draw counts",
           "[render][fbo][composite]") {
    using WallpaperEngine::Render::Objects::ImageCompositeStepEnd;
    using WallpaperEngine::Render::Objects::imageEffectCompositeStepEnds;
    using WallpaperEngine::Render::Objects::imageEffectMainStepCount;
    Project project {};
    const auto parse = [&] (const char* source) {
        return EffectParser::parse (WallpaperEngine::Data::JSON::parseAuthoringJson (source, "main step fixture"),
                                    project);
    };
    // Native's boolean-only compose field defaults false; strings/numbers
    // do not request a main step. A named output alone never requests one.
    const auto effect = parse (R"({"passes":[
        {"command":"copy","source":"previous","target":"scratch"},
        {"command":"copy","source":"previous","target":"scratch","compose":false},
        {"command":"copy","source":"previous","target":"scratch","compose":1},
        {"command":"copy","source":"previous","target":"scratch","compose":"true"},
        {"command":"swap","source":"scratch","target":"other","compose":true}
    ]})");
    for (size_t index = 0; index < 4; ++index) REQUIRE_FALSE (effect->passes[index]->compose);
    REQUIRE (effect->passes[4]->compose);
    REQUIRE (imageEffectMainStepCount (*effect) == 2);
    // Expanded material draws and zero-draw swaps do not change the count.
    const std::array<ImageCompositeStepEnd, 5> descriptorEnds {{
        {2, 0}, {3, 0}, {5, 0}, {6, 0}, {6, 1},
    }};
    REQUIRE (imageEffectCompositeStepEnds (*effect, descriptorEnds)
             == std::vector<ImageCompositeStepEnd> {{6, 1}, {6, 1}});
    REQUIRE_THROWS_AS (imageEffectCompositeStepEnds (*effect, std::span (descriptorEnds).first (4)),
                       std::invalid_argument);
    const auto empty = parse (R"({"passes":[]})");
    REQUIRE (imageEffectMainStepCount (*empty) == 1);
    REQUIRE (imageEffectCompositeStepEnds (*empty, {}).empty ());

    // Native explicit-ending contract: one effect contributes one step;
    // two separate effects or a compose:true descriptor contribute two.
    const auto sameEffect = parse (R"({"passes":[
        {"source":"previous"}, {"source":"previous","target":"scratch"}
    ]})");
    const std::array<ImageCompositeStepEnd, 2> twoDraws {{{2, 0}, {3, 0}}};
    REQUIRE (imageEffectCompositeStepEnds (*sameEffect, twoDraws)
             == std::vector<ImageCompositeStepEnd> {{3, 0}});
    REQUIRE (imageEffectMainStepCount (*sameEffect) == 1);
    sameEffect->passes[0]->compose = true;
    REQUIRE (imageEffectCompositeStepEnds (*sameEffect, twoDraws)
             == std::vector<ImageCompositeStepEnd> {{2, 0}, {3, 0}});
    REQUIRE (imageEffectMainStepCount (*sameEffect) == 2);
    const auto oneEffect = parse (R"({"passes":[{"target":"scratch"}]})");
    const std::array<ImageCompositeStepEnd, 1> oneDraw {{{3, 0}}};
    REQUIRE (imageEffectCompositeStepEnds (*oneEffect, oneDraw)
             == std::vector<ImageCompositeStepEnd> {{3, 0}});
}

TEST_CASE ("Named effect clear actions use current logical targets and restore framebuffer bindings", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    Project project {};
    const auto effect = EffectParser::parse (WallpaperEngine::Data::JSON::parseAuthoringJson (R"({
        "passes":[],
        "fbos":[
            {"name":"a","format":"r16f","clear":"-0.25"},
            {"name":"b","format":"r16f","clear":"2.5"}
        ],
        "functions":{"reset":{"action":"clear","fbos":["a","b","missing"]},
                     "secondOnly":{"action":"clear","fbos":["b"]},
                     "ignored":{"action":"unknown","fbos":["a"]}}
    })", "effect clear fixture"), project);
    REQUIRE (effect->clearFunctions.at ("reset") == std::vector<std::string> {"a", "b"});
    REQUIRE_FALSE (effect->clearFunctions.contains ("ignored"));

    FBOProvider provider (nullptr);
    const auto a = provider.create (*effect->fbos[0], 0, {2, 2});
    const auto b = provider.create (*effect->fbos[1], 0, {2, 2});
    const auto sentinel = provider.create ("sentinel", TextureFormat_ARGB8888, 0, 1.0f,
                                           {2, 2}, {2, 2});
    clearAndRead (*a, GL_RED, {1, 0, 0, 0});
    clearAndRead (*b, GL_RED, {1, 0, 0, 0});
    REQUIRE (executeEffectClearAction (*effect, provider, "secondOnly"));
    REQUIRE (readRed (*a) == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE (readRed (*b) == Catch::Approx (1.0f).margin (0.01f));
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, sentinel->getFramebuffer ());
    glBindFramebuffer (GL_READ_FRAMEBUFFER, a->getFramebuffer ());
    REQUIRE (executeEffectClearAction (*effect, provider, "reset"));
    GLint draw = 0, read = 0;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read);
    REQUIRE (draw == static_cast<GLint> (sentinel->getFramebuffer ()));
    REQUIRE (read == static_cast<GLint> (a->getFramebuffer ()));
    REQUIRE (readRed (*a) == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE (readRed (*b) == Catch::Approx (2.5f).margin (0.01f));

    provider.swap ("a", "b");
    REQUIRE (executeEffectClearAction (*effect, provider, "reset"));
    REQUIRE (readRed (*a) == Catch::Approx (2.5f).margin (0.01f));
    REQUIRE (readRed (*b) == Catch::Approx (-0.25f).margin (0.01f));
    REQUIRE_FALSE (executeEffectClearAction (*effect, provider, "missing"));
    REQUIRE (glGetError () == GL_NO_ERROR);
}

TEST_CASE ("Logical swap changes lookup identity without moving float target contents", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider parent (nullptr);
    const auto a = parent.create ("a", TextureFormat_R16f, 0, 1.0f, {2, 2}, {2, 2});
    const auto b = parent.create ("b", TextureFormat_R16f, 0, 1.0f, {2, 2}, {2, 2});
    REQUIRE (clearAndRead (*a, GL_RED, {-0.5f, 0, 0, 0})[0] == Catch::Approx (-0.5f));
    REQUIRE (clearAndRead (*b, GL_RED, {2.5f, 0, 0, 0})[0] == Catch::Approx (2.5f));

    FBOProvider child (&parent);
    REQUIRE (child.alias ("alias", "a") == a);
    REQUIRE (child.find ("alias") == a);
    REQUIRE_THROWS_AS (child.alias ("missing_alias", "missing"), std::invalid_argument);
    REQUIRE (child.find ("missing_alias") == nullptr);
    REQUIRE_THROWS_AS (child.swap ("a", "missing"), std::invalid_argument);
    REQUIRE (child.find ("a") == a);

    child.swap ("a", "b");
    REQUIRE (child.find ("a") == b);
    REQUIRE (child.find ("b") == a);
    REQUIRE (parent.find ("a") == a);
    REQUIRE (child.find ("alias") == a);
    REQUIRE (readRed (*child.find ("a")) == Catch::Approx (2.5f));
    REQUIRE (readRed (*child.find ("b")) == Catch::Approx (-0.5f));
    child.swap ("a", "b");
    REQUIRE (child.find ("a") == a);
    REQUIRE (child.find ("b") == b);
}

TEST_CASE ("Scene target unregister checks identity and preserves external aliases", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider scene (nullptr);
    auto first = scene.create ("_rt_imageLayerComposite_42_a", TextureFormat_R16f,
                               0, 1.0f, {2, 2}, {2, 2});
    const std::weak_ptr<CFBO> original = first;
    auto alias = scene.alias ("retainedAlias", "_rt_imageLayerComposite_42_a");
    auto replacement = scene.create ("_rt_imageLayerComposite_42_a", TextureFormat_R16f,
                                     0, 1.0f, {2, 2}, {2, 2});
    REQUIRE_FALSE (scene.eraseIfMappedTo ("_rt_imageLayerComposite_42_a", first));
    REQUIRE (scene.find ("_rt_imageLayerComposite_42_a") == replacement);
    REQUIRE (scene.eraseIfMappedTo ("_rt_imageLayerComposite_42_a", replacement));
    REQUIRE (scene.find ("_rt_imageLayerComposite_42_a") == nullptr);
    REQUIRE (scene.find ("retainedAlias") == original.lock ());
    first.reset ();
    REQUIRE_FALSE (original.expired ());
    REQUIRE (scene.eraseIfMappedTo ("retainedAlias", alias));
    alias.reset ();
    REQUIRE (original.expired ());
    REQUIRE_FALSE (scene.eraseIfMappedTo ("retainedAlias", replacement));
}

TEST_CASE ("Repeated image composite mappings release after logical swaps", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider scene (nullptr);
    for (int generation = 0; generation < 16; ++generation) {
        const std::string firstName = "_rt_imageLayerComposite_42_a";
        const std::string secondName = "_rt_imageLayerComposite_42_b";
        auto first = scene.create (firstName, TextureFormat_R16f, 0, 1.0f, {2, 2}, {2, 2});
        auto second = scene.create (secondName, TextureFormat_R16f, 0, 1.0f, {2, 2}, {2, 2});
        const std::weak_ptr<CFBO> firstWeak = first;
        const std::weak_ptr<CFBO> secondWeak = second;
        scene.swap (firstName, secondName);
        for (const auto& name : {firstName, secondName}) {
            if (!scene.eraseIfMappedTo (name, first))
                REQUIRE (scene.eraseIfMappedTo (name, second));
        }
        REQUIRE (scene.find (firstName) == nullptr);
        REQUIRE (scene.find (secondName) == nullptr);
        first.reset ();
        second.reset ();
        REQUIRE (firstWeak.expired ());
        REQUIRE (secondWeak.expired ());
    }
}

TEST_CASE ("Unique effect targets share the scene cache and retain identity on resize", "[render][fbo]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    FBOProvider scene (nullptr);
    std::weak_ptr<CFBO> released;
    {
        FBOProvider firstEffect (&scene);
        FBOProvider secondEffect (&scene);
        FBOProvider nonuniqueEffect (&scene);
        const FBO first {.name = "shared", .format = "r16f", .scale = 1.0f,
                         .unique = true, .clear = glm::vec4 (-0.25f, 0, 0, 1)};
        const FBO second {.name = "shared", .format = "r16f", .scale = 1.0f,
                          .unique = true, .clear = glm::vec4 (2.5f, 0, 0, 1)};
        const auto samplerFlags = TextureFlags_ClampUVs | TextureFlags_NoInterpolation;
        auto a = firstEffect.create (first, samplerFlags, {4, 3});
        released = a;
        REQUIRE (readRed (*a) == Catch::Approx (-0.25f).margin (0.01f));
        const auto originalTexture = a->getTextureID (0);
        auto b = secondEffect.create (second, samplerFlags, {8, 5});
        REQUIRE (b == a);
        // The first acquisition of this descriptor reuses existing storage,
        // yet its own authored clear runs even on a cache hit.
        REQUIRE (b->getTextureWidth (0) == 4);
        REQUIRE (readRed (*a) == Catch::Approx (2.5f).margin (0.01f));
        const FBO third {.name = "shared", .format = "r16f", .scale = 1.0f,
                         .unique = true, .clear = glm::vec4 (1.25f, 0, 0, 1)};
        REQUIRE (secondEffect.create (third, samplerFlags, {9, 6}) == a);
        REQUIRE (a->getTextureWidth (0) == 4);
        REQUIRE (readRed (*a) == Catch::Approx (1.25f).margin (0.01f));
        auto unrelated = nonuniqueEffect.create (
            FBO {.name = "shared", .format = "r16f", .scale = 1.0f, .unique = false}, 0, {3, 2});
        REQUIRE (unrelated != a);

        glEnable (GL_INVALID_ENUM);
        REQUIRE (secondEffect.create (second, samplerFlags, {8, 5}) == a);
        REQUIRE (glGetError () == GL_INVALID_ENUM);
        REQUIRE (a->getTextureWidth (0) == 8);
        REQUIRE (a->getTextureHeight (0) == 5);
        REQUIRE (a->getTextureID (0) != originalTexture);
        glBindTexture (GL_TEXTURE_2D, a->getTextureID (0));
        GLint wrapAfterResize = 0;
        GLint filterAfterResize = 0;
        glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapAfterResize);
        glGetTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filterAfterResize);
        REQUIRE (wrapAfterResize == GL_CLAMP_TO_EDGE);
        REQUIRE (filterAfterResize == GL_NEAREST);
        REQUIRE (a->getFrames ().front ()->width1 == 8);
        REQUIRE (framebufferComplete (*a));
        REQUIRE (readRed (*a, 7, 4) == Catch::Approx (2.5f).margin (0.01f));
        const auto otherMapping = secondEffect.create ("other-mapping", TextureFormat_R16f, 0, 1.0f,
                                                        {8, 5}, {8, 5});
        secondEffect.swap ("shared", "other-mapping");
        REQUIRE (secondEffect.create (second, samplerFlags, {10, 7}) == a);
        REQUIRE (secondEffect.find ("shared") == otherMapping);
        REQUIRE (secondEffect.find ("other-mapping") == a);
        REQUIRE (a->getTextureWidth (0) == 10);
        REQUIRE (a->getTextureHeight (0) == 7);
        REQUIRE (readRed (*secondEffect.find ("other-mapping"), 9, 6)
                 == Catch::Approx (2.5f).margin (0.01f));
        secondEffect.swap ("shared", "other-mapping");
        REQUIRE_THROWS_AS (a->resize (8, 5, 0, 5), std::invalid_argument);
        REQUIRE (a->getTextureWidth (0) == 10);
        REQUIRE (firstEffect.find ("shared") == a);
        REQUIRE (secondEffect.find ("shared") == a);
    }
    REQUIRE (released.expired ());
    REQUIRE (glGetError () == GL_NO_ERROR);
}
