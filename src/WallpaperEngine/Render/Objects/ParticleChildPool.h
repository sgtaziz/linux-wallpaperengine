#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Native builds static
// children and event descriptor groups only for nodes below depth 10. Event
// nodes are constructed through the same function, so it bounds every chain,
// including self-referencing assets; there is no separate cycle refusal.
inline constexpr uint32_t kNativeChildDepthLimit = 10u;

[[nodiscard]] constexpr bool nativeNodeOwnsChildren (uint32_t depth) {
    return depth < kNativeChildDepthLimit;
}

struct NativeDeathDescriptor {
    size_t descriptor;
    float probability;
    uint32_t maxCount;
};
using NativeBirthDescriptor = NativeDeathDescriptor;

// Native. One budget, sum(maxcount - active) over the death
// descriptors, is computed per tick before any spawn. Each expired particle
// dispatches only while the budget is non-zero. Each death descriptor then
// draws once and spawns on draw <= probability, decrements the shared budget
// and stops this particle's walk when that descriptor reaches active ==
// maxcount. No descriptor checks its own cap first, so a descriptor can pass
// it and the budget can go negative (and stay non-zero).
// active(descriptor) -> count, draw() -> float, spawn(expiredIndex, descriptor).
template <typename Active, typename Draw, typename Spawn>
void dispatchNativeDeathEvents (size_t expiredCount, const std::vector<NativeDeathDescriptor>& descriptors,
                                Active&& active, Draw&& draw, Spawn&& spawn) {
    if (expiredCount == 0 || descriptors.empty ()) return;
    int64_t budget = 0;
    for (const auto& death : descriptors)
        budget += static_cast<int64_t> (death.maxCount) - static_cast<int64_t> (active (death.descriptor));
    for (size_t expired = 0; expired < expiredCount; ++expired) {
        if (budget == 0) continue;
        for (const auto& death : descriptors) {
            if (!(draw () <= death.probability)) continue;
            spawn (expired, death.descriptor);
            --budget;
            if (static_cast<uint64_t> (active (death.descriptor)) == death.maxCount) break;
        }
    }
}

// Native, after the full emitter stage: descriptors
// outermost, then the slots born this tick. A full descriptor is skipped;
// otherwise each birth draws once and spawns on draw <= probability, and the
// walk stops at active == maxcount. Births alone never pass maxcount. A
// spawned child's warm-up draws come from spawn(), before the next draw.
// active(descriptor) -> count, draw() -> float, spawn(bornIndex, descriptor).
template <typename Active, typename Draw, typename Spawn>
void dispatchNativeBirthEvents (size_t bornCount, const std::vector<NativeBirthDescriptor>& descriptors,
                                Active&& active, Draw&& draw, Spawn&& spawn) {
    for (const auto& birth : descriptors) {
        for (size_t born = 0; born < bornCount; ++born) {
            if (active (birth.descriptor) >= birth.maxCount) break;
            if (!(draw () <= birth.probability)) continue;
            spawn (born, birth.descriptor);
        }
    }
}

// Native records expired slots, which follow detach and death dispatch walk,
// only for death/follow event bits or a rope renderer buffer.
[[nodiscard]] constexpr bool nativeRecordsExpiredSlots (bool eventsEnabled, bool ropeRenderer) {
    return eventsEnabled || ropeRenderer;
}

// Native writes an event node's anchor B·T(particle)·descriptor.
// B is identity when the child and parent presets agree, the parent stack S
// for a world child of a local parent, and S⁻¹ for a local child of a world
// parent; nullopt when that S is singular.
[[nodiscard]] inline std::optional<glm::mat4> nativeEventChildBasis (
    const glm::mat4& parentStack, bool childWorld, bool parentWorld) {
    if (childWorld == parentWorld) return glm::mat4 (1.0f);
    if (childWorld) return parentStack;
    return Wallpapers::inverseFiniteTransform (parentStack);
}

// a world child's stack top is its anchor; a local child's is the
// parent's current stack times the anchor. The anchor changes only at spawn
// and on live-follow ticks, never at detach.
[[nodiscard]] inline glm::mat4 nativeEventChildStack (
    const glm::mat4& currentParentStack, const glm::mat4& basis, const glm::vec3& particle,
    const glm::mat4& descriptor, bool childWorld) {
    const glm::mat4 anchor = basis * glm::translate (glm::mat4 (1.0f), particle) * descriptor;
    return childWorld ? anchor : currentParentStack * anchor;
}

// A node's draw model: substitutes the scene base for a world
// preset (identity in Linux's centered GL space, the Y flip in perspective),
// 's view-stack shift from the owning scene object applies to
// every node once. Children compose the parent's pre-parallax simulation.
[[nodiscard]] inline glm::mat4 nativeNodeDrawModel (
    const glm::vec3& ownerParallax, const glm::mat4& simulation, bool worldPreset, bool perspective) {
    const glm::mat4 base = !worldPreset ? simulation
        : perspective ? glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f)) : glm::mat4 (1.0f);
    return glm::translate (glm::mat4 (1.0f), ownerParallax) * base;
}

// Native event descriptors own separate inactive vectors. Completion appends
// in active traversal order; the next accepted event reuses the last entry.
template <typename Node> class RetainedChildPool {
public:
    void retire (size_t descriptor, Node node) {
        if (m_nodes.size () <= descriptor) m_nodes.resize (descriptor + 1);
        m_nodes[descriptor].push_back (std::move (node));
    }

    std::optional<Node> take (size_t descriptor) {
        if (descriptor >= m_nodes.size () || m_nodes[descriptor].empty ())
            return std::nullopt;
        auto& nodes = m_nodes[descriptor];
        std::optional<Node> result (std::move (nodes.back ()));
        nodes.pop_back ();
        return result;
    }

    template <typename Visitor> void visit (Visitor&& visitor) {
        for (auto& descriptor : m_nodes)
            for (auto& node : descriptor) visitor (node);
    }

private:
    std::vector<std::vector<Node>> m_nodes;
};

} // namespace WallpaperEngine::Render::Objects::ParticleCore
