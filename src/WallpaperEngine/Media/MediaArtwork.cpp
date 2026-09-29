#include "MediaArtwork.h"

#include <stb_image.h>
#include <webp/decode.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace WallpaperEngine::Media;

namespace {
int hexDigit (char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}
}

std::optional<std::filesystem::path> WallpaperEngine::Media::localArtworkPath (std::string_view url) {
    if (!url.starts_with ("file://")) return std::nullopt;
    url.remove_prefix (7);
    if (url.starts_with ("localhost/")) url.remove_prefix (9);
    if (!url.starts_with ('/')) return std::nullopt;
    std::string decoded;
    decoded.reserve (url.size ());
    for (std::size_t index = 0; index < url.size (); ++index) {
        char value = url[index];
        if (value == '?' || value == '#') return std::nullopt;
        if (value == '%') {
            if (index + 2 >= url.size ()) return std::nullopt;
            const int high = hexDigit (url[++index]);
            const int low = hexDigit (url[++index]);
            if (high < 0 || low < 0) return std::nullopt;
            value = static_cast<char> ((high << 4) | low);
        }
        if (value == '\0') return std::nullopt;
        decoded.push_back (value);
    }
    return std::filesystem::path (decoded);
}

std::shared_ptr<const MediaArtwork> WallpaperEngine::Media::decodeLocalArtwork (std::string_view url) {
    const auto path = localArtworkPath (url);
    if (!path) return {};
    std::error_code error;
    if (!std::filesystem::is_regular_file (*path, error) || error) return {};
    const auto size = std::filesystem::file_size (*path, error);
    constexpr std::uintmax_t maxBytes = 16 * 1024 * 1024;
    if (error || size == 0 || size > maxBytes || size > static_cast<std::uintmax_t> (std::numeric_limits<int>::max ()))
        return {};
    std::ifstream stream (*path, std::ios::binary);
    if (!stream) return {};
    std::vector<stbi_uc> bytes (static_cast<std::size_t> (size));
    stream.read (reinterpret_cast<char*> (bytes.data ()), static_cast<std::streamsize> (bytes.size ()));
    if (stream.gcount () != static_cast<std::streamsize> (bytes.size ())) return {};
    // Browser MPRIS integrations can write WebP bytes to a file named .jpg.
    // Inspect the bytes, not the extension, before falling back to stb_image.
    const bool webp = bytes.size () >= 12 &&
        std::memcmp (bytes.data (), "RIFF", 4) == 0 &&
        std::memcmp (bytes.data () + 8, "WEBP", 4) == 0;
    if (webp) {
        int width = 0, height = 0;
        if (!WebPGetInfo (bytes.data (), bytes.size (), &width, &height) ||
            width <= 0 || height <= 0 || width > 4096 || height > 4096) return {};
        auto artwork = std::make_shared<MediaArtwork> ();
        artwork->width = width;
        artwork->height = height;
        artwork->rgba.resize (static_cast<std::size_t> (width) * height * 4);
        if (!WebPDecodeRGBAInto (bytes.data (), bytes.size (), artwork->rgba.data (),
                                static_cast<int> (artwork->rgba.size ()), width * 4)) return {};
        return artwork;
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory (bytes.data (), static_cast<int> (bytes.size ()),
                                &width, &height, &channels)
        || width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return {};
    std::unique_ptr<stbi_uc, decltype (&stbi_image_free)> image (
        stbi_load_from_memory (bytes.data (), static_cast<int> (bytes.size ()),
                               &width, &height, &channels, 4), &stbi_image_free);
    if (!image) return {};
    auto artwork = std::make_shared<MediaArtwork> ();
    artwork->width = width;
    artwork->height = height;
    artwork->rgba.assign (image.get (), image.get () + static_cast<std::size_t> (width) * height * 4);
    return artwork;
}

bool WallpaperEngine::Media::canDecodeLocalArtwork (std::string_view url) {
    return decodeLocalArtwork (url) != nullptr;
}

std::shared_ptr<const MediaArtwork> MediaArtworkProbeCache::load (std::string_view url) {
    const auto path = localArtworkPath (url);
    if (!path) { m_cached = false; m_artwork.reset (); return {}; }
    std::error_code error;
    const auto size = std::filesystem::file_size (*path, error);
    if (error) { m_cached = false; m_artwork.reset (); return {}; }
    const auto modified = std::filesystem::last_write_time (*path, error);
    if (error) { m_cached = false; m_artwork.reset (); return {}; }
    if (m_cached && *path == m_path && size == m_size && modified == m_modified)
	return m_artwork;
    m_path = *path;
    m_size = size;
    m_modified = modified;
    m_artwork = decodeLocalArtwork (url);
    m_cached = true;
    return m_artwork;
}

bool MediaArtworkProbeCache::available (std::string_view url) { return load (url) != nullptr; }
