#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace WallpaperEngine::Render::Shaders {
// Stores successful shader translations by both complete source strings.
class ExactSourceCache {
public:
    using SourcePair = std::pair<std::string, std::string>;

    ExactSourceCache (size_t maxEntries, size_t maxBytes) :
	m_maxEntries (maxEntries), m_maxBytes (maxBytes) {}

    const SourcePair* find (const SourcePair& source) {
	const auto it = m_entries.find (source);
	if (it == m_entries.end ()) return nullptr;
	it->second.lastUse = ++m_clock;
	return &it->second.result;
    }

    void insert (SourcePair source, SourcePair result) {
	if (m_maxEntries == 0) return;
	size_t bytes = 0;
	for (const auto& part : {&source.first, &source.second, &result.first, &result.second}) {
	    if (part->size () > m_maxBytes - bytes) return;
	    bytes += part->size ();
	}
	if (const auto existing = m_entries.find (source); existing != m_entries.end ()) {
	    m_bytes -= existing->second.bytes;
	    m_entries.erase (existing);
	}
	while (!m_entries.empty () &&
	       (m_entries.size () >= m_maxEntries || m_bytes > m_maxBytes - bytes)) {
	    const auto oldest = std::min_element (m_entries.begin (), m_entries.end (),
		[] (const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; });
	    m_bytes -= oldest->second.bytes;
	    m_entries.erase (oldest);
	}
	m_bytes += bytes;
	m_entries.emplace (std::move (source), Entry {std::move (result), bytes, ++m_clock});
    }

    size_t size () const { return m_entries.size (); }
    size_t bytes () const { return m_bytes; }

private:
    struct Entry {
	SourcePair result;
	size_t bytes;
	uint64_t lastUse;
    };
    const size_t m_maxEntries;
    const size_t m_maxBytes;
    std::map<SourcePair, Entry> m_entries;
    size_t m_bytes = 0;
    uint64_t m_clock = 0;
};
} // namespace WallpaperEngine::Render::Shaders
