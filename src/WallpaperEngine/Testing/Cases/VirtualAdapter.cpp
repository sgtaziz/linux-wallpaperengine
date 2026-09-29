#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/FileSystem/Adapters/Virtual.h"
#include "WallpaperEngine/FileSystem/Adapters/Package.h"

#include <iterator>
#include <sstream>
#include <string>

using WallpaperEngine::FileSystem::Adapters::VirtualAdapter;

TEST_CASE ("Virtual files give each open an independent read cursor", "[filesystem][virtual]") {
    VirtualAdapter files;
    files.add ("reopened.txt", "first\nsecond\n");

    auto first = files.open ("reopened.txt");
    std::string line;
    REQUIRE (std::getline (*first, line));
    REQUIRE (line == "first");

    auto concurrent = files.open ("reopened.txt");
    REQUIRE (std::getline (*concurrent, line));
    REQUIRE (line == "first");
    REQUIRE (std::getline (*first, line));
    REQUIRE (line == "second");
    REQUIRE (std::getline (*concurrent, line));
    REQUIRE (line == "second");

    auto reopened = files.open ("reopened.txt");
    REQUIRE (std::getline (*reopened, line));
    REQUIRE (line == "first");
}

TEST_CASE ("Packaged paths use exact names before a unique ASCII case fallback", "[filesystem][package]") {
    using namespace WallpaperEngine::Data::Assets;
    using WallpaperEngine::Data::Utils::BinaryReader;
    using WallpaperEngine::FileSystem::Adapters::PackageAdapter;

    auto package = std::make_unique<Package> ();
    package->file = std::make_unique<BinaryReader> (std::make_shared<std::istringstream> ("ABC"));
    package->baseOffset = 0;
    const auto add = [&] (std::string name, uint32_t offset) {
        package->files.push_back (std::make_unique<FileEntry> (
            FileEntry {.filename = std::move (name), .offset = offset, .length = 1}));
    };
    add ("materials/UI phai.JSON", 0);
    add ("materials/Other.TEX", 1);
    add ("materials/other.tex", 2);
    PackageAdapter files (std::move (package));

    REQUIRE (files.exists ("materials/ui phai.json"));
    REQUIRE (files.open ("materials/ui phai.json")->get () == 'A');
    REQUIRE (files.exists ("materials/Other.TEX"));
    REQUIRE (files.open ("materials/Other.TEX")->get () == 'B');
    REQUIRE (files.open ("materials/other.tex")->get () == 'C');
    REQUIRE_FALSE (files.exists ("materials/OTHER.tex"));
    REQUIRE_THROWS (files.open ("materials/OTHER.tex"));
    REQUIRE_FALSE (files.exists ("materials/missing.json"));
}
