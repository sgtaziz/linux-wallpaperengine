#include "PropertyAnimation.h"

#include <algorithm>
#include <cmath>

using namespace WallpaperEngine::Data::Model;

namespace {
float bezier (float a, float b, float c, float d, float t) {
    const float u = 1.0f - t;
    return u*u*u*a + 3.0f*u*u*t*b + 3.0f*u*t*t*c + t*t*t*d;
}
}

void PropertyAnimation::play () {
    if (finished) time = 0.0f;
    paused = false;
    finished = false;
}

void PropertyAnimation::stop () {
    // Native 140170830 sets pause, then clears both pause and finished bits.
    paused = false;
    finished = false;
    time = 0.0f;
}

bool PropertyAnimation::setFrame (float value) {
    if (!std::isfinite (value) || !(fps > 0.0f) || !std::isfinite (fps)) return false;
    const float nextTime = value / fps;
    if (!std::isfinite (nextTime) || !std::isfinite (nextTime * fps)) return false;
    time = nextTime;
    return true;
}

void PropertyAnimation::advance (float seconds) {
    if (!isPlaying () || !(duration () > 0.0f) || !std::isfinite (duration ()) ||
        !std::isfinite (seconds) || !std::isfinite (rate)) return;
    const float step = seconds * rate * (reverse ? -1.0f : 1.0f);
    if (!std::isfinite (step) || !std::isfinite (time + step)) return;
    time += step;
    const float end = duration ();
    if (mode == Mode::Single) {
        if (time >= end) { time = end; finished = true; }
        if (time < 0.0f) { time = 0.0f; finished = true; }
    } else if (mode == Mode::Mirror) {
        if (time >= end) { time = end - std::fmod (time, end); reverse = true; }
        else if (time < 0.0f) { time = -std::fmod (time, end); reverse = false; }
    } else {
        time = std::fmod (time, end);
        if (time < 0.0f) time += end;
    }
}

float PropertyAnimation::sampleFrame (size_t channel, int atFrame) const {
    if (channel >= channels.size ()) return 0.0f;
    const auto& keys = channels[channel];
    if (keys.empty ()) return 0.0f;
    if (atFrame <= keys.front ().frame) return keys.front ().value;
    if (atFrame >= keys.back ().frame) return keys.back ().value;
    const auto after = std::upper_bound (keys.begin (), keys.end (), atFrame,
        [] (int frame, const Key& key) { return frame < key.frame; });
    const Key& right = *after;
    const Key& left = *(after - 1);
    if (right.constant) return left.value;
    const float halfSpan = (right.frame - left.frame) * 0.5f;
    const float x0 = static_cast<float> (left.frame);
    const float x1 = x0 + halfSpan * (left.front.enabled ? left.front.x : 0.0f);
    const float x3 = static_cast<float> (right.frame);
    const float x2 = x3 + halfSpan * (right.back.enabled ? right.back.x : 0.0f);
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 32; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (bezier (x0, x1, x2, x3, mid) < atFrame) lo = mid;
        else hi = mid;
    }
    const float t = (lo + hi) * 0.5f;
    const float y1 = left.value + (left.front.enabled ? left.front.y : 0.0f);
    const float y2 = right.value + (right.back.enabled ? right.back.y : 0.0f);
    return bezier (left.value, y1, y2, right.value, t);
}

float PropertyAnimation::sample (size_t channel) const {
    if (fps <= 0.0f || length <= 0 || !std::isfinite (fps) || !std::isfinite (time)) return 0.0f;
    // Native 140170580 uses the stored frame period for both frame division
    // and fmod fraction; multiplying by fps changes the single-mode endpoint.
    const float period = 1.0f / fps;
    const float framePosition = time / period;
    // Branch before float-to-int conversion so script-set extreme but finite
    // frames cannot invoke an out-of-range conversion.
    const int first = framePosition <= 0.0f ? 0 :
        framePosition >= static_cast<float> (length - 1) ? length - 1 :
        static_cast<int> (framePosition);
    const int second = std::min (first + 1, length);
    float fraction = std::fmod (time, period) / period;
    if (!std::isfinite (fraction)) fraction = 0.0f;
    fraction = std::clamp (fraction, 0.0f, 1.0f);
    return sampleFrame (channel, first) * (1.0f - fraction)
         + sampleFrame (channel, second) * fraction;
}
