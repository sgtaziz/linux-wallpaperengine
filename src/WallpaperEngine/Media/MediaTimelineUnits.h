#pragma once

#include <cstdint>

namespace WallpaperEngine::Media {
// MPRIS Position and mpris:length are signed microseconds; SceneScript exposes seconds.
[[nodiscard]] constexpr double mprisMicrosecondsToSeconds (int64_t value) {
    return static_cast<double> (value) / 1000000.0;
}
}
