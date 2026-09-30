#include <cmath>
#include <cstring>
#include <filesystem>

#include <lz4.h>
#include <nlohmann/json.hpp>

#include "TextureParser.h"
#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::Data::Parsers;

TextureUniquePtr TextureParser::parse (const BinaryReader& file) {
    auto result = std::make_unique<Texture> ();

    parseTextureHeader (*result, file);
    parseContainer (*result, file);

    for (uint32_t image = 0; image < result->imageCount; image++) {
	const uint32_t mipmapCount = file.nextUInt32 ();
	if (mipmapCount == 0)
	    sLog.exception ("Texture image ", image, " has no mipmaps");
	MipmapList mipmaps;

	for (uint32_t mipmap = 0; mipmap < mipmapCount; mipmap++) {
	    mipmaps.emplace_back (parseMipmap (file, *result));
	}

	result->images.emplace (image, mipmaps);
    }

    if (!result->isAnimated ()) {
	return result;
    }

    parseAnimations (*result, file);

    return result;
}

MipmapSharedPtr TextureParser::parseMipmap (const BinaryReader& file, const Texture& header) {
    auto result = std::make_shared<Mipmap> ();

    // TEXB0004 has some extra data in the header that has to be handled
    if (header.containerVersion == ContainerVersion_TEXB0004) {
	// some integers that we can ignore as they only seem to affect
	// the editor
	std::ignore = file.nextUInt32 ();
	std::ignore = file.nextUInt32 ();
	// this format includes some json in the header that we might need
	// to parse at some point...
	result->json = file.nextNullTerminatedString ();
	// last ignorable integer
	std::ignore = file.nextUInt32 ();
    }

    result->width = file.nextUInt32 ();
    result->height = file.nextUInt32 ();
    if (result->width == 0 || result->height == 0)
	sLog.exception ("Texture mipmap has zero dimensions");

    if (header.containerVersion == ContainerVersion_TEXB0004 || header.containerVersion == ContainerVersion_TEXB0003
	|| header.containerVersion == ContainerVersion_TEXB0002) {
	result->compression = file.nextUInt32 ();
	result->uncompressedSize = file.nextInt ();
    }

    result->compressedSize = file.nextInt ();

    if (result->compression != 0 && result->compression != 1)
	sLog.exception ("Unsupported texture mipmap compression: ", result->compression);
    if (result->compressedSize <= 0 ||
	(result->compression == 1 && result->uncompressedSize <= 0))
	sLog.exception ("Texture mipmap has invalid byte lengths");

    if (result->compression == 0) {
	// this might be better named as mipmap_bytes_size instead of compressedSize
	// as in uncompressed files this variable actually holds the file length
	result->uncompressedSize = result->compressedSize;
    }

    result->uncompressedData = std::unique_ptr<char[]> (new char[result->uncompressedSize]);

    if (result->compression == 1) {
	result->compressedData = std::unique_ptr<char[]> (new char[result->compressedSize]);
	// read the compressed data into the buffer
	file.next (result->compressedData.get (), result->compressedSize);
	// finally decompress it
	int bytes = LZ4_decompress_safe (
	    result->compressedData.get (), result->uncompressedData.get (), result->compressedSize,
	    result->uncompressedSize
	);

	if (bytes != result->uncompressedSize) {
	    sLog.exception ("Cannot decompress texture data to its declared size: got ", bytes,
	                    ", expected ", result->uncompressedSize);
	}
    } else {
	file.next (result->uncompressedData.get (), result->uncompressedSize);
    }

    return result;
}

FrameSharedPtr TextureParser::parseFrameV1 (const BinaryReader& file) {
    auto result = std::make_shared<Frame> ();

    result->frameNumber = file.nextUInt32 ();
    result->frametime = file.nextFloat ();
    result->x = static_cast<float> (file.nextUInt32 ());
    result->y = static_cast<float> (file.nextUInt32 ());
    result->width1 = static_cast<float> (file.nextUInt32 ());
    std::ignore = file.nextUInt32 (); // unknown
    std::ignore = file.nextUInt32 (); // unknown
    result->height1 = static_cast<float> (file.nextUInt32 ());

    return result;
}

FrameSharedPtr TextureParser::parseFrame (const BinaryReader& file) {
    auto result = std::make_shared<Frame> ();

    result->frameNumber = file.nextUInt32 ();
    result->frametime = file.nextFloat ();
    result->x = file.nextFloat ();
    result->y = file.nextFloat ();
    result->width1 = file.nextFloat ();
    result->width2 = file.nextFloat ();
    result->height2 = file.nextFloat ();
    result->height1 = file.nextFloat ();

    return result;
}

TextureMap TextureParser::parseTextureMap (const JSON& it) {
    if (!it.is_array ()) {
	return {};
    }

    TextureMap result = {};
    int textureIndex = -1;

    for (const auto& cur : it) {
	textureIndex++;

	if (cur.is_null ()) {
	    continue;
	}

	if (cur.is_object ()) {
	    const auto nameIt = cur.find ("name");
	    if (nameIt != cur.end () && nameIt->is_string ()) {
		result.emplace (textureIndex, nameIt->get<std::string> ());
	    }
	} else if (cur.is_string ()) {
	    std::string texName = cur;
	    if (!texName.empty ()) {
		result.emplace (textureIndex, texName);
	    }
	}
    }

    return result;
}

UserTextureMap TextureParser::parseUserTextureMap (const JSON& it) {
    if (!it.is_array ()) {
	return {};
    }

    UserTextureMap result = {};
    int textureIndex = -1;
    for (const auto& cur : it) {
	textureIndex++;
	if (cur.is_null ()) {
	    continue;
	}

	UserTextureSelector selector;
	if (cur.is_string ()) {
	    selector.name = cur.get<std::string> ();
	} else if (cur.is_object ()) {
	    const auto nameIt = cur.find ("name");
	    if (nameIt == cur.end () || !nameIt->is_string ()) {
		continue;
	    }
	    selector.name = nameIt->get<std::string> ();

	    const auto typeIt = cur.find ("type");
	    if (typeIt != cur.end () && typeIt->is_string ()) {
		const auto type = typeIt->get<std::string> ();
		if (type == "system") {
		    selector.source = UserTextureSource::System;
		} else if (type == "usershortcut") {
		    selector.source = UserTextureSource::UserShortcut;
		}
	    }

	    const auto keepAspectIt = cur.find ("keepaspect");
	    if (keepAspectIt != cur.end () && keepAspectIt->is_boolean ()) {
		selector.keepAspect = keepAspectIt->get<bool> ();
	    }
	} else {
	    continue;
	}

	result.emplace (textureIndex, std::move (selector));
    }

    return result;
}

TextureFormat TextureParser::parseTextureFormat (uint32_t value) {
    switch (value) {
	case TextureFormat_UNKNOWN:
	case TextureFormat_ARGB8888:
	case TextureFormat_RGB888:
	case TextureFormat_RGB565:
	case TextureFormat_DXT5:
	case TextureFormat_DXT3:
	case TextureFormat_DXT1:
	case TextureFormat_RG88:
	case TextureFormat_R8:
	case TextureFormat_RG1616f:
	case TextureFormat_R16f:
	case TextureFormat_BC7:
	case TextureFormat_RGBa1010102:
	case TextureFormat_RGBA16161616f:
	case TextureFormat_RGB161616f:
	    return static_cast<TextureFormat> (value);

	default:
	    sLog.exception ("unknown texture format: ", value);
    }
}

void TextureParser::parseTextureHeader (Texture& header, const BinaryReader& file) {
    char magic[9] = { 0 };

    file.next (magic, 9);

    if (strncmp (magic, "TEXV0005", 9) != 0) {
	sLog.exception ("unexpected texture container type: ", std::string_view (magic, 9));
    }

    file.next (magic, 9);

    if (strncmp (magic, "TEXI0001", 9) != 0) {
	sLog.exception ("unexpected texture sub-container type: ", std::string_view (magic, 9));
    }

    header.format = parseTextureFormat (file.nextUInt32 ());
    header.flags = parseTextureFlags (file.nextUInt32 ());
    header.textureWidth = file.nextUInt32 ();
    header.textureHeight = file.nextUInt32 ();
    header.width = file.nextUInt32 ();
    header.height = file.nextUInt32 ();

    // ignore some more bytes
    std::ignore = file.nextUInt32 ();
}

void TextureParser::parseContainer (Texture& header, const BinaryReader& file) {
    char magic[9] = { 0 };

    file.next (magic, 9);

    header.imageCount = file.nextUInt32 ();
    if (header.imageCount == 0)
	sLog.exception ("Texture contains no images");

    if (strncmp (magic, "TEXB0004", 9) == 0) {
	header.containerVersion = ContainerVersion_TEXB0004;
	header.freeImageFormat = parseFIF (file.nextUInt32 ());
	header.isVideoMp4 = file.nextUInt32 () == 1;

	if (header.freeImageFormat == FIF_UNKNOWN && header.isVideoMp4) {
	    header.freeImageFormat = FIF_MP4;
	}

	// default to TEXB0003 format here
	if (header.freeImageFormat != FIF_MP4) {
	    header.containerVersion = ContainerVersion_TEXB0003;
	}
    } else if (strncmp (magic, "TEXB0003", 9) == 0) {
	header.containerVersion = ContainerVersion_TEXB0003;
	header.freeImageFormat = parseFIF (file.nextUInt32 ());
    } else if (strncmp (magic, "TEXB0002", 9) == 0) {
	header.containerVersion = ContainerVersion_TEXB0002;
    } else if (strncmp (magic, "TEXB0001", 9) == 0) {
	header.containerVersion = ContainerVersion_TEXB0001;
    } else {
	sLog.exception ("unknown texture format type: ", std::string_view (magic, 9));
    }
}

void TextureParser::parseAnimations (Texture& header, const BinaryReader& file) {
    char magic[9] = { 0 };

    // image is animated, keep parsing the rest of the image info
    file.next (magic, 9);

    if (strncmp (magic, "TEXS0001", 9) == 0) {
	header.animatedVersion = AnimatedVersion_TEXS0001;
    } else if (strncmp (magic, "TEXS0002", 9) == 0) {
	header.animatedVersion = AnimatedVersion_TEXS0002;
    } else if (strncmp (magic, "TEXS0003", 9) == 0) {
	header.animatedVersion = AnimatedVersion_TEXS0003;
    } else {
	sLog.exception ("found animation information of unknown type: ", std::string_view (magic, 9));
    }

    uint32_t frameCount = file.nextUInt32 ();
    if (frameCount == 0)
	sLog.exception ("Animated texture has no frames");

    if (header.animatedVersion == AnimatedVersion_TEXS0003) {
	header.gifWidth = file.nextUInt32 ();
	header.gifHeight = file.nextUInt32 ();
    }

    while (frameCount-- > 0) {
	auto frame = header.animatedVersion == AnimatedVersion_TEXS0001
	    ? parseFrameV1 (file) : parseFrame (file);
	// Shipped particle atlases can carry zero per-frame durations and select
	// their tiles by particle lifetime instead of a texture-wide timeline.
	// Native TEXS loading retains these records; runtime timeline consumers
	// separately decline a zero-duration sequence.
	if (frame->frameNumber >= header.imageCount ||
	    !std::isfinite (frame->frametime) || frame->frametime < 0.0f)
	    sLog.exception ("Animated texture has invalid frame page or duration");
	header.frames.push_back (std::move (frame));
    }

    // ensure gif width and height is right for TEXS0001, TEXS0002
    if (header.animatedVersion == AnimatedVersion_TEXS0001 || header.animatedVersion == AnimatedVersion_TEXS0002) {
	header.gifWidth = (*header.frames.begin ())->width1;
	header.gifHeight = (*header.frames.begin ())->height1;
    }

    // Calculate spritesheet grid dimensions from animation frames
    // Spritesheets are grid-based textures where each frame is at a specific position
    if (!header.frames.empty () && header.width > 0 && header.height > 0) {
	auto& firstFrame = *header.frames.front ();
	float frameWidth = firstFrame.width1;
	float frameHeight = firstFrame.height1;

	if (frameWidth > 0.0f && frameHeight > 0.0f) {
	    const uint32_t cols = static_cast<uint32_t> (std::round (static_cast<double> (header.width) / frameWidth));
	    const uint32_t rows
		= static_cast<uint32_t> (std::round (static_cast<double> (header.height) / frameHeight));
	    const uint32_t frameCount = static_cast<uint32_t> (header.frames.size ());

	    // Only populate spritesheet metadata if the inferred grid can actually hold all frames
	    // This prevents GIFs (where frameWidth == textureWidth) from being treated as 1×1 spritesheets
	    if (cols > 0 && rows > 0 && cols * rows >= frameCount) {
		header.spritesheetCols = cols;
		header.spritesheetRows = rows;
		header.spritesheetFrames = frameCount;

		float totalDuration = 0.0f;
		for (const auto& frame : header.frames) {
		    totalDuration += frame->frametime;
		}
		header.spritesheetDuration = totalDuration;
	    }
	}
    }
}

uint32_t TextureParser::parseTextureFlags (uint32_t value) {
    // Native TEXI reader 14015c760 retains the complete flag word. Flags
    // are a bitmask, not a bounded enum; sampling/upload consumers select
    // the bits they implement. New authoring metadata must not reject an
    // otherwise supported texture payload (for example flags 0x800002).
    return value;
}

FIF TextureParser::parseFIF (uint32_t value) {
    switch (value) {
	case FIF_UNKNOWN:
	case FIF_BMP:
	case FIF_ICO:
	case FIF_JPEG:
	case FIF_JNG:
	case FIF_KOALA:
	case FIF_LBM:
	case FIF_MNG:
	case FIF_PBM:
	case FIF_PBMRAW:
	case FIF_PCD:
	case FIF_PCX:
	case FIF_PGM:
	case FIF_PGMRAW:
	case FIF_PNG:
	case FIF_PPM:
	case FIF_PPMRAW:
	case FIF_RAS:
	case FIF_TARGA:
	case FIF_TIFF:
	case FIF_WBMP:
	case FIF_PSD:
	case FIF_CUT:
	case FIF_XBM:
	case FIF_XPM:
	case FIF_DDS:
	case FIF_GIF:
	case FIF_HDR:
	case FIF_FAXG3:
	case FIF_SGI:
	case FIF_EXR:
	case FIF_J2K:
	case FIF_JP2:
	case FIF_PFM:
	case FIF_PICT:
	case FIF_RAW:
	case FIF_WEBP:
	case FIF_JXR:
	    return static_cast<FIF> (value);

	default:
	    sLog.exception ("unknown free image format: ", value);
    }
}

TextureUniquePtr TextureParser::parse (
    const BinaryReader& file, const std::string& filename,
    std::function<std::string (const std::string&)> metadataLoader
) {
    // Parse the binary .tex file first
    auto result = parse (file);

    // Try to load optional .tex-json metadata for spritesheet data
    if (metadataLoader) {
	parseSpritesheetMetadata (*result, filename, metadataLoader);
    }

    return result;
}

void TextureParser::parseSpritesheetMetadata (
    Texture& header, const std::string& filename, std::function<std::string (const std::string&)> metadataLoader
) {
    std::string texJsonContent;
    try {
	texJsonContent = metadataLoader (filename + ".tex-json");
    } catch (const std::filesystem::filesystem_error& e) {
	if (e.code () != std::errc::no_such_file_or_directory) {
	    sLog.error ("Cannot load spritesheet metadata ", filename, ".tex-json: ", e.what ());
	}
	return;
    } catch (const std::exception& e) {
	sLog.error ("Cannot load spritesheet metadata ", filename, ".tex-json: ", e.what ());
	return;
    }
    try {
	auto texJson = WallpaperEngine::Data::JSON::parseAuthoringJson (
	    texJsonContent, filename + ".tex-json");

	// Check for spritesheet sequences
	if (texJson.contains ("spritesheetsequences") && texJson["spritesheetsequences"].is_array ()) {
	    auto& sequences = texJson["spritesheetsequences"];
	    if (!sequences.empty ()) {
		auto& firstSeq = sequences[0];
		int frames = firstSeq.value ("frames", 0);
		float frameWidth = firstSeq.value ("width", 0.0f);
		float frameHeight = firstSeq.value ("height", 0.0f);
		float duration = firstSeq.value ("duration", 1.0f);

		if (frames > 0 && frameWidth > 0.0f && frameHeight > 0.0f && header.width > 0 && header.height > 0) {
		    // Calculate grid dimensions from texture size and frame size
		    header.spritesheetCols = static_cast<uint32_t> (std::round (header.width / frameWidth));
		    header.spritesheetRows = static_cast<uint32_t> (std::round (header.height / frameHeight));
		    header.spritesheetFrames = static_cast<uint32_t> (frames);
		    header.spritesheetDuration = duration;
		}
	    }
	}
	} catch (const std::exception& e) {
	sLog.error ("Cannot parse spritesheet metadata ", filename, ".tex-json: ", e.what ());
    }
}
