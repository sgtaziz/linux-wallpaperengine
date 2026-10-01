#pragma once

#include <glm/glm.hpp>

namespace WallpaperEngine::Scripting::Adapters {
// SceneScript subtraction returns a new value; it never updates its receiver.
// Native 2.8 baseclasses.js Vec2/3/4 and the controlled numeric probes agree.
template <glm::length_t N>
glm::vec<N, float, glm::defaultp> subtractVectorValue (
    const glm::vec<N, float, glm::defaultp>& receiver,
    const glm::vec<N, float, glm::defaultp>& argument) {
    return receiver - argument;
}
}
