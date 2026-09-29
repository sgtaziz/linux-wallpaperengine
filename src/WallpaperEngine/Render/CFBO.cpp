#include "CFBO.h"
#include "WallpaperEngine/Logging/Log.h"

#include <stdexcept>
#include <limits>
#include <cmath>

using namespace WallpaperEngine::Render;

namespace {
struct GLTextureFormat {
    GLint internal;
    GLenum channels;
    GLenum component;
};

GLTextureFormat glTextureFormat (TextureFormat format) {
    switch (format) {
        case TextureFormat_ARGB8888: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case TextureFormat_RGB888: return {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE};
        case TextureFormat_R8: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case TextureFormat_RG88: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
        case TextureFormat_R16f: return {GL_R16F, GL_RED, GL_FLOAT};
        case TextureFormat_RG1616f: return {GL_RG16F, GL_RG, GL_FLOAT};
        case TextureFormat_RGBA16161616f: return {GL_RGBA16F, GL_RGBA, GL_FLOAT};
        case TextureFormat_RGB161616f: return {GL_RGB16F, GL_RGB, GL_FLOAT};
        default: throw std::invalid_argument ("Unsupported render-target texture format");
    }
}
}

CFBO::CFBO (
    std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
    uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight, std::optional<glm::vec4> clearColor
) : m_scale (scale), m_name (std::move (name)), m_format (format), m_flags (flags) {
    const auto storage = glTextureFormat (format);
    // create an empty texture that'll be free'd so the FBO is transparent
    constexpr GLenum drawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
    // create the main framebuffer
    glGenFramebuffers (1, &this->m_framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    // create the main texture
    glGenTextures (1, &this->m_texture);
    // bind the new texture to set settings on it
    glBindTexture (GL_TEXTURE_2D, this->m_texture);
    // give OpenGL an empty image
    glTexImage2D (GL_TEXTURE_2D, 0, storage.internal, textureWidth, textureHeight, 0,
                  storage.channels, storage.component, nullptr);
    // label stuff for debugging
#if !NDEBUG
    glObjectLabel (GL_TEXTURE, this->m_texture, -1, this->m_name.c_str ());
#endif /* DEBUG */
    // set filtering parameters, otherwise the texture is not rendered
    if (flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (flags & TextureFlags_ClampUVsBorder) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, m_maxAnisotropy);

    // set the texture as the colour attachmend #0
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->m_texture, 0);
    // finally set the list of draw buffers
    glDrawBuffers (1, drawBuffers);

    // ensure first framebuffer is okay
    const GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        // A throwing constructor has no destructor; release the partially
        // allocated handles before reporting the failed attachment contract.
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        glBindTexture (GL_TEXTURE_2D, 0);
        glDeleteTextures (1, &m_texture);
        glDeleteFramebuffers (1, &m_framebuffer);
        m_texture = GL_NONE;
        m_framebuffer = GL_NONE;
	sLog.exception ("Framebuffer '", m_name, "' is incomplete (status ", status, ")");
    }

    // Clear only on allocation. Authored effect targets can choose an RGBA
    // initialization value; other layer targets start transparent. Rebinding
    // an existing target must preserve the values written by prior passes.
    this->clear (clearColor.value_or (glm::vec4 (0.0f)));

    this->m_resolution = { textureWidth, textureHeight, realWidth, realHeight };

    // create the textureframe entries
    const auto frame = std::make_shared<Frame> ();

    frame->frameNumber = 0;
    frame->frametime = 0;
    frame->height1 = textureHeight;
    frame->height2 = realHeight;
    frame->width1 = textureWidth;
    frame->width2 = realWidth;
    frame->x = 0;
    frame->y = 0;

    this->m_frames.push_back (frame);
}

CFBO::~CFBO () {
    // free opengl texture and framebuffer
    if (m_depthbuffer != GL_NONE) glDeleteRenderbuffers (1, &m_depthbuffer);
    glDeleteTextures (1, &this->m_texture);
    glDeleteFramebuffers (1, &this->m_framebuffer);
}

void CFBO::setMaxAnisotropy (float value) {
    if (value < 1.0f || !std::isfinite (value))
        throw std::invalid_argument ("Invalid framebuffer anisotropy");
    m_maxAnisotropy = value;
    GLint previousTexture = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &previousTexture);
    glBindTexture (GL_TEXTURE_2D, m_texture);
    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, value);
    glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (previousTexture));
}

const std::string& CFBO::getName () const { return this->m_name; }

const float& CFBO::getScale () const { return this->m_scale; }

TextureFormat CFBO::getFormat () const { return this->m_format; }

uint32_t CFBO::getFlags () const { return this->m_flags; }

GLuint CFBO::getFramebuffer () const { return this->m_framebuffer; }

GLuint CFBO::getDepthbuffer () const { return this->m_depthbuffer; }

void CFBO::attachDepth16 () {
    if (m_depthbuffer != GL_NONE) return;
    GLint previousDraw = 0;
    GLint previousRead = 0;
    GLint previousRenderbuffer = 0;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv (GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    GLuint depth = GL_NONE;
    glGenRenderbuffers (1, &depth);
    glBindRenderbuffer (GL_RENDERBUFFER, depth);
    glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                           static_cast<GLsizei> (getTextureWidth (0)),
                           static_cast<GLsizei> (getTextureHeight (0)));
    glBindFramebuffer (GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLenum status = glCheckFramebufferStatus (GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, GL_NONE);
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (previousDraw));
    glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (previousRead));
    glBindRenderbuffer (GL_RENDERBUFFER, static_cast<GLuint> (previousRenderbuffer));
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteRenderbuffers (1, &depth);
        sLog.exception ("D16 scene framebuffer '", m_name, "' is incomplete (status ", status, ")");
    }
    m_depthbuffer = depth;
}

void CFBO::clear (const glm::vec4& color) const {
    glBindFramebuffer (GL_FRAMEBUFFER, this->m_framebuffer);
    const GLboolean scissorEnabled = glIsEnabled (GL_SCISSOR_TEST);
    GLboolean writeMask[4] = {};
    glGetBooleanv (GL_COLOR_WRITEMASK, writeMask);
    glDisable (GL_SCISSOR_TEST);
    glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    const GLfloat channels[4] = {color.r, color.g, color.b, color.a};
    glClearBufferfv (GL_COLOR, 0, channels);
    glColorMask (writeMask[0], writeMask[1], writeMask[2], writeMask[3]);
    if (scissorEnabled) glEnable (GL_SCISSOR_TEST);
}

void CFBO::resize (uint32_t realWidth, uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight) {
    if (!realWidth || !realHeight || !textureWidth || !textureHeight
        || textureWidth > static_cast<uint32_t> (std::numeric_limits<GLsizei>::max ())
        || textureHeight > static_cast<uint32_t> (std::numeric_limits<GLsizei>::max ()))
        throw std::invalid_argument ("Invalid framebuffer resize dimensions");
    GLint hardwareLimit = 0;
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &hardwareLimit);
    if (hardwareLimit > 0 && (textureWidth > static_cast<uint32_t> (hardwareLimit)
                              || textureHeight > static_cast<uint32_t> (hardwareLimit)))
        throw std::invalid_argument ("Framebuffer resize exceeds hardware texture limit");
    if (getTextureWidth (0) == textureWidth && getTextureHeight (0) == textureHeight
        && getRealWidth () == realWidth && getRealHeight () == realHeight) return;

    GLint previousTexture = 0;
    GLint previousDrawFramebuffer = 0;
    GLint previousReadFramebuffer = 0;
    GLint previousRenderbuffer = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &previousTexture);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
    glGetIntegerv (GL_RENDERBUFFER_BINDING, &previousRenderbuffer);

    const auto storage = glTextureFormat (m_format);
    GLuint replacement = GL_NONE;
    glGenTextures (1, &replacement);
    glBindTexture (GL_TEXTURE_2D, replacement);
    glTexImage2D (GL_TEXTURE_2D, 0, storage.internal, textureWidth, textureHeight, 0,
                  storage.channels, storage.component, nullptr);
    GLint allocatedWidth = 0;
    GLint allocatedHeight = 0;
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &allocatedWidth);
    glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &allocatedHeight);
    if (m_flags & TextureFlags_ClampUVs) {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (m_flags & TextureFlags_ClampUVsBorder) {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    } else {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    const auto filter = m_flags & TextureFlags_NoInterpolation ? GL_NEAREST : GL_LINEAR;
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, m_maxAnisotropy);
    GLuint replacementDepth = GL_NONE;
    if (m_depthbuffer != GL_NONE) {
        glGenRenderbuffers (1, &replacementDepth);
        glBindRenderbuffer (GL_RENDERBUFFER, replacementDepth);
        glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                               static_cast<GLsizei> (textureWidth),
                               static_cast<GLsizei> (textureHeight));
    }
    glBindFramebuffer (GL_FRAMEBUFFER, m_framebuffer);
    if (allocatedWidth == static_cast<GLint> (textureWidth)
        && allocatedHeight == static_cast<GLint> (textureHeight)) {
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, replacement, 0);
        if (replacementDepth != GL_NONE)
            glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_RENDERBUFFER, replacementDepth);
    }
    const auto status = allocatedWidth == static_cast<GLint> (textureWidth)
                         && allocatedHeight == static_cast<GLint> (textureHeight)
        ? glCheckFramebufferStatus (GL_FRAMEBUFFER) : GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
        if (replacementDepth != GL_NONE)
            glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_RENDERBUFFER, m_depthbuffer);
    }
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    glBindFramebuffer (GL_READ_FRAMEBUFFER, previousReadFramebuffer);
    glBindTexture (GL_TEXTURE_2D, status == GL_FRAMEBUFFER_COMPLETE && previousTexture == static_cast<GLint> (m_texture)
                                     ? replacement : static_cast<GLuint> (previousTexture));
    glBindRenderbuffer (GL_RENDERBUFFER,
                        status == GL_FRAMEBUFFER_COMPLETE && replacementDepth != GL_NONE
                            && previousRenderbuffer == static_cast<GLint> (m_depthbuffer)
                            ? replacementDepth : static_cast<GLuint> (previousRenderbuffer));
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteTextures (1, &replacement);
        if (replacementDepth != GL_NONE) glDeleteRenderbuffers (1, &replacementDepth);
        sLog.exception ("Resized framebuffer '", m_name, "' is incomplete (status ", status, ")");
    }

    const auto previousTargetTexture = m_texture;
    m_texture = replacement;
    glDeleteTextures (1, &previousTargetTexture);
    if (replacementDepth != GL_NONE) {
        const auto previousDepth = m_depthbuffer;
        m_depthbuffer = replacementDepth;
        glDeleteRenderbuffers (1, &previousDepth);
    }

    m_resolution = {textureWidth, textureHeight, realWidth, realHeight};
    auto& frame = *m_frames.front ();
    frame.width1 = textureWidth;
    frame.width2 = realWidth;
    frame.height1 = textureHeight;
    frame.height2 = realHeight;
}

GLuint CFBO::getTextureID (uint32_t imageIndex) const { return this->m_texture; }

uint32_t CFBO::getTextureWidth (uint32_t imageIndex) const { return this->m_resolution.x; }

uint32_t CFBO::getTextureHeight (uint32_t imageIndex) const { return this->m_resolution.y; }

uint32_t CFBO::getRealWidth () const { return this->m_resolution.z; }

uint32_t CFBO::getRealHeight () const { return this->m_resolution.w; }

const std::vector<FrameSharedPtr>& CFBO::getFrames () const { return this->m_frames; }

const glm::vec4* CFBO::getResolution () const { return &this->m_resolution; }

bool CFBO::isAnimated () const { return false; }

uint32_t CFBO::getSpritesheetCols () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetRows () const {
    return 0; // FBOs don't have spritesheets
}

uint32_t CFBO::getSpritesheetFrames () const {
    return 0; // FBOs don't have spritesheets
}

float CFBO::getSpritesheetDuration () const {
    return 0.0f; // FBOs don't have spritesheets
}

void CFBO::incrementUsageCount () const { }
void CFBO::decrementUsageCount () const { }
void CFBO::update () const { }
// FBOs are always ready
bool CFBO::isReady () const { return true; }
