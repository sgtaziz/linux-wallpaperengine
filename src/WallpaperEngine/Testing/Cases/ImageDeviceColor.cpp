#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/Objects/ImageDeviceColor.h"

using WallpaperEngine::Render::Objects::imageDeviceColor;

TEST_CASE ("Image device color applies scripted brightness only in HDR", "[image][brightness]") {
    const glm::vec4 authored (0.5f, 0.25f, 0.75f, 0.8f);
    const auto hdr = imageDeviceColor (authored, 0.5f, 1.3f, true);
    CHECK (hdr.r == 0.65f);
    CHECK (hdr.g == 0.325f);
    CHECK (hdr.b == Catch::Approx (0.975f));
    CHECK (hdr.a == 0.4f);

    const auto sdr = imageDeviceColor (authored, 0.5f, 1.3f, false);
    CHECK (sdr.r == authored.r);
    CHECK (sdr.g == authored.g);
    CHECK (sdr.b == authored.b);
    CHECK (sdr.a == hdr.a);
}
