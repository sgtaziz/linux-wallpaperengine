#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Material.h"

TEST_CASE ("Depth write requires a non-blended pass with authored depthwrite enabled", "[render][depth]") {
    using namespace WallpaperEngine::Data::Model;
    // Native per-draw state (O1, R-norm-nodw, R-add, R-trans captures): only normal+enabled writes depth.
    REQUIRE (resolveDepthWrite (BlendingMode_Normal, DepthwriteMode_Enabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Normal, DepthwriteMode_Disabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Additive, DepthwriteMode_Enabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Additive, DepthwriteMode_Disabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Translucent, DepthwriteMode_Enabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Translucent, DepthwriteMode_Disabled));
    // Unknown blending issues no blend call (blend off) and so follows the authored depthwrite.
    REQUIRE (resolveDepthWrite (BlendingMode_Unknown, DepthwriteMode_Enabled));
    REQUIRE_FALSE (resolveDepthWrite (BlendingMode_Unknown, DepthwriteMode_Unknown));
}
