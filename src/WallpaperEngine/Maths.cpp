#include "Maths.h"

using namespace WallpaperEngine::Maths;

float WallpaperEngine::Maths::randomFloat (std::mt19937& rng, float min, float max) {
    // Native particle range helpers consume one MT word even for equal bounds.
    // Their top-24-bit conversion also preserves reversed authored ranges.
    const float top24 = static_cast<float> (rng () >> 8);
    return ((max - min) * top24) * (1.0f / 16777216.0f) + min;
}

glm::vec3 WallpaperEngine::Maths::randomVec3 (std::mt19937& rng, const glm::vec3& min, const glm::vec3& max) {
    const float x = randomFloat (rng, min.x, max.x);
    const float y = randomFloat (rng, min.y, max.y);
    const float z = randomFloat (rng, min.z, max.z);
    return { x, y, z };
}

// Helper: Linear interpolation
float WallpaperEngine::Maths::lerp (float t, float a, float b) { return a + t * (b - a); }

// Helper: Fade value change over lifetime
float WallpaperEngine::Maths::fadeValue (float life, float startTime, float endTime, float startValue, float endValue) {
    if (life <= startTime) {
	return startValue;
    } else if (life >= endTime) {
	return endValue;
    } else {
	float t = (life - startTime) / (endTime - startTime);
	return lerp (t, startValue, endValue);
    }
}
