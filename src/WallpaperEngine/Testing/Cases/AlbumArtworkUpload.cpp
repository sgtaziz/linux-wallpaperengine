#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Media/MediaArtwork.h"
#include "WallpaperEngine/Render/AlbumTexture.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <array>

using namespace WallpaperEngine::Render;

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
        EGLint count = 0;
        if (!eglChooseConfig (display, configAttributes, &config, 1, &count) || count != 1) return;
        const EGLint surfaceAttributes[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
        surface = eglCreatePbufferSurface (display, config, surfaceAttributes);
        const EGLint contextAttributes[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE};
        context = eglCreateContext (display, config, EGL_NO_CONTEXT, contextAttributes);
        if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
            !eglMakeCurrent (display, surface, surface, context)) return;
        glewExperimental = GL_TRUE;
        (void) glewInit ();
        while (glGetError () != GL_NO_ERROR) { }
        ready = glTexImage2D != nullptr && glBindBuffer != nullptr;
    }
    ~SurfacelessGL () {
        if (display == EGL_NO_DISPLAY) return;
        eglMakeCurrent (display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext (display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface (display, surface);
        eglTerminate (display);
    }
};

std::array<unsigned char, 4> firstPixel (GLuint texture) {
    glBindTexture (GL_TEXTURE_2D, texture);
    std::array<unsigned char, 4> pixel {};
    glGetTexImage (GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data ());
    return pixel;
}
}

TEST_CASE ("Album artwork upload replaces and clears pixels under ambient PBO state",
           "[render][media-artwork]") {
    SurfacelessGL gl;
    REQUIRE (gl.ready);
    GLuint textures[3] {};
    glGenTextures (3, textures);
    GLuint unpackBuffer = 0;
    glGenBuffers (1, &unpackBuffer);
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData (GL_PIXEL_UNPACK_BUFFER, 1024, nullptr, GL_STATIC_DRAW);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 9);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 2);
    glBindTexture (GL_TEXTURE_2D, textures[2]);
    WallpaperEngine::Media::MediaArtwork cyan {1, 1, {20, 220, 240, 255}};
    WallpaperEngine::Media::MediaArtwork orange {1, 1, {240, 50, 20, 255}};
    uploadAlbumArtworkTexture (textures[0], &cyan);
    uploadAlbumArtworkTexture (textures[1], &cyan);
    GLint state = 0;
    glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &state);
    REQUIRE (state == static_cast<GLint> (unpackBuffer));
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &state); REQUIRE (state == 8);
    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &state); REQUIRE (state == 9);
    glGetIntegerv (GL_UNPACK_SKIP_ROWS, &state); REQUIRE (state == 1);
    glGetIntegerv (GL_UNPACK_SKIP_PIXELS, &state); REQUIRE (state == 2);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &state);
    REQUIRE (state == static_cast<GLint> (textures[2]));
    REQUIRE (firstPixel (textures[0]) == std::array<unsigned char, 4> {20,220,240,255});
    uploadAlbumArtworkTexture (textures[0], &orange);
    REQUIRE (firstPixel (textures[0]) == std::array<unsigned char, 4> {240,50,20,255});
    REQUIRE (firstPixel (textures[1]) == std::array<unsigned char, 4> {20,220,240,255});
    uploadAlbumArtworkTexture (textures[0], nullptr);
    REQUIRE (firstPixel (textures[0]) == std::array<unsigned char, 4> {0,0,0,0});
    REQUIRE (firstPixel (textures[1]) == std::array<unsigned char, 4> {20,220,240,255});
    REQUIRE (glGetError () == GL_NO_ERROR);
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
    glDeleteBuffers (1, &unpackBuffer);
    glDeleteTextures (3, textures);
}
