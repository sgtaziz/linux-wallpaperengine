#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

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
