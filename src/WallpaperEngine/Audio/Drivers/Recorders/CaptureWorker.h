#pragma once

#include "StereoSpectrum.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <exception>
#include <mutex>
#include <stop_token>
#include <thread>

namespace WallpaperEngine::Audio::Drivers::Recorders {

// wallpaper64 2.8.42 FUN_14006c280: floor first, then halve, clamp and truncate.
[[nodiscard]] inline std::chrono::milliseconds nativeCaptureDelay (int framesPerSecond) {
    const float interval = std::floor (1000.0f / static_cast<float> (framesPerSecond)) * 0.5f;
    return std::chrono::milliseconds (static_cast<int> (std::clamp (interval, 6.0f, 100.0f)));
}

// A poll drains whole packets, retaining an incomplete prefix between polls.
// Native flag 2 requests zero publication; a later normal packet clears it.
class CapturePollState {
public:
    void begin () { m_hadPacket = m_silent = m_failed = false; }
    void packet (bool silent) { m_hadPacket = true; m_silent = silent; }
    void failed () { m_failed = true; }
    [[nodiscard]] bool finish (std::chrono::milliseconds delay) {
        bool idleReset = false;
        if (m_hadPacket) m_idleMilliseconds = 0;
        else if (m_idleMilliseconds > 1000) idleReset = true;
        else m_idleMilliseconds += delay.count ();
        return m_failed || m_silent || idleReset;
    }
    void reset () { m_idleMilliseconds = 0; begin (); }
private:
    int64_t m_idleMilliseconds = 0;
    bool m_hadPacket = false;
    bool m_silent = false;
    bool m_failed = false;
};

// The wait starts after each poll's work. There is no accumulated deadline or
// burst of catch-up polls when capture, FFT, or the scheduler takes longer.
template <typename Poll, typename Wait>
void runCapturePolling (std::stop_token stop, std::chrono::milliseconds delay, Poll&& poll, Wait&& wait) {
    while (!stop.stop_requested ()) {
        poll ();
        wait (stop, delay);
    }
}

/** Single capture owner and a complete, locked handoff to render-frame callers. */
class CaptureWorker {
public:
    struct Snapshot {
        StereoSpectrum::Bands bands {};
        uint64_t resetGeneration = 0;
    };
    explicit CaptureWorker (int framesPerSecond) : m_delay (nativeCaptureDelay (framesPerSecond)) {}
    ~CaptureWorker () { stop (); }
    CaptureWorker (const CaptureWorker&) = delete;
    CaptureWorker& operator= (const CaptureWorker&) = delete;

    // The owner supplies exception reporting; cleanup always runs on the owner
    // thread, including initialization failures and stop during a callback.
    void start (std::function<void (std::stop_token)> initialize, std::function<void ()> poll,
                std::function<void ()> cleanup, std::function<void (std::exception_ptr)> failed) {
        m_thread = std::jthread ([this, initialize, poll, cleanup, failed] (std::stop_token stop) {
            try {
                initialize (stop);
                runCapturePolling (stop, m_delay, poll, [this] (std::stop_token token, auto delay) {
                    std::unique_lock lock (m_waitMutex);
                    m_wait.wait_for (lock, token, delay, [] { return false; });
                });
            } catch (...) { failed (std::current_exception ()); }
            cleanup ();
        });
    }
    void stop () {
        if (!m_thread.joinable ()) return;
        m_thread.request_stop ();
        m_thread.join ();
    }
    void publish (const StereoSpectrum::Bands& bands, bool reset = false) {
        std::lock_guard lock (m_snapshotMutex);
        m_snapshot.bands = bands;
        if (reset) ++m_snapshot.resetGeneration;
    }
    [[nodiscard]] Snapshot snapshot () const {
        std::lock_guard lock (m_snapshotMutex);
        return m_snapshot;
    }
    [[nodiscard]] std::chrono::milliseconds delay () const { return m_delay; }
private:
    std::chrono::milliseconds m_delay;
    mutable std::mutex m_snapshotMutex;
    Snapshot m_snapshot;
    std::mutex m_waitMutex;
    std::condition_variable_any m_wait;
    std::jthread m_thread;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
