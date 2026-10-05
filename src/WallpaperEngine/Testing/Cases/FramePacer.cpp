#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Render/Drivers/FramePacer.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"

#include <chrono>
#include <cstdint>
#include <vector>

using WallpaperEngine::Render::Drivers::FramePacer;
using namespace std::chrono_literals;

namespace {
// Runs the pacer with a fixed amount of work per frame and sleeps that last
// exactly as requested. Returns each frame's period in nanoseconds.
std::vector<int64_t> pace (int fps, std::chrono::nanoseconds work, int frames, std::vector<int64_t>* sleeps = nullptr) {
    FramePacer pacer (fps);
    auto now = FramePacer::Clock::time_point {} + 1s;
    auto previous = now;
    std::vector<int64_t> periods;
    for (int frame = 0; frame < frames; ++frame) {
	now += work;
	const int64_t milliseconds = pacer.sleepMilliseconds (now);
	if (sleeps) sleeps->push_back (milliseconds);
	const auto done = now;
	if (milliseconds > 0) now += std::chrono::milliseconds (milliseconds);
	pacer.woke (done, now);
	periods.push_back (std::chrono::duration_cast<std::chrono::nanoseconds> (now - previous).count ());
	previous = now;
    }
    return periods;
}
} // namespace

TEST_CASE ("Frame pacer spends each second's budget over fps frames like native", "[frame-pacer]") {
    // fps 4 without work: the first second only spans three frames (the
    // counter starts at zero and is incremented before the share), then every
    // four frames share one second exactly.
    std::vector<int64_t> sleeps;
    pace (4, 0ns, 11, &sleeps);
    REQUIRE (sleeps == std::vector<int64_t> {333, 333, 334, 250, 250, 250, 250, 250, 250, 250, 250});
}

TEST_CASE ("Frame pacer subtracts the frame's work and truncates to whole milliseconds", "[frame-pacer]") {
    std::vector<int64_t> sleeps;
    pace (30, 5ms, 3, &sleeps);
    // The first frame starts the schedule (no work yet): 1 s / 29 = 34.48 ms.
    // Then (1 s - 34 ms - 5 ms) / 28 - 5 ms = 29.32 ms and (961 ms - 29 ms - 5 ms) / 27 - 5 ms = 29.33 ms.
    REQUIRE (sleeps == std::vector<int64_t> {34, 29, 29});

    // Over many seconds the frames still average 1/fps.
    const auto periods = pace (30, 3ms, 30 * 20);
    int64_t total = 0;
    for (size_t frame = 30; frame < periods.size (); ++frame) total += periods[frame];
    const double average = static_cast<double> (total) / static_cast<double> (periods.size () - 30);
    REQUIRE (average > 33.0e6);
    REQUIRE (average < 33.7e6);
}

TEST_CASE ("Frame pacer skips the sleep when the frame overruns its budget", "[frame-pacer]") {
    std::vector<int64_t> sleeps;
    pace (30, 50ms, 4, &sleeps);
    // The first frame starts the schedule; every later frame does more work than its share.
    for (size_t frame = 1; frame < sleeps.size (); ++frame) REQUIRE (sleeps[frame] <= 0);
    REQUIRE (FramePacer (0).sleepMilliseconds (FramePacer::Clock::now ()) == 0);
}

TEST_CASE ("Rope trail history sampled under native pacing skips to the next frame", "[frame-pacer][particle]") {
    // resets the countdown to the interval without carry. With native
    // pacing, a three-frame window that sums just short of a 0.1 s interval
    // waits for a fourth frame; a limiter that keeps every frame at or above
    // 1/fps samples every third frame.
    const auto count = [] (const std::vector<int64_t>& periods) {
	WallpaperEngine::Render::Objects::ParticleCore::RopeTrailHistory history (1, 6);
	history.born (0, glm::vec3 (0.0f));
	float countdown = 0.1f;
	int frame = 0, last = 0, longer = 0, samples = 0;
	for (const int64_t period : periods) {
	    ++frame;
	    if (history.advance (static_cast<float> (period) * 1.0e-9f, 0.1f, countdown, 1,
	                         [] (uint32_t) { return glm::vec3 (0.0f); })) {
		if (last && frame - last > 3) ++longer;
		if (last) ++samples;
		last = frame;
	    }
	}
	return std::pair {samples, longer};
    };
    const auto [nativeSamples, nativeLonger] = count (pace (30, 2ms, 30 * 10));
    REQUIRE (nativeSamples > 0);
    REQUIRE (nativeLonger > 0);
    const std::vector<int64_t> minimumFrames (30 * 10, 33433333);   // old limiter: never shorter than 1/30
    const auto [oldSamples, oldLonger] = count (minimumFrames);
    REQUIRE (oldSamples > 0);
    REQUIRE (oldLonger == 0);
}
