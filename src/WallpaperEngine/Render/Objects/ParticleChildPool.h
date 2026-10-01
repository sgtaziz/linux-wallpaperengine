#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace WallpaperEngine::Render::Objects::ParticleCore {

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
