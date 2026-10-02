#pragma once

#include "CFBO.h"
#include <glm/vec2.hpp>
#include <algorithm>

namespace WallpaperEngine::Render {

struct FramebufferCaptureSource {
    GLuint framebuffer;
    glm::ivec2 extent;
    glm::vec4 uv; // u start/end, v start/end
};

// Read the selected framebuffer's rendered extent, which can differ from
// authored scene dimensions after an output resize. Scene roots already
// apply presentation cropping in their projection; video/web roots retain
// the UV crop applied by their final presentation draw.
inline FramebufferCaptureSource framebufferCaptureSource (
    const CFBO& target, glm::vec4 presentationUV, bool projectedPresentation, bool vflip) {
    if (projectedPresentation)
        presentationUV = {0.0f, 1.0f, vflip ? 0.0f : 1.0f, vflip ? 1.0f : 0.0f};
    return {target.getFramebuffer (), {target.getRealWidth (), target.getRealHeight ()}, presentationUV};
}

inline glm::vec4 framebufferCaptureSlice (glm::vec4 uv, glm::vec4 slice) {
    return {uv.x + slice.x * (uv.y - uv.x), uv.x + slice.y * (uv.y - uv.x),
            uv.z + slice.z * (uv.w - uv.z), uv.z + slice.w * (uv.w - uv.z)};
}

// UV endpoints describe the final quad's top and bottom. Sampling output
// pixel centers directly produces PNG rows; another vflip would reverse the
// already oriented image and duplicate the endpoint row.
inline glm::ivec2 framebufferCapturePixel (
    glm::ivec2 sourceExtent, glm::vec4 uv, glm::ivec2 pixel, glm::ivec2 outputExtent) {
    const float u = uv.x + (pixel.x + 0.5f) / outputExtent.x * (uv.y - uv.x);
    const float v = uv.z + (pixel.y + 0.5f) / outputExtent.y * (uv.w - uv.z);
    return {std::clamp (static_cast<int> (u * sourceExtent.x), 0, sourceExtent.x - 1),
            std::clamp (static_cast<int> (v * sourceExtent.y), 0, sourceExtent.y - 1)};
}

}
