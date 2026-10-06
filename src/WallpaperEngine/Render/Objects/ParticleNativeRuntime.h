#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace WallpaperEngine::Render::Objects::ParticleCore {

#if defined(__SSE__)
inline constexpr bool nativeRuntimeArithmeticAvailable = true;
#else
inline constexpr bool nativeRuntimeArithmeticAvailable = false;
#endif

// The original interpreter uses RCPPS/RSQRTPS, including their IEEE zero and
// nonfinite behavior. Keep this route separate from accepted compact kernels.
inline float nativeRuntimeReciprocal (float value) {
#if defined(__SSE__)
    return _mm_cvtss_f32 (_mm_rcp_ss (_mm_set_ss (value)));
#else
    throw std::invalid_argument ("Native physical particle arithmetic requires SSE reciprocal support");
#endif
}

inline float nativeRuntimeReciprocalSqrt (float value) {
#if defined(__SSE__)
    return _mm_cvtss_f32 (_mm_rsqrt_ss (_mm_set_ss (value)));
#else
    throw std::invalid_argument ("Native physical particle arithmetic requires SSE reciprocal support");
#endif
}

inline float nativeRuntimeMinimum (float first, float second) {
    // MINPS selects its second operand for unordered/equal values.
    return first < second ? first : second;
}

inline float nativeRuntimeMaximum (float first, float second) {
    return first > second ? first : second;
}

inline float nativeRuntimeClamp01 (float value) {
    return nativeRuntimeMaximum (0.0f, nativeRuntimeMinimum (value, 1.0f));
}

inline float nativeRuntimeLifetimeFraction (float age, float lifetime) {
    return age * nativeRuntimeReciprocal (lifetime);
}

// opcode 3 (alphafade): masks and ramps use the RCPPS lifetime
// fraction; the factor is the bitwise OR of the masked fade-in ramp, the
// masked fade-out ramp and 1.0 when neither mask holds. With fadein > fadeout
// both masks hold in fadeout < life < fadein and the ramps' bits merge, so the
// result depends on this CPU's RCPPS approximation exactly as native does.
inline float nativeRuntimeAlphaFade (float age, float lifetime, float fadeIn, float fadeOut) {
    const float life = nativeRuntimeLifetimeFraction (age, lifetime);
    const bool fadingIn = life < fadeIn;
    const bool fadingOut = fadeOut < life;
    const uint32_t in = fadingIn ? std::bit_cast<uint32_t> (life * nativeRuntimeReciprocal (fadeIn)) : 0u;
    const uint32_t out = fadingOut ? std::bit_cast<uint32_t> ((1.0f - life) * nativeRuntimeReciprocal (1.0f - fadeOut)) : 0u;
    const uint32_t neither = fadingIn || fadingOut ? 0u : std::bit_cast<uint32_t> (1.0f);
    return std::bit_cast<float> (in | out | neither);
}

inline float nativeRuntimeSquaredLength (float x, float y, float z) {
    return (y * y + x * x) + z * z;
}

inline float nativeRuntimeLength (float x, float y, float z) {
    return std::sqrt (nativeRuntimeSquaredLength (x, y, z));
}

} // namespace WallpaperEngine::Render::Objects::ParticleCore
