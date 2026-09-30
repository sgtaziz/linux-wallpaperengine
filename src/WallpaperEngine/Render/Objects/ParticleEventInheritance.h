#pragma once

#include "WallpaperEngine/Render/Objects/ParticleCore.h"

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Wallpaper Engine 2.8.42 1402611f0 selector order. Unknown authored values
// become Noop rather than silently choosing another inherited channel.
enum class EventInheritanceMode {
    SetColor, MultiplyColor, SetOpacity, MultiplyOpacity,
    SetColorOpacity, MultiplyColorOpacity, SetVelocity, AddVelocity,
    SetSize, MultiplySize, SetRotation, AddRotation,
    SetAngularVelocity, AddAngularVelocity, Noop
};

struct EventParticleValues {
    glm::vec3 color { 1.0f };
    float alpha { 1.0f };
    float size { 1.0f };
    glm::vec3 velocity { 0.0f };
    glm::vec3 rotation { 0.0f };
    glm::vec3 angularVelocity { 0.0f };
    bool rotationValid { false };
    bool angularVelocityValid { false };
    bool valid { false };
    bool alive { false };
};

inline bool applyEventInheritance (EventParticleValues& child,
                                   const EventParticleValues& parent,
                                   EventInheritanceMode mode,
                                   float strength = 1.0f,
                                   bool requireAlive = false,
                                   bool envelopeActive = false) {
    // 14023b340 case16 accepts an allocated parent slot even after expiry;
    // 14023fbc0 cases20/40 additionally require its live marker >0. Sources
    // are current parent values, not that parent's initializer baseline.
    if (!parent.valid || (requireAlive && !parent.alive)) return false;
    const auto set = [strength, envelopeActive] (auto& destination, const auto& source) {
        if (strength == 1.0f && !envelopeActive) destination = source;
        else destination += (source - destination) * strength;
    };
    const auto multiply = [strength, envelopeActive] (auto& destination, const auto& source) {
        if (strength == 1.0f && !envelopeActive) destination *= source;
        else destination *= (source - 1.0f) * strength + 1.0f;
    };
    switch (mode) {
        case EventInheritanceMode::SetColor: set (child.color, parent.color); break;
        case EventInheritanceMode::MultiplyColor: multiply (child.color, parent.color); break;
        case EventInheritanceMode::SetOpacity: set (child.alpha, parent.alpha); break;
        case EventInheritanceMode::MultiplyOpacity: multiply (child.alpha, parent.alpha); break;
        case EventInheritanceMode::SetColorOpacity:
            set (child.color, parent.color); set (child.alpha, parent.alpha); break;
        case EventInheritanceMode::MultiplyColorOpacity:
            multiply (child.color, parent.color); multiply (child.alpha, parent.alpha); break;
        case EventInheritanceMode::SetVelocity: set (child.velocity, parent.velocity); break;
        case EventInheritanceMode::AddVelocity: child.velocity += parent.velocity * strength; break;
        case EventInheritanceMode::SetSize: set (child.size, parent.size); break;
        case EventInheritanceMode::MultiplySize: multiply (child.size, parent.size); break;
        case EventInheritanceMode::SetRotation:
        case EventInheritanceMode::AddRotation:
            if (!(child.rotationValid && parent.rotationValid)) return false;
            if (mode == EventInheritanceMode::SetRotation) set (child.rotation, parent.rotation);
            else child.rotation += parent.rotation * strength;
            break;
        case EventInheritanceMode::SetAngularVelocity:
        case EventInheritanceMode::AddAngularVelocity:
            if (!(child.angularVelocityValid && parent.angularVelocityValid)) return false;
            if (mode == EventInheritanceMode::SetAngularVelocity)
                set (child.angularVelocity, parent.angularVelocity);
            else child.angularVelocity += parent.angularVelocity * strength;
            break;
        case EventInheritanceMode::Noop: return false;
        default: return false;
    }
    return true;
}

inline glm::vec3 inheritedControlPointVelocity (
    std::mt19937& rng, glm::vec3 currentPosition, glm::vec3 previousPosition,
    float elapsed, float factorMin = 0.1f, float factorMax = 0.2f,
    glm::mat3 birthBasis = glm::mat3 (1.0f),
    glm::mat3 sceneInverseBasis = glm::mat3 (1.0f),
    bool particleWorldSpace = false, bool controlPointWorldSpace = false) {
    // 14023b340 case8: one MT draw even for a stationary CP or equal bounds.
    // Both transforms apply to vectors (w=0), so translation is excluded.
    glm::vec3 velocity = (currentPosition - previousPosition) / elapsed;
    if (particleWorldSpace && !controlPointWorldSpace)
        velocity = sceneInverseBasis * velocity;
    const float factor = nativeRandomUnit (rng) * (factorMax - factorMin) + factorMin;
    return birthBasis * (velocity * factor);
}

}
