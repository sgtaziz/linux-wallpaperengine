#pragma once

#include <array>
#include <cstddef>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace WallpaperEngine::Render::Objects {

// Native emits each image/effect/compose quad as four vertices,
// v0..v3 = TL, TR, BL, BR, and indexes them 0,2,1, 1,2,3, so both triangles
// share the BL-TR diagonal. Its D3D viewport puts TL on target memory row 0.
// Interpolated varyings round per triangle, so the split must match.
//
// Linux corners below are named by where they land in target memory (row 0
// first), which for these quads is also their image-content corner. GL maps
// clip -Y to memory row 0, so a Linux "top" corner has the smaller clip/ortho
// Y. Triangles keep native's diagonal but list it as (TL,TR,BL),(TR,BR,BL):
// counter-clockwise in GL window space like the former split, so culling
// materials keep facing.
inline constexpr std::array<std::size_t, 6> imageQuadCornerOrder {0, 1, 2, 1, 3, 2};

template <std::size_t N>
std::array<float, 6 * N> imageQuadVertices (
    const std::array<float, N>& topLeft, const std::array<float, N>& topRight,
    const std::array<float, N>& bottomLeft, const std::array<float, N>& bottomRight
) {
    const std::array<const std::array<float, N>*, 4> corners {&topLeft, &topRight, &bottomLeft, &bottomRight};
    std::array<float, 6 * N> vertices {};
    for (std::size_t vertex = 0; vertex < imageQuadCornerOrder.size (); ++vertex)
        for (std::size_t component = 0; component < N; ++component)
            vertices[vertex * N + component] = (*corners[imageQuadCornerOrder[vertex]])[component];
    return vertices;
}

// Local image-target quad drawn through imageTargetProjection: Y=0 is memory
// row 0 (the image's first source row).
inline std::array<float, 18> imageTargetQuadPositions (float left, float top, float right, float bottom) {
    return imageQuadVertices<3> ({left, top, 0.0f}, {right, top, 0.0f},
                                 {left, bottom, 0.0f}, {right, bottom, 0.0f});
}

inline glm::mat4 imageTargetProjection (const glm::vec2& size) {
    return glm::ortho<float> (0.0f, size.x, 0.0f, size.y);
}

// Texture-space rectangle; V=top is the source's first memory row.
inline std::array<float, 12> imageQuadTexcoords (float left, float top, float right, float bottom) {
    return imageQuadVertices<2> ({left, top}, {right, top}, {left, bottom}, {right, bottom});
}

// Identity-projected effect pass quad covering the whole target.
inline std::array<float, 18> imagePassQuadPositions () {
    return imageTargetQuadPositions (-1.0f, -1.0f, 1.0f, 1.0f);
}

// Image-local corners [0]=(-x,-y), [1]=(-x,+y), [2]=(+x,-y), [3]=(+x,+y),
// already transformed by the caller. Native's TL is local (-x,+y); Linux
// scene, lighting and prelighting projections put local +Y on the smaller
// memory row, as native does.
inline std::array<float, 18> imageLocalQuadPositions (const glm::vec3 (&corners)[4]) {
    const auto corner = [&] (std::size_t index) {
        return std::array<float, 3> {corners[index].x, corners[index].y, corners[index].z};
    };
    return imageQuadVertices<3> (corner (1), corner (3), corner (0), corner (2));
}

inline std::array<float, 18> imageLocalQuadPositions (const glm::vec2& half) {
    return imageLocalQuadPositions ({glm::vec3 (-half.x, -half.y, 0.0f), glm::vec3 (-half.x, half.y, 0.0f),
                                     glm::vec3 (half.x, -half.y, 0.0f), glm::vec3 (half.x, half.y, 0.0f)});
}

} // namespace WallpaperEngine::Render::Objects
