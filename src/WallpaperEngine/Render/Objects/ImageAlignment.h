#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace WallpaperEngine::Render::Objects {

// 1402065e0/140209360 cast the geometry inputs to signed integers before
// 1402066a0 stores the local alignment offset. Match CVTT on invalid inputs.
inline float imageAlignmentDimension (float value) {
    if (!std::isfinite (value) || value >= 2147483648.0f || value < -2147483648.0f)
        return static_cast<float> (std::numeric_limits<int32_t>::min ());
    return std::trunc (value);
}

struct ImageEmissionAlignment {
    glm::vec2 offset {0.0f};
    bool centered = true;

    // Geometry loading and the public alignment callback rebuild this
    // stored offset. A subsequent logical-size change alone is not a getter.
    void rebuild (std::string_view alignment, glm::vec2 builtSize) {
        offset = glm::vec2 (0.0f);
        centered = alignment == "center";
        const float halfWidth = imageAlignmentDimension (builtSize.x) * 0.5f;
        const float halfHeight = imageAlignmentDimension (builtSize.y) * 0.5f;
        if (alignment == "top") offset = {0, -halfHeight};
        else if (alignment == "topright") offset = {-halfWidth, -halfHeight};
        else if (alignment == "right") offset = {-halfWidth, 0};
        else if (alignment == "bottomright") offset = {-halfWidth, halfHeight};
        else if (alignment == "bottom") offset = {0, halfHeight};
        else if (alignment == "bottomleft") offset = {halfWidth, halfHeight};
        else if (alignment == "left") offset = {halfWidth, 0};
        else if (alignment == "topleft") offset = {halfWidth, -halfHeight};
        else centered = true;
    }

    [[nodiscard]] glm::mat4 world (glm::mat4 authoredWorld) const {
        if (centered) return authoredWorld;
        // Original 1401fd3f0 adds X to the original translation first,
        // then Y, then Z. It copies all basis entries and homogeneous W.
        for (int axis = 0; axis < 3; ++axis)
            authoredWorld[3][axis] = authoredWorld[2][axis] * 0.0f
                + (authoredWorld[1][axis] * offset.y
                   + (authoredWorld[0][axis] * offset.x + authoredWorld[3][axis]));
        return authoredWorld;
    }
};

} // namespace WallpaperEngine::Render::Objects
