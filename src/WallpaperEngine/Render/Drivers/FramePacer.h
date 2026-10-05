#pragma once

#include <chrono>
#include <cstdint>

namespace WallpaperEngine::Render::Drivers {
/**
 * Native frame limiter. Each second of timer ticks is
 * spent over `fps` frames: a frame sleeps its share of the second's remaining
 * budget minus the time since the previous frame woke, truncated to whole
 * milliseconds. The budget resets after fps frames or once overdrawn, so
 * frames average 1/fps but individual frames run a little short or long.
 *
 * Rope-trail history samples on a countdown without carry, so a
 * history interval that is an exact multiple of 1/fps lands on the following
 * frame whenever that frame window sums slightly short of the interval.
 */
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    explicit FramePacer (int fps) : m_fps (fps) { }

    /**
     * Called when the frame's work is done. Returns the milliseconds to sleep.
     */
    [[nodiscard]] int64_t sleepMilliseconds (Clock::time_point now) {
	if (m_fps <= 0) return 0;
	if (!m_started) {
	    m_started = true;
	    m_mark = now;
	}
	const int64_t work = std::chrono::duration_cast<std::chrono::nanoseconds> (now - m_mark).count ();
	m_pending = m_budget - work;
	if (m_pending < 0 || m_fps - 1 <= m_frame) {
	    m_pending = SECOND;
	    m_frame = 0;
	} else {
	    m_frame++;
	}
	const double share = static_cast<double> (m_pending) / static_cast<double> (m_fps - m_frame)
	    - static_cast<double> (work);
	// (DWORD)((float)(longlong)share / (float)(ticks per millisecond))
	return static_cast<int64_t> (static_cast<float> (static_cast<int64_t> (share)) / 1.0e6f);
    }

    /**
     * Called after the sleep returns, with the time the frame was done (as
     * passed to sleepMilliseconds) and the time it woke.
     */
    void woke (Clock::time_point done, Clock::time_point now) {
	if (m_fps <= 0) return;
	m_budget = m_pending - std::chrono::duration_cast<std::chrono::nanoseconds> (now - done).count ();
	m_mark = now;
    }

private:
    static constexpr int64_t SECOND = 1000000000;

    int m_fps;
    bool m_started = false;
    int m_frame = 0;
    int64_t m_budget = SECOND;
    int64_t m_pending = SECOND;
    Clock::time_point m_mark;
};
} // namespace WallpaperEngine::Render::Drivers
