#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Wallpapers {

struct SceneDependencyReport {
    // Both sides of a duplicate ID, a whole cycle, and all their dependents
    // are rejected together; independent objects may still be created.
    std::map<int, std::string> rejected;
};

inline SceneDependencyReport inspectSceneDependencies (const Data::Model::ObjectList& objects) {
    using Data::Model::Object;
    SceneDependencyReport report;
    std::map<int, const Object*> byId;
    for (const auto& object : objects) {
        if (!byId.emplace (object->id, object.get ()).second) {
            report.rejected[object->id] = "Duplicate scene object id " + std::to_string (object->id);
        }
    }

    // Missing references invalidate their owners. Self-dependencies retain
    // the previous creation policy and are ignored; self-parent is a cycle.
    for (const auto& [id, object] : byId) {
        if (report.rejected.contains (id)) continue;
        for (int dependency : object->dependencies) {
            if (dependency != id && !byId.contains (dependency)) {
                report.rejected[id] = "Object " + std::to_string (id) + " has missing dependency "
                                      + std::to_string (dependency);
                break;
            }
        }
        if (object->parent && !byId.contains (*object->parent)) {
            report.rejected[id] = "Cannot find parent " + std::to_string (*object->parent)
                                  + " for object " + std::to_string (id);
        }
    }

    enum class Visit : uint8_t { Active, Complete };
    std::map<int, Visit> visited;
    std::vector<int> path;
    std::function<void (int)> visit = [&] (int id) {
        if (report.rejected.contains (id)) return;
        if (const auto prior = visited.find (id); prior != visited.end ()) {
            if (prior->second == Visit::Complete) return;
            const auto begin = std::find (path.begin (), path.end (), id);
            std::string cycle = "Scene object dependency cycle: ";
            for (auto it = begin; it != path.end (); ++it) cycle += std::to_string (*it) + " -> ";
            cycle += std::to_string (id);
            for (auto it = begin; it != path.end (); ++it) report.rejected[*it] = cycle;
            return;
        }
        visited.emplace (id, Visit::Active);
        path.push_back (id);
        const Object& object = *byId.at (id);
        for (int dependency : object.dependencies) {
            if (dependency != id && byId.contains (dependency)) visit (dependency);
        }
        if (object.parent && byId.contains (*object.parent)) visit (*object.parent);
        path.pop_back ();
        visited[id] = Visit::Complete;
    };
    for (const auto& [id, object] : byId) visit (id);

    // A child or dependent of a rejected object is also rejected. Repeated
    // passes handle chains in any ID order.
    bool changed;
    do {
        changed = false;
        for (const auto& [id, object] : byId) {
            if (report.rejected.contains (id)) continue;
            auto rejectFor = [&] (int target) {
                if (report.rejected.contains (target)) {
                    report.rejected[id] = "Object " + std::to_string (id)
                                          + " depends on invalid object " + std::to_string (target);
                    changed = true;
                }
            };
            for (int dependency : object->dependencies) {
                if (dependency != id) rejectFor (dependency);
                if (report.rejected.contains (id)) break;
            }
            if (!report.rejected.contains (id) && object->parent) rejectFor (*object->parent);
        }
    } while (changed);
    return report;
}

// A strict entry point remains useful for tests and callers that require a
// complete graph; CScene uses the report to preserve unrelated objects.
inline void validateSceneDependencies (const Data::Model::ObjectList& objects) {
    const auto report = inspectSceneDependencies (objects);
    if (!report.rejected.empty ()) throw std::invalid_argument (report.rejected.begin ()->second);
}

} // namespace WallpaperEngine::Render::Wallpapers
