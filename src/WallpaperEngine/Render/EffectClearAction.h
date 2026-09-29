#pragma once

#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Render/FBOProvider.h"

#include <GL/glew.h>

namespace WallpaperEngine::Render {
/** Run a named authored clear against the effect's current logical target mappings. */
inline bool executeEffectClearAction (const Data::Model::Effect& effect, FBOProvider& provider,
                                      const std::string& name) {
    const auto action = effect.clearFunctions.find (name);
    if (action == effect.clearFunctions.end ()) return false;

    GLint priorDraw = 0, priorRead = 0;
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &priorDraw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &priorRead);
    Data::Utils::ScopeGuard restore ([&] {
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, priorDraw);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, priorRead);
    });

    // Wallpaper Engine 2.8.42 uses the selected-index vector only for its
    // length, then clears descriptor indices 0..N-1 (1401ee411–4fe). Preserve
    // that observed behavior even when authored names select later targets.
    for (size_t index = 0; index < action->second.size () && index < effect.fbos.size (); ++index) {
        const auto& descriptor = effect.fbos[index];
        const auto target = provider.find (descriptor->name);
        if (target) target->clear (descriptor->clear.value_or (glm::vec4 (0.0f)));
    }
    return true;
}
} // namespace WallpaperEngine::Render
