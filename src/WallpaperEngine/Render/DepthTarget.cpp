#include "DepthTarget.h"

#include <stdexcept>

using namespace WallpaperEngine::Render;

DepthTarget::DepthTarget (uint32_t width, uint32_t height) {
    glGenTextures (1, &m_texture);
    glGenFramebuffers (1, &m_framebuffer);
    try { resize (width, height); }
    catch (...) {
        glDeleteTextures (1, &m_texture);
        glDeleteFramebuffers (1, &m_framebuffer);
        throw;
    }
}

DepthTarget::~DepthTarget () {
    glDeleteTextures (1, &m_texture);
    glDeleteFramebuffers (1, &m_framebuffer);
}

void DepthTarget::resize (uint32_t width, uint32_t height) {
    if (!width || !height) throw std::invalid_argument ("Shadow atlas dimensions must be positive");
    if (getRealWidth () == width && getRealHeight () == height) return;
    GLint texture = 0, draw = 0, read = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read);
    glBindTexture (GL_TEXTURE_2D, m_texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                  GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);
    const GLfloat border[4] {};
    glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glBindFramebuffer (GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_texture, 0);
    glDrawBuffer (GL_NONE);
    glReadBuffer (GL_NONE);
    const auto status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    glBindTexture (GL_TEXTURE_2D, GLuint (texture));
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, GLuint (draw));
    glBindFramebuffer (GL_READ_FRAMEBUFFER, GLuint (read));
    if (status != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error ("Incomplete D32 shadow atlas");
    m_resolution = {width, height, width, height};
    auto frame = std::make_shared<Frame> ();
    frame->width1 = frame->width2 = width;
    frame->height1 = frame->height2 = height;
    m_frames = {std::move (frame)};
    clear ();
}

void DepthTarget::clear () const {
    GLint draw = 0;
    GLboolean depthMask = GL_FALSE;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetBooleanv (GL_DEPTH_WRITEMASK, &depthMask);
    const bool scissor = glIsEnabled (GL_SCISSOR_TEST);
    glDisable (GL_SCISSOR_TEST);
    glDepthMask (GL_TRUE);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_framebuffer);
    const GLfloat zero = 0;
    glClearBufferfv (GL_DEPTH, 0, &zero);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, GLuint (draw));
    glDepthMask (depthMask);
    if (scissor) glEnable (GL_SCISSOR_TEST);
}
