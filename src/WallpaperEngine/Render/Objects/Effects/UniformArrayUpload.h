#pragma once

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace WallpaperEngine::Render::Objects::Effects {

inline void uploadVec4Array (GLint location, const glm::vec4* values, GLsizei count) {
    glUniform4fv (location, count, glm::value_ptr (*values));
}

inline void uploadMat4Array (GLint location, const glm::mat4* values, GLsizei count) {
    glUniformMatrix4fv (location, count, GL_FALSE, glm::value_ptr (*values));
}

} // namespace WallpaperEngine::Render::Objects::Effects
