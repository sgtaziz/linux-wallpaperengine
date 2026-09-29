#pragma once

#include "PlaybackRecorder.h"
#include "StereoSpectrum.h"
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
	pa_stream* captureStream = nullptr;
	std::string monitorName;
	bool refreshRequested = true;
	bool queryPending = false;
	bool sawSinkInfo = false;
	bool clearPublished = false;
	std::chrono::steady_clock::time_point retryAfter {};
    };

    PulseAudioPlaybackRecorder ();
    ~PulseAudioPlaybackRecorder () override;

    void update () override;

private:
    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    PulseAudioData m_captureData;

    StereoSpectrum::Bands m_destination;
    StereoSpectrum::Bands m_published;
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
