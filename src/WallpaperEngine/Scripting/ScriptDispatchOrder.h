#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace WallpaperEngine::Scripting {

// Native keeps one script list that
// appends each compiled script, and update, resize, media, cursor,
// applyUserProperties and destroy all walk that list. Scripts are created while
// the scene loads: the general settings first, then objects in
// their array order, each by loader phase. The loader walks a
// JSON object's keys through jsoncpp's std::map, so within a phase the keys
// register in byte order. Scripts created later are appended.
enum class ScriptPhase : uint32_t {
    // An image's base material and a
    // particle's instanceoverride register before Load.
    BeforeLoad = 0,
    // Image animation layers precede the image's own keys.
    AnimationLayers = 1,
    Own = 2,
    // Effects and model animation layers follow.
    AfterOwn = 3,
};

struct ScriptPlacement {
    ScriptPhase phase = ScriptPhase::Own;
    // Layer, effect or pass indices inside the phase, outermost first.
    uint32_t group = 0;
    uint32_t subgroup = 0;
    // The JSON key the loader reads; empty keeps creation order.
    std::string key;
};

struct ScriptDispatchRank {
    // -1 for scene general settings; scene objects by array index; scripts
    // created after the scene loaded sort after every loaded one.
    int64_t objectIndex = 0;
    ScriptPlacement placement;
    size_t queueOrder = 0;

    static constexpr int64_t kGeneral = -1;
    static constexpr int64_t kAppended = std::numeric_limits<int64_t>::max ();

    friend bool operator< (const ScriptDispatchRank& left, const ScriptDispatchRank& right) {
        const auto tie = [] (const ScriptDispatchRank& rank) {
            // Runtime-created scripts keep their creation order only.
            if (rank.objectIndex == kAppended)
                return std::make_tuple (rank.objectIndex, uint32_t {0}, uint32_t {0}, uint32_t {0},
                                        std::string_view {}, rank.queueOrder);
            return std::make_tuple (rank.objectIndex, static_cast<uint32_t> (rank.placement.phase),
                                    rank.placement.group, rank.placement.subgroup,
                                    std::string_view (rank.placement.key), rank.queueOrder);
        };
        return tie (left) < tie (right);
    }
};

// JSON keys of the script-capable fields whose script binding names differ.
inline std::string nativeScriptJsonKey (std::string_view bindingName) {
    static constexpr std::pair<std::string_view, std::string_view> renamed[] {
        {"pointSize", "pointsize"}, {"limitRows", "limitrows"}, {"maxRows", "maxrows"},
        {"limitWidth", "limitwidth"}, {"maxWidth", "maxwidth"}, {"limitUseEllipsis", "limituseellipsis"},
        {"opaqueBackground", "opaquebackground"}, {"backgroundColor", "backgroundcolor"},
        {"backgroundBrightness", "backgroundbrightness"},
    };
    for (const auto& [binding, json] : renamed)
        if (binding == bindingName) return std::string (json);
    return std::string (bindingName);
}

// The registered scripts in native dispatch order.
class ScriptDispatchOrder {
public:
    void insert (const std::string& name, const ScriptDispatchRank& rank) {
        const auto position = std::upper_bound (m_entries.begin (), m_entries.end (), rank,
            [] (const ScriptDispatchRank& value, const Entry& entry) { return value < entry.rank; });
        m_entries.insert (position, Entry {rank, name});
    }

    void erase (const std::string& name) {
        std::erase_if (m_entries, [&name] (const Entry& entry) { return entry.name == name; });
    }

    void clear () { m_entries.clear (); }

    // Callers dispatch from a copy so that scripts added or removed by a
    // callback do not disturb the walk.
    [[nodiscard]] size_t size () const { return m_entries.size (); }
    [[nodiscard]] std::vector<std::string> snapshot () const {
        std::vector<std::string> names;
        names.reserve (m_entries.size ());
        for (const auto& entry : m_entries) names.push_back (entry.name);
        return names;
    }

private:
    struct Entry {
        ScriptDispatchRank rank;
        std::string name;
    };
    std::vector<Entry> m_entries;
};

} // namespace WallpaperEngine::Scripting
