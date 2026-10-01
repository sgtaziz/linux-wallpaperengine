#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Canonical native physical streams. Slot must expose age, lifetime and alive;
// all other streams belong to the slot and survive expiry and public stop.
// Native count is intentionally independent of nonzero lifetime markers: a
// zero-lifetime birth increments it, although that slot is absent from draws.
template <typename Slot> class NativeSlotStreams {
public:
    NativeSlotStreams () = default;
    NativeSlotStreams (uint32_t capacity, const Slot& zeroedStreams)
        : m_slots ((capacity + 3u) & ~3u, zeroedStreams), m_capacity (capacity) {}

    [[nodiscard]] uint32_t capacity () const { return m_capacity; }
    [[nodiscard]] uint32_t highWater () const { return m_highWater; }
    [[nodiscard]] uint32_t paddedHighWater () const { return (m_highWater + 3u) & ~3u; }
    [[nodiscard]] uint32_t nativeCount () const { return m_nativeCount; }
    [[nodiscard]] std::vector<Slot>& streams () { return m_slots; }
    [[nodiscard]] const std::vector<Slot>& streams () const { return m_slots; }

    // 1402378a0 starts this cursor once for the whole emitter-record pass.
    // Every birth advances it, including a birth whose initializer sets zero
    // lifetime. Such a slot becomes reusable on the next emission pass.
    void beginEmissionPass () { m_birthCursor = 0; }

    [[nodiscard]] std::optional<uint32_t> nextBirthSlot () {
        if (m_nativeCount >= m_capacity) return std::nullopt;
        while (m_birthCursor < m_capacity && m_slots[m_birthCursor].lifetime != 0.0f)
            ++m_birthCursor;
        if (m_birthCursor >= m_capacity) return std::nullopt;
        return m_birthCursor;
    }

    void commitBirth (uint32_t index) {
        // The native initializer runs before either counter is published.
        ++m_nativeCount;
        if (index == m_highWater) ++m_highWater;
        ++m_birthCursor;
    }

    template <typename Initialize> std::optional<uint32_t> birth (Initialize&& initialize) {
        const auto index = nextBirthSlot ();
        if (!index) return std::nullopt;
        std::forward<Initialize> (initialize) (m_slots[*index], *index);
        commitBirth (*index);
        return index;
    }

    // 140236cd0 ages every four-lane block, including padded/dead lanes. Its
    // ordered expiry comparison leaves NaN markers occupied and clears only
    // the lifetime marker of expired lanes, retaining their other streams.
    template <typename Expired> void ageAndExpire (float duration, Expired&& expired) {
        for (uint32_t index = 0; index < paddedHighWater (); ++index) {
            auto& slot = m_slots[index];
            slot.age += duration;
            if (slot.lifetime != 0.0f && slot.age > slot.lifetime) {
                slot.lifetime = 0.0f;
                slot.alive = false;
                --m_nativeCount;
                expired (slot, index);
            }
        }
    }

    // Native memcpy resets cover exactly highwater, whereas SIMD operators
    // cover paddedHighWater. The distinction is observable in never-born tail.
    template <typename Restore> void restore (Restore&& restoreSlot) {
        for (uint32_t index = 0; index < m_highWater; ++index)
            restoreSlot (m_slots[index]);
    }

    template <typename Visit> void visitDrawSlots (Visit&& visit) const {
        if (m_nativeCount == 0) return;
        for (uint32_t index = 0; index < m_highWater; ++index)
            if (m_slots[index].lifetime != 0.0f) visit (m_slots[index], index);
    }

    void stop () {
        for (auto& slot : m_slots) {
            slot.lifetime = 0.0f;
            slot.alive = false;
        }
        m_highWater = 0;
        m_nativeCount = 0;
        m_birthCursor = 0;
    }

private:
    std::vector<Slot> m_slots;
    uint32_t m_capacity { 0 };
    uint32_t m_highWater { 0 };
    uint32_t m_nativeCount { 0 };
    uint32_t m_birthCursor { 0 };
};

} // namespace WallpaperEngine::Render::Objects::ParticleCore
