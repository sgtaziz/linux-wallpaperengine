#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace WallpaperEngine::Render::Objects {

inline bool staticModelHasOnlyPadding (std::span<const uint8_t> bytes, size_t meshEnd) {
    if (meshEnd > bytes.size ()) return false;
    // Native140261880 reads a bounded C-string after the mesh section and
    // stops optional records on an empty string. Accept the observed zero
    // padding subset, retaining rejection of unsupported nonzero sections.
    const auto tail = bytes.subspan (meshEnd);
    return std::ranges::all_of (tail, [] (uint8_t value) { return value == 0; });
}

} // namespace WallpaperEngine::Render::Objects
