#include "CTexture.h"
#include "WallpaperEngine/Logging/Log.h"

#include <lz4.h>
#include <limits>

#define STB_IMAGE_IMPLEMENTATION
#include "RenderContext.h"

#include <stb_image.h>

using namespace WallpaperEngine::Render;

CTexture::CTexture (RenderContext& context, TextureUniquePtr header, std::string assetName) :
    Helpers::ContextAware (context), m_header (std::move (header)), m_assetName (std::move (assetName)) {
    // ensure the header is parsed
    this->setupResolution ();
    const GLint internalFormat = storageFormat (*this->m_header);

    // videos are a bit special, they only have one framebuffer, one mipmap
    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	if (this->m_header->images.empty () || this->m_header->images.begin ()->second.empty ()) {
	    sLog.exception ("Cannot load video texture, no mipmaps found");
	}

	// generate the texture and set it up to be used by the player
	auto allocatedID = std::make_unique<GLuint[]> (1);
	this->m_textureID = allocatedID.get ();
	glGenTextures (1, this->m_textureID);
	try {
	    this->setupOpenGLParameters (0);
	    const auto mipmap = *this->m_header->images.begin ()->second.begin ();
	    this->m_player = std::make_unique<GLPlayer> (
		this->getContext (), this->m_textureID[0],
		std::make_unique<MemoryStreamProtocol> (mipmap->uncompressedData.get (), mipmap->uncompressedSize),
		this->m_header->textureWidth, this->m_header->textureHeight
	    );
	    this->m_player->setMuted ();
	    this->m_player->setVolume (0.0f);
	    this->m_player->setUntimed ();
	} catch (...) {
	    this->m_player.reset ();
	    glDeleteTextures (1, this->m_textureID);
	    this->m_textureID = nullptr;
	    throw;
	}
	this->m_textureID = allocatedID.release ();
	// texture is ready, nothing else to do
	return;
    }

    // allocate texture ids list
    auto allocatedIDs = std::make_unique<GLuint[]> (this->m_header->imageCount);
    this->m_textureID = allocatedIDs.get ();
    // ask opengl for the correct amount of textures and framebuffers
    glGenTextures (this->m_header->imageCount, this->m_textureID);

    try {
	for (const auto& [index, mipmaps] : this->m_header->images) {
	    if (index >= this->m_header->imageCount)
		sLog.exception ("Cannot load texture ", m_assetName, ": image index ", index,
		                " exceeds image count ", this->m_header->imageCount);
	    this->setupOpenGLParameters (index);
	    GLint level = 0;
	    for (const auto& mipmap : mipmaps)
		uploadLevel (*this->m_header, *mipmap, internalFormat, this->m_textureID[index], level++, m_assetName);
	}
    } catch (...) {
	glDeleteTextures (this->m_header->imageCount, this->m_textureID);
	this->m_textureID = nullptr;
	throw;
    }
    this->m_textureID = allocatedIDs.release ();
}

void CTexture::uploadLevel (const Texture& header, const Mipmap& mipmap, GLint internalFormat,
                            GLuint textureID, GLint level, const std::string& assetName) {
    if (mipmap.width == 0 || mipmap.height == 0 ||
        mipmap.width > std::numeric_limits<GLsizei>::max () ||
        mipmap.height > std::numeric_limits<GLsizei>::max ())
        sLog.exception ("Cannot load texture ", assetName, ": invalid mip dimensions");

    if (header.freeImageFormat == FIF_UNKNOWN) {
        unsigned int bytesPerPixel = 0;
        unsigned int bytesPerBlock = 0;
        switch (header.format) {
            case TextureFormat_R8: bytesPerPixel = 1; break;
            case TextureFormat_RG88: bytesPerPixel = 2; break;
            case TextureFormat_RGB888: bytesPerPixel = 3; break;
            case TextureFormat_ARGB8888: bytesPerPixel = 4; break;
            case TextureFormat_R16f: bytesPerPixel = 2; break;
            case TextureFormat_RG1616f: bytesPerPixel = 4; break;
            case TextureFormat_RGB161616f: bytesPerPixel = 6; break;
            case TextureFormat_RGBA16161616f: bytesPerPixel = 8; break;
            case TextureFormat_DXT1: bytesPerBlock = 8; break;
            case TextureFormat_DXT3:
            case TextureFormat_DXT5: bytesPerBlock = 16; break;
            case TextureFormat_BC7: bytesPerBlock = 16; break;
            case TextureFormat_RGBa1010102: bytesPerPixel = 4; break;
            default: break;
        }
        if (bytesPerPixel != 0) {
            const uint64_t rowBytes = uint64_t (mipmap.width) * bytesPerPixel;
            if (rowBytes > std::numeric_limits<uint64_t>::max () / mipmap.height ||
                mipmap.uncompressedSize < 0 ||
                uint64_t (mipmap.uncompressedSize) < rowBytes * mipmap.height ||
                mipmap.uncompressedData == nullptr)
                sLog.exception ("Cannot load texture ", assetName, ": truncated mipmap data");
        } else if (bytesPerBlock != 0) {
            const uint64_t blocksWide = (uint64_t (mipmap.width) + 3) / 4;
            const uint64_t blocksHigh = (uint64_t (mipmap.height) + 3) / 4;
            const uint64_t required = blocksWide * blocksHigh * bytesPerBlock;
            if (required > std::numeric_limits<GLsizei>::max () ||
                mipmap.uncompressedSize < 0 || uint64_t (mipmap.uncompressedSize) != required ||
                mipmap.uncompressedData == nullptr)
                sLog.exception ("Cannot load texture ", assetName, ": invalid compressed block length");
        }
    }
    glBindTexture (GL_TEXTURE_2D, textureID);
    int width = mipmap.width;
    int height = mipmap.height;
    GLenum sourceFormat = GL_RGBA;
    GLenum sourceType = GL_UNSIGNED_BYTE;
    const void* data = mipmap.uncompressedData.get ();
    std::unique_ptr<stbi_uc, decltype (&stbi_image_free)> decoded (nullptr, &stbi_image_free);

    if (header.freeImageFormat != FIF_UNKNOWN) {
	int fileChannels = 0;
	decoded.reset (stbi_load_from_memory (
	    reinterpret_cast<const stbi_uc*> (mipmap.uncompressedData.get ()), mipmap.uncompressedSize,
	    &width, &height, &fileChannels, 4));
	if (!decoded)
	    sLog.exception ("Cannot decode texture ", assetName, ": ", stbi_failure_reason ());
	data = decoded.get ();
    } else {
	switch (header.format) {
	    case TextureFormat_R8:
	    case TextureFormat_R16f:
		sourceFormat = GL_RED;
		break;
	    case TextureFormat_RG88:
	    case TextureFormat_RG1616f:
		sourceFormat = GL_RG;
		break;
	    case TextureFormat_RGB888:
	    case TextureFormat_RGB161616f:
		sourceFormat = GL_RGB;
		break;
	    default:
		break;
	}
	if (header.format == TextureFormat_RGBa1010102)
	    sourceType = GL_UNSIGNED_INT_2_10_10_10_REV;
	if (header.format == TextureFormat_R16f || header.format == TextureFormat_RG1616f ||
	    header.format == TextureFormat_RGB161616f || header.format == TextureFormat_RGBA16161616f)
	    sourceType = GL_HALF_FLOAT;
    }

    switch (internalFormat) {
	case GL_RGBA8:
	case GL_RGB8:
	case GL_RG8:
	case GL_R8:
	case GL_R16F:
	case GL_RG16F:
	case GL_RGB16F:
	case GL_RGBA16F:
	case GL_RGB10_A2: {
	    GLint priorAlignment = 4;
	    GLint priorRowLength = 0;
	    glGetIntegerv (GL_UNPACK_ALIGNMENT, &priorAlignment);
	    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &priorRowLength);
	    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	    glTexImage2D (GL_TEXTURE_2D, level, internalFormat, width, height, 0,
	                  sourceFormat, sourceType, data);
	    glPixelStorei (GL_UNPACK_ROW_LENGTH, priorRowLength);
	    glPixelStorei (GL_UNPACK_ALIGNMENT, priorAlignment);
	    break;
	}
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
	case GL_COMPRESSED_RGBA_BPTC_UNORM:
	    if (internalFormat == GL_COMPRESSED_RGBA_BPTC_UNORM &&
	        !GLEW_VERSION_4_2 && !GLEW_ARB_texture_compression_bptc)
	        sLog.exception ("Cannot load texture ", assetName, ": BC7 compression is unavailable");
	    glCompressedTexImage2D (GL_TEXTURE_2D, level, internalFormat, width, height, 0,
	                            mipmap.uncompressedSize, data);
	    break;
	default:
	    sLog.exception ("Cannot load texture ", assetName, ", unknown format ", header.format);
    }
}

CTexture::~CTexture () {
    // first release the player to prevent using null references
    this->m_player.reset ();

    if (this->m_header->isVideoMp4 || this->m_header->flags & TextureFlags_Video) {
	glDeleteTextures (1, this->m_textureID);
    } else {
	glDeleteTextures (this->m_header->imageCount, this->m_textureID);
    }

    delete[] this->m_textureID;
}

void CTexture::setupResolution () {
    if (this->isAnimated ()) {
	this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->gifWidth,
			       this->m_header->gifHeight };
    } else {
	if (this->m_header->freeImageFormat != FIF_UNKNOWN) {
	    // wpengine-texture format always has one mipmap
	    // get first image size
	    const auto element = this->m_header->images.find (0)->second.begin ();

	    // set the texture resolution
	    this->m_resolution
		= { (*element)->width, (*element)->height, this->m_header->width, this->m_header->height };
	} else {
	    // set the texture resolution
	    this->m_resolution = { this->m_header->textureWidth, this->m_header->textureHeight, this->m_header->width,
				   this->m_header->height };
	}
    }
}

GLint CTexture::storageFormat (const Texture& header) {
    if (header.freeImageFormat != FIF_UNKNOWN) {
	return GL_RGBA8;
    }

    // detect the image format and hand it to openGL to be used
    switch (header.format) {
	case TextureFormat_DXT5:
	    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	case TextureFormat_DXT3:
	    return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
	case TextureFormat_DXT1:
	    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
	case TextureFormat_ARGB8888:
	    return GL_RGBA8;
	case TextureFormat_RGB888:
	    return GL_RGB8;
	case TextureFormat_R8:
	    return GL_R8;
	case TextureFormat_RG88:
	    return GL_RG8;
	case TextureFormat_R16f:
	    return GL_R16F;
	case TextureFormat_RG1616f:
	    return GL_RG16F;
	case TextureFormat_RGB161616f:
	    return GL_RGB16F;
	case TextureFormat_BC7:
	    return GL_COMPRESSED_RGBA_BPTC_UNORM;
	case TextureFormat_RGBa1010102:
	    return GL_RGB10_A2;
	case TextureFormat_RGBA16161616f:
	    return GL_RGBA16F;
	default:
	    sLog.exception ("Cannot determine texture format");
    }
}

void CTexture::setupOpenGLParameters (const uint32_t textureID) const {
    configureSampling (*this->m_header, textureID, this->m_textureID[textureID]);
}

void CTexture::configureSampling (const Texture& header, uint32_t imageIndex, GLuint textureID) {
    // TODO: LABEL ELEMENTS TOO
    // bind the texture to assign information to it
    glBindTexture (GL_TEXTURE_2D, textureID);

    // set mipmap levels
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, header.images.at (imageIndex).size () - 1);

    // setup texture wrapping and filtering
    if (header.flags & TextureFlags_ClampUVs) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else if (header.flags & TextureFlags_ClampUVsBorder) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	const GLfloat transparentBlack[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	glTexParameterfv (GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, transparentBlack);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    if (header.flags & TextureFlags_NoInterpolation) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    } else {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }

    glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
}

GLuint CTexture::getTextureID (const uint32_t imageIndex) const {
    // ensure we do not go out of bounds
    if (imageIndex >= this->m_header->imageCount) {
	return this->m_textureID[0];
    }

    return this->m_textureID[imageIndex];
}

uint32_t CTexture::getTextureWidth (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureWidth;
    }

    return (*this->m_header->images[imageIndex].begin ())->width;
}

uint32_t CTexture::getTextureHeight (const uint32_t imageIndex) const {
    if (imageIndex >= this->m_header->imageCount) {
	return this->getHeader ().textureHeight;
    }

    return (*this->m_header->images[imageIndex].begin ())->height;
}

uint32_t CTexture::getRealWidth () const {
    return this->isAnimated () ? this->getHeader ().gifWidth : this->getHeader ().width;
}

uint32_t CTexture::getRealHeight () const {
    return this->isAnimated () ? this->getHeader ().gifHeight : this->getHeader ().height;
}

TextureFormat CTexture::getFormat () const { return this->getHeader ().format; }

uint32_t CTexture::getFlags () const { return this->getHeader ().flags; }

const Texture& CTexture::getHeader () const { return *this->m_header; }

const glm::vec4* CTexture::getResolution () const { return &this->m_resolution; }

const std::vector<FrameSharedPtr>& CTexture::getFrames () const { return this->getHeader ().frames; }

bool CTexture::isAnimated () const { return this->getHeader ().isAnimated (); }

uint32_t CTexture::getSpritesheetCols () const { return this->getHeader ().spritesheetCols; }

uint32_t CTexture::getSpritesheetRows () const { return this->getHeader ().spritesheetRows; }

uint32_t CTexture::getSpritesheetFrames () const { return this->getHeader ().spritesheetFrames; }

float CTexture::getSpritesheetDuration () const { return this->getHeader ().spritesheetDuration; }

void CTexture::incrementUsageCount () const {
    if (this->m_player) {
	this->m_player->incrementUsageCount ();
    }
}

void CTexture::decrementUsageCount () const {
    if (this->m_player) {
	this->m_player->decrementUsageCount ();
    }
}

void CTexture::update () const {
    if (this->m_player) {
	this->m_player->render ();
    }
}

// CTextures are always ready to be rendered at all times
bool CTexture::isReady () const { return true; }
