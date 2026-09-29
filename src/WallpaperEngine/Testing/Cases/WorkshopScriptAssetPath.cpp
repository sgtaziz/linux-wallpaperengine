#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Scripting/WorkshopScriptAssetPath.h"

#include <set>
#include <string>

using WallpaperEngine::Scripting::resolveWorkshopScriptAssetPath;

TEST_CASE ("Script asset paths resolve in the owning Workshop module", "[script][asset]") {
    const std::set<std::string> available {
        "models/workshop/2099072548/bar.json",
        "models/workshop/2115341179/bar.json",
        "models/plain.json",
        "models/workshop/2115341179/plain.json",
    };
    const auto exists = [&] (const std::string& path) { return available.contains (path); };

    REQUIRE (resolveWorkshopScriptAssetPath ("models/bar.json", "2099072548", exists)
             == "models/workshop/2099072548/bar.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/bar.json", "2115341179", exists)
             == "models/workshop/2115341179/bar.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/plain.json", "2115341179", exists)
             == "models/plain.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/bar.json", "", exists) == "models/bar.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/bar.json", "unknown", exists) == "models/bar.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/workshop/2099072548/bar.json", "2115341179", exists)
             == "models/workshop/2099072548/bar.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/missing.json", "2099072548", exists)
             == "models/missing.json");
    REQUIRE (resolveWorkshopScriptAssetPath ("models/../bar.json", "2099072548", exists)
             == "models/../bar.json");
}
