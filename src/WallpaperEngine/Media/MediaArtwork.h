#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Media {

// The album texture adapter currently supports local artwork only. Keep its
// URI resolution and SceneScript hasThumbnail probe on the same file path.
std::optional<std::filesystem::path> localArtworkPath (std::string_view url);

struct MediaArtwork {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

std::shared_ptr<const MediaArtwork> decodeLocalArtwork (std::string_view url);

// True only when a bounded local file is a decodable image. A URL alone does
// not imply that `$mediaThumbnail` can be displayed.
bool canDecodeLocalArtwork (std::string_view url);

class MediaArtworkProbeCache {
public:
    std::shared_ptr<const MediaArtwork> load (std::string_view url);
    bool available (std::string_view url);
private:
    std::filesystem::path m_path;
    std::uintmax_t m_size = 0;
    std::filesystem::file_time_type m_modified {};
    bool m_cached = false;
    std::shared_ptr<const MediaArtwork> m_artwork;
};

}
