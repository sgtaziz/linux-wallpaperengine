#pragma once

#include "WallpaperEngine/Data/Model/Object.h"
#include <utility>
#include <vector>

namespace WallpaperEngine::Render::Objects::ParticleCore {

// Native 1401d15a0 opcode4: mulss, addss, maxss, divss. The authored
// count is a factory literal; only the root instance factor is read live.
inline float instanceSequenceStep (float authoredCount, float instanceCount) {
    // Keep the product rounded before subtraction (the native path is not FMA).
    volatile float product = instanceCount * authoredCount;
    const float denominator = product - 1.0f;
    // MAXSS selects its second operand for NaN as well as values below epsilon.
    return 1.0f / (denominator > 0.0001f ? denominator : 0.0001f);
}

struct InstanceSequencePatch {
    float authoredCount { 1.0f };
    bool enabled { false };

    void apply (float& step, float instanceCount) const {
        if (enabled) step = instanceSequenceStep (authoredCount, instanceCount);
    }
};

// Root instance writes are batched independently of initializer phase.
// One outer root tick clears the dirty byte before traversing its patch streams.
// The native descriptor callback marks writes, including same-valued writes.
class InstancePatchWrites {
public:
    explicit InstancePatchWrites (const Data::Model::ParticleInstanceOverride& instance) {
        const auto observe = [this] (const Data::Model::UserSettingUniquePtr& setting) {
            if (setting && setting->value)
                m_disconnect.push_back (setting->value->listen (
                    [this] (const auto&, auto) { mark (); }));
        };
        observe (instance.alpha);
        observe (instance.brightness);
        observe (instance.size);
        observe (instance.count);
        observe (instance.speed);
        observe (instance.lifetime);
        observe (instance.colorn);
        for (const auto& setting : instance.controlPoints) observe (setting);
        for (const auto& setting : instance.controlPointAngles) observe (setting);
        // Rate has no native 14022ab30 callback; neither does enabled.
    }
    ~InstancePatchWrites () { for (const auto& disconnect : m_disconnect) disconnect (); }
    InstancePatchWrites (const InstancePatchWrites&) = delete;
    InstancePatchWrites& operator= (const InstancePatchWrites&) = delete;
    void mark () { m_dirty = true; }
    bool consume () { return std::exchange (m_dirty, false); }

private:
    bool m_dirty { false };
    std::vector<std::function<void ()>> m_disconnect;
};

}
