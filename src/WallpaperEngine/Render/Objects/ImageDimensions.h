#pragma once

#include <glm/vec2.hpp>
#include <glm/common.hpp>
#include <cmath>
#include <optional>

namespace WallpaperEngine::Render::Objects {

// Zero is a valid logical layer dimension, but OpenGL backing targets must
// retain a nonzero extent. Do not change negative/nonfinite input semantics.
inline glm::vec2 imageBackingDimensions (glm::vec2 size) {
    if (size.x == 0.0f) size.x = 1.0f;
    if (size.y == 0.0f) size.y = 1.0f;
    return size;
}

struct LoadedImageDimensions {
    glm::vec2 logical;
    glm::vec2 geometry;
    glm::vec2 backing;
};

struct ImageDimensionFlags {
    bool autosize = false;
    bool fullscreen = false;
    bool solidlayer = false;
    bool passthrough = false;
    bool animated = false;
    bool projectlayer = false;
};

// Automatic projection is inferred before image source loading. Omitted
// authored size must not add the logical constructor default to that extent.
inline glm::vec2 imageAutoProjectionExtent (glm::vec2 origin, std::optional<glm::vec2> authoredSize) {
    return glm::abs (origin) + authoredSize.value_or (glm::vec2 (0.0f)) * 0.5f;
}

inline LoadedImageDimensions loadedImageDimensions (
    glm::vec2 authored, ImageDimensionFlags flags,
    std::optional<glm::vec2> source, glm::vec2 projection
) {
    // Native 140209360 changes autosize/fullscreen dimensions when a source
    // texture loads. Source-less solids retain their authored dimensions.
    const glm::vec2 logical = source && flags.fullscreen ? *source
        : source && flags.autosize
            ? flags.projectlayer && !flags.animated ? projection : *source
            : authored;
    // Nonanimated autosize project layers use the scene projection at
    // context+0x84/+0x88 (140209360), even when their root input has resized
    // to the output window. Fullscreen and animated source branches precede it.
    // Native 1402091e0 uses source extents for ordinary textured composites.
    // Non-fullscreen solid/passthrough composites use logical extents unless
    // the source is animated. 1401ea500 keeps each allocated axis at least 4.
    const bool logicalTarget = !source || (!flags.fullscreen
        && (flags.solidlayer || flags.passthrough) && !flags.animated);
    const glm::vec2 target = logicalTarget
        ? glm::vec2 (std::round (logical.x), std::round (logical.y)) : *source;
    return {logical, source && flags.fullscreen ? projection : logical,
            glm::max (target, glm::vec2 (4.0f))};
}

} // namespace WallpaperEngine::Render::Objects
