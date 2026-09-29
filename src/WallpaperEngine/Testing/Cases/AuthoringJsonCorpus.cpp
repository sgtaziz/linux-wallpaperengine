#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/JSON.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

TEST_CASE ("Authored JSON accepts the optional shipped source corpus", "[json][corpus]") {
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) return;

    const std::filesystem::path assets (root);
    REQUIRE (std::filesystem::is_directory (assets));
    size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator (assets)) {
        if (!entry.is_regular_file ()
            || (entry.path ().extension () != ".json" && entry.path ().extension () != ".tex-json")) continue;
        std::ifstream input (entry.path (), std::ios::binary);
        REQUIRE (input.is_open ());
        const std::string content ((std::istreambuf_iterator<char> (input)), std::istreambuf_iterator<char> ());
        INFO (entry.path ().string ());
        REQUIRE_NOTHROW (WallpaperEngine::Data::JSON::parseAuthoringJson (content, entry.path ().string ()));
        ++files;
    }
    REQUIRE (files >= 1900);
}
