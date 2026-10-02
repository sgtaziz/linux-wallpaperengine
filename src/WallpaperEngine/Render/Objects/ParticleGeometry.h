#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "ParticleCore.h"

namespace WallpaperEngine::Render::Objects::ParticleCore {

// The published stream owns geometry independently of mutable simulation
// slots. Texture pages follow their shared draw-time clock. Retained nodes keep
// their last publication across
// inner-only warmup; a cold node starts with no publication.
struct RopeDrawSegment {
    glm::vec3 previous, start, end, next;
    float startSize, endSize;
};

struct GeometryPacket {
    size_t renderer;
    bool rope;
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    std::vector<RopeDrawSegment> ropeSegments;
    uint32_t subdivision = 1;
};

// Native skips stream writes for zero live particles; ordinary rope also
// skips its own stream with one particle. Each renderer group therefore keeps
// its last publication independently until that family's next write.
class GeometryPublication {
public:
    bool beginStream (size_t renderer, uint32_t liveCount, bool ordinaryRope) {
        if (liveCount == 0 || (ordinaryRope && liveCount < 2)) return false;
        if (m_streams.size () <= renderer) m_streams.resize (renderer + 1);
        m_streams[renderer].clear ();
        return true;
    }
    void append (GeometryPacket packet) {
        m_streams.at (packet.renderer).push_back (std::move (packet));
    }
    [[nodiscard]] const auto& streams () const { return m_streams; }
private:
    std::vector<std::vector<GeometryPacket>> m_streams;
};

inline bool automaticEmissionAllowed (bool enabled, bool paused, uint32_t forcedCount) {
    return (enabled && !paused) || forcedCount != 0;
}

// Native stores segment attributes in its stream, but its geometry shader
// expands width using the current draw matrix. Recompute only those derived
// fields after a script changes the model; published positions/UVs stay fixed.
inline std::vector<float> ropeDrawVertices (
    const GeometryPacket& packet, const glm::vec3& eyeModel,
    const glm::vec3& fixedEyeDirection, bool screenOrientation) {
    auto vertices = packet.vertices;
    constexpr size_t stride = 37;
    for (size_t segment = 0; segment < packet.ropeSegments.size (); ++segment) {
        const auto& points = packet.ropeSegments[segment];
        const auto eyeVector = screenOrientation
            ? (points.start + points.end) * 0.5f - eyeModel : fixedEyeDirection;
        const float eyeSquared = glm::dot (eyeVector, eyeVector);
        const auto eye = screenOrientation && std::isfinite (eyeSquared) && eyeSquared > 0.0f
            ? eyeVector / std::sqrt (eyeSquared) : eyeVector;
        const auto start = ropeReflectedSizedRight (eye, points.end - points.previous, points.startSize);
        const auto end = ropeReflectedSizedRight (eye, points.next - points.start, points.endSize);
        for (uint32_t subdivision = 0; subdivision < packet.subdivision; ++subdivision) {
            const auto rightStart = ropeInterpolatedRight (start, end,
                static_cast<float> (subdivision) / packet.subdivision);
            const auto rightEnd = ropeInterpolatedRight (start, end,
                static_cast<float> (subdivision + 1) / packet.subdivision);
            for (size_t corner = 0; corner < 4; ++corner) {
                const size_t base = ((segment * packet.subdivision + subdivision) * 4 + corner) * stride;
                for (size_t axis = 0; axis < 3; ++axis) {
                    vertices[base + 28 + axis] = rightStart[axis];
                    vertices[base + 31 + axis] = rightEnd[axis];
                    vertices[base + 34 + axis] = eye[axis];
                }
            }
        }
    }
    return vertices;
}

// Native warmup temporarily clears spawn/follow descriptor dispatch bits.
// Death processing and emitter/initializer execution remain inner-tick work.
class SuppressBirthEvents {
public:
    explicit SuppressBirthEvents (bool& enabled) : m_enabled (enabled), m_previous (enabled) {
        m_enabled = false;
    }
    ~SuppressBirthEvents () { m_enabled = m_previous; }
    SuppressBirthEvents (const SuppressBirthEvents&) = delete;
    SuppressBirthEvents& operator= (const SuppressBirthEvents&) = delete;
private:
    bool& m_enabled;
    bool m_previous;
};

enum class NodeAdvanceMode { Outer, Warmup };

template <typename Inner, typename Publish, typename Children>
void advanceNodeStages (NodeAdvanceMode mode, bool& birthEvents,
                        Inner&& inner, Publish&& publish, Children&& children) {
    if (mode == NodeAdvanceMode::Warmup) {
        SuppressBirthEvents suppress (birthEvents);
        inner ();
        return;
    }
    inner ();
    publish ();
    children ();
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
