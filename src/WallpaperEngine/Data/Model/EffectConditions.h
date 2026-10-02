#pragma once

#include "Types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Model {
enum class EffectConditionComparison { Equal, GreaterEqual, Greater, LessEqual, Less };
struct EffectCondition {
    std::string combo;
    int32_t value = 0;
    EffectConditionComparison comparison = EffectConditionComparison::Equal;
};
using EffectConditions = std::vector<EffectCondition>;

// Original ANDs every key of every object in the conditions array.
// Missing or nonnumeric instance combos read as zero, independently of the
// material's compiled shader defaults.
inline bool effectConditionsMatch (const EffectConditions& conditions, const ComboMap& combos) {
    for (const auto& condition : conditions) {
        const auto found = combos.find (condition.combo);
        const int32_t actual = found == combos.end () ? 0 : found->second;
        bool matches = false;
        switch (condition.comparison) {
        case EffectConditionComparison::Equal: matches = actual == condition.value; break;
        case EffectConditionComparison::GreaterEqual: matches = actual >= condition.value; break;
        case EffectConditionComparison::Greater: matches = actual > condition.value; break;
        case EffectConditionComparison::LessEqual: matches = actual <= condition.value; break;
        case EffectConditionComparison::Less: matches = actual < condition.value; break;
        }
        if (!matches) return false;
    }
    return true;
}
}
