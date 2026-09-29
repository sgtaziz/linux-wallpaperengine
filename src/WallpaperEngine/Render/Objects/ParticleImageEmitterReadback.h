#pragma once

#include "ParticleCore.h"
#include "WallpaperEngine/Render/TextureProvider.h"

#include <GL/glew.h>
#include <vector>

namespace WallpaperEngine::Render::Objects {

// Native 1401d3ae0 renders a four-diagonal-tap quarter source before the
// alpha-threshold/X-major sample scan. Keep GL state local to the readback:
// particle emission occurs inside the scene render pass.
class ParticleImageEmitterReadback {
public:
    ParticleImageEmitterReadback () = default;
    ParticleImageEmitterReadback (const ParticleImageEmitterReadback&) = delete;
    ParticleImageEmitterReadback& operator= (const ParticleImageEmitterReadback&) = delete;
    ~ParticleImageEmitterReadback ();

    [[nodiscard]] bool sample (const TextureProvider& source, const TextureProvider* mask,
        std::vector<ParticleCore::ImageEmitterSample>& output);

private:
    [[nodiscard]] bool ensureProgram ();
    GLuint m_program = 0;
    GLuint m_vertexArray = 0;
};

} // namespace WallpaperEngine::Render::Objects
