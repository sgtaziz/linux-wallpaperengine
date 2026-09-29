#include "AlbumTexture.h"

#include "RenderContext.h"
#include "WallpaperEngine/Media/MediaArtwork.h"
#include "WallpaperEngine/Media/MediaSource.h"

using namespace WallpaperEngine::Render;

namespace {
class TightPixelTransfer {
public:
    TightPixelTransfer () {
        glGetIntegerv (GL_TEXTURE_BINDING_2D, &m_texture);
        glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &m_unpackBuffer);
        glGetIntegerv (GL_UNPACK_ALIGNMENT, &m_unpackAlignment);
        glGetIntegerv (GL_UNPACK_ROW_LENGTH, &m_unpackRowLength);
        glGetIntegerv (GL_UNPACK_SKIP_ROWS, &m_unpackSkipRows);
        glGetIntegerv (GL_UNPACK_SKIP_PIXELS, &m_unpackSkipPixels);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
    }

    ~TightPixelTransfer () {
        glPixelStorei (GL_UNPACK_ALIGNMENT, m_unpackAlignment);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, m_unpackRowLength);
        glPixelStorei (GL_UNPACK_SKIP_ROWS, m_unpackSkipRows);
        glPixelStorei (GL_UNPACK_SKIP_PIXELS, m_unpackSkipPixels);
        glBindBuffer (GL_PIXEL_UNPACK_BUFFER, m_unpackBuffer);
        glBindTexture (GL_TEXTURE_2D, m_texture);
    }

private:
    GLint m_texture = 0;
    GLint m_unpackBuffer = 0, m_unpackAlignment = 4, m_unpackRowLength = 0;
    GLint m_unpackSkipRows = 0, m_unpackSkipPixels = 0;
};

}

void WallpaperEngine::Render::uploadAlbumArtworkTexture (
    GLuint texture, const WallpaperEngine::Media::MediaArtwork* artwork
) {
    TightPixelTransfer transfer;
    glBindTexture (GL_TEXTURE_2D, texture);
    if (!artwork) {
        constexpr std::uint32_t transparent = 0;
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA,
                      GL_UNSIGNED_BYTE, &transparent);
        return;
    }
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, artwork->width, artwork->height,
                  0, GL_RGBA, GL_UNSIGNED_BYTE, artwork->rgba.data ());
}

AlbumTexture::AlbumTexture (RenderContext& context) : Helpers::ContextAware (context) {
    // setup a basic texture with clamping and no mipmaps
    this->m_resolution = glm::vec4 (1.0f, 1.0f, 1.0f, 1.0f);

    glGenTextures (1, &this->m_textureID);
    glBindTexture (GL_TEXTURE_2D, this->m_textureID);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

AlbumTexture::~AlbumTexture () { glDeleteTextures (1, &this->m_textureID); }

GLuint AlbumTexture::getTextureID (uint32_t imageIndex) const { return this->m_textureID; }
uint32_t AlbumTexture::getTextureWidth (uint32_t imageIndex) const { return this->m_width; }
uint32_t AlbumTexture::getTextureHeight (uint32_t imageIndex) const { return this->m_height; }
uint32_t AlbumTexture::getRealWidth () const { return this->m_width; }
uint32_t AlbumTexture::getRealHeight () const { return this->m_height; }
TextureFormat AlbumTexture::getFormat () const { return TextureFormat_ARGB8888; }
uint32_t AlbumTexture::getFlags () const { return TextureFlags_NoFlags; }
const std::vector<FrameSharedPtr>& AlbumTexture::getFrames () const { return this->m_frames; }
const glm::vec4* AlbumTexture::getResolution () const { return &this->m_resolution; }
bool AlbumTexture::isAnimated () const { return false; }
uint32_t AlbumTexture::getSpritesheetCols () const { return 1; }
uint32_t AlbumTexture::getSpritesheetRows () const { return 1; }
uint32_t AlbumTexture::getSpritesheetFrames () const { return 1; }
float AlbumTexture::getSpritesheetDuration () const { return 0.0f; }

void AlbumTexture::incrementUsageCount () const { }
void AlbumTexture::decrementUsageCount () const { }
void AlbumTexture::update () const { }

void AlbumTexture::copyContents (const AlbumTexture& other) const noexcept {
    // The old cover is already decoded in memory. Preserve that immutable
    // revision without a blocking GPU readback from the render callback.
    m_artwork = other.m_artwork;
    m_width = other.m_width;
    m_height = other.m_height;
    m_resolution = other.m_resolution;
    uploadAlbumArtworkTexture (m_textureID, m_artwork.get ());
}

void AlbumTexture::load () const {
    this->m_width = 0;
    this->m_height = 0;
    this->m_resolution = glm::vec4 (1.0f);
    m_artwork = getContext ().getMediaSource ().getMediaInfo ().artwork;
    if (!m_artwork) { uploadAlbumArtworkTexture (m_textureID, nullptr); return; }
    this->m_width = m_artwork->width;
    this->m_height = m_artwork->height;
    this->m_resolution = glm::vec4 (m_width, m_height, m_width, m_height);
    uploadAlbumArtworkTexture (m_textureID, m_artwork.get ());
}

bool AlbumTexture::isReady () const {
    // these are only ready to be rendered if their content's are present
    return this->m_width > 0 && this->m_height > 0;
}
