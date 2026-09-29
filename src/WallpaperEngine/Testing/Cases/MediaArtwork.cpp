#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Media/MediaArtwork.h"
#include "WallpaperEngine/Scripting/MediaEventPayloads.h"
#include <webp/encode.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace WallpaperEngine::Media;

TEST_CASE ("Media artwork requires a decodable local file", "[script][media]") {
    const auto stamp = std::chrono::steady_clock::now ().time_since_epoch ().count ();
    const auto directory = std::filesystem::temp_directory_path () /
        ("wpe-media-art-" + std::to_string (getpid ()) + "-" + std::to_string (stamp));
    struct Cleanup { std::filesystem::path path; ~Cleanup () { std::error_code error;
        std::filesystem::remove_all (path, error); } } cleanup {directory};
    std::filesystem::create_directories (directory);
    const auto artwork = directory / "art work.png";
    constexpr std::array<unsigned char, 68> png {{
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,
        0,181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,3,3,2,0,
        239,163,31,87,0,0,0,0,73,69,78,68,174,66,96,130}};
    { std::ofstream stream (artwork, std::ios::binary);
      stream.write (reinterpret_cast<const char*> (png.data ()), png.size ()); }
    std::string url = "file://" + artwork.string ();
    const auto space = url.find (' ');
    url.replace (space, 1, "%20");
    REQUIRE (localArtworkPath (url) == artwork);
    REQUIRE (canDecodeLocalArtwork (url));
    MediaArtworkProbeCache cache;
    const auto initial = cache.load (url);
    REQUIRE (initial != nullptr);
    REQUIRE (cache.load (url) == initial);
    const auto special = directory / "art?#.png";
    std::filesystem::copy_file (artwork, special);
    std::string specialUrl = "file://" + special.string ();
    specialUrl.replace (specialUrl.find ('?'), 1, "%3F");
    specialUrl.replace (specialUrl.find ('#'), 1, "%23");
    REQUIRE (localArtworkPath (specialUrl) == special);
    REQUIRE (canDecodeLocalArtwork (specialUrl));
    REQUIRE_FALSE (localArtworkPath ("file://" + special.string ()).has_value ());
    REQUIRE_FALSE (canDecodeLocalArtwork ("https://example.com/album.png"));
    REQUIRE_FALSE (canDecodeLocalArtwork ("file:///definitely/missing/art.png"));
    REQUIRE_FALSE (canDecodeLocalArtwork ("file:///bad%2Gescape.png"));
    { std::ofstream stream (artwork, std::ios::binary | std::ios::trunc);
      stream << "not an image"; }
    REQUIRE_FALSE (canDecodeLocalArtwork (url));
    REQUIRE_FALSE (cache.available (url));
    { std::ofstream stream (artwork, std::ios::binary | std::ios::trunc);
      stream.write (reinterpret_cast<const char*> (png.data ()), png.size ()); }
    const auto restored = cache.load (url);
    REQUIRE (restored != nullptr);
    REQUIRE (restored != initial);
    std::filesystem::last_write_time (artwork,
        std::filesystem::last_write_time (artwork) + std::chrono::seconds (2));
    REQUIRE (cache.load (url) != restored);
}

TEST_CASE ("Media artwork decodes WebP by signature even with a JPG filename", "[script][media]") {
    const auto path = std::filesystem::temp_directory_path () /
        ("wpe-webp-art-" + std::to_string (getpid ()) + ".jpg");
    struct Cleanup { std::filesystem::path path; ~Cleanup () { std::error_code error;
        std::filesystem::remove (path, error); } } cleanup {path};
    constexpr std::array<uint8_t, 16> pixels {{
        20, 220, 240, 255, 240, 30, 20, 255,
        20, 220, 240, 255, 240, 30, 20, 255,
    }};
    uint8_t* encoded = nullptr;
    const size_t length = WebPEncodeLosslessRGBA (pixels.data (), 2, 2, 8, &encoded);
    REQUIRE (length > 12);
    REQUIRE (encoded != nullptr);
    const std::string url = "file://" + path.string ();
    { std::ofstream file (path, std::ios::binary);
      file.write (reinterpret_cast<const char*> (encoded), static_cast<std::streamsize> (length)); }
    const auto decoded = decodeLocalArtwork (url);
    REQUIRE (decoded != nullptr);
    REQUIRE (decoded->width == 2);
    REQUIRE (decoded->height == 2);
    REQUIRE (decoded->rgba.size () == pixels.size ());
    REQUIRE (std::equal (decoded->rgba.begin (), decoded->rgba.end (), pixels.begin ()));
    { std::ofstream file (path, std::ios::binary | std::ios::trunc);
      file.write (reinterpret_cast<const char*> (encoded), 12); }
    REQUIRE_FALSE (canDecodeLocalArtwork (url));
    WebPFree (encoded);
}

TEST_CASE ("Media palette uses the retained decoded artwork snapshot", "[script][media]") {
    MediaArtwork artwork {7, 1, {
        255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
        0, 255, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 0,
    }};
    const auto palette = WallpaperEngine::Scripting::paletteFromArtwork (&artwork);
    REQUIRE (palette.primary == glm::vec3 (1.0f, 0.0f, 0.0f));
    REQUIRE (palette.secondary == glm::vec3 (0.0f, 1.0f, 0.0f));
    REQUIRE (palette.tertiary == glm::vec3 (0.0f, 0.0f, 1.0f));
    REQUIRE (palette.text == glm::vec3 (0.0f));
    REQUIRE (palette.highContrast == palette.text);
    const auto missing = WallpaperEngine::Scripting::paletteFromArtwork (nullptr);
    REQUIRE (missing.primary == glm::vec3 (0.0f));
    REQUIRE (missing.text == glm::vec3 (0.0f));
    artwork.rgba.clear ();
    REQUIRE (WallpaperEngine::Scripting::paletteFromArtwork (&artwork).primary == glm::vec3 (0.0f));
}
