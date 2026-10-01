#pragma once

#include "PlaybackRecorder.h"
#include "StereoSpectrum.h"
#include "CaptureWorker.h"
#include <pulse/pulseaudio.h>
#include <chrono>
#include <string>

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder;

class PulseAudioPlaybackRecorder final : public PlaybackRecorder {
public:
    /**
     * Struct that contains all the required data for the PulseAudio callbacks
     */
    struct PulseAudioData {
	StereoSpectrum spectrum;
	CapturePollState poll;
	std::stop_token stop;
	pa_stream* captureStream = nullptr;
	std::string monitorName;
	bool refreshRequested = true;
	bool queryPending = false;
	bool sawSinkInfo = false;
	bool clearPublished = false;
	std::chrono::steady_clock::time_point retryAfter {};
    };

    explicit PulseAudioPlaybackRecorder (int framesPerSecond);
    ~PulseAudioPlaybackRecorder () override;

    void update () override;

private:
    void initializeCapture ();
    void pollCapture ();
    void destroyCapture ();
    pa_mainloop* m_mainloop = nullptr;
    pa_context* m_context = nullptr;
    PulseAudioData m_captureData;
    CaptureWorker m_worker;
    uint64_t m_resetGeneration = 0;

    StereoSpectrum::Bands m_destination;
    StereoSpectrum::Bands m_published;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
