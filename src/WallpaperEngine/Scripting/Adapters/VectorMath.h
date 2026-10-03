#pragma once

#include <glm/glm.hpp>

namespace WallpaperEngine::Scripting::Adapters {
// Receiver/argument contracts of the shipped 2.8 assets/scripts/jsclasses/
// baseclasses.js Vec2/Vec3/Vec4 methods. The receiver is always `this`.
template <glm::length_t N>
float lengthSqrVectorValue (const glm::vec<N, float, glm::defaultp>& receiver) {
    // lengthSqr(){ return this.x*this.x+this.y*this.y(+...); }
    return glm::dot (receiver, receiver);
}

template <glm::length_t N>
float dotVectorValue (const glm::vec<N, float, glm::defaultp>& receiver,
                      const glm::vec<N, float, glm::defaultp>& argument) {
    // dot(f){ return this.x*f.x+this.y*f.y(+...); } returns a number.
    return glm::dot (receiver, argument);
}

inline glm::vec3 crossVectorValue (const glm::vec3& receiver, const glm::vec3& argument) {
    // cross(f){ return new Vec3(this.y*f.z-this.z*f.y, ...); } is this x f.
    return glm::cross (receiver, argument);
}

template <glm::length_t N>
glm::vec<N, float, glm::defaultp> reflectVectorValue (
    const glm::vec<N, float, glm::defaultp>& receiver,
    const glm::vec<N, float, glm::defaultp>& normal) {
    // reflect(f){ return this.subtract(f.multiply(2*this.dot(f))); }
    return receiver - normal * (2.0f * glm::dot (receiver, normal));
}
}
