#pragma once

#include <array>
#include <algorithm>

namespace WallpaperEngine::WebBrowser {
// Native web listeners receive left then right 64-band spectra. Values above
// one are valid; clipping belongs to the wallpaper's visualization code.
inline std::array<double, 128> webAudioFrame (const float (&left)[64], const float (&right)[64]) {
    std::array<double, 128> result {};
    std::copy_n (left, 64, result.begin ());
    std::copy_n (right, 64, result.begin () + 64);
    return result;
}
}
