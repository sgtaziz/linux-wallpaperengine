#include "PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Logging/Log.h"
#include <algorithm>
#include <cstdint>
#include <exception>

namespace WallpaperEngine::Audio::Drivers::Recorders {
void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata);

namespace {
using PulseAudioData = PulseAudioPlaybackRecorder::PulseAudioData;

void requestServerInfo (pa_context* ctx, PulseAudioData& recorder) {
    if (recorder.queryPending || pa_context_get_state (ctx) != PA_CONTEXT_READY) return;
    recorder.refreshRequested = false;
    recorder.queryPending = true;
    pa_operation* operation = pa_context_get_server_info (ctx, &pa_server_info_cb, &recorder);
    if (operation) {
	pa_operation_unref (operation);
    } else {
	recorder.queryPending = false;
	recorder.refreshRequested = true;
	recorder.retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    }
}

void releaseCapture (PulseAudioData& recorder) {
    if (!recorder.captureStream) return;
    pa_stream_set_state_callback (recorder.captureStream, nullptr, nullptr);
    pa_stream_set_read_callback (recorder.captureStream, nullptr, nullptr);
    pa_stream_set_moved_callback (recorder.captureStream, nullptr, nullptr);
    pa_stream_set_suspended_callback (recorder.captureStream, nullptr, nullptr);
    pa_stream_disconnect (recorder.captureStream);
    pa_stream_unref (recorder.captureStream);
    recorder.captureStream = nullptr;
    recorder.monitorName.clear ();
    recorder.clearPublished = true;
}
} // namespace

void pa_stream_notify_cb (pa_stream* stream, void* userdata) {
    auto* recorder = static_cast<PulseAudioData*> (userdata);
    switch (pa_stream_get_state (stream)) {
	case PA_STREAM_FAILED:
	    sLog.error ("Capture stream failed; retrying the output monitor");
	    recorder->refreshRequested = true;
	    recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	    break;
	case PA_STREAM_TERMINATED:
	    recorder->refreshRequested = true;
	    recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	    break;
	case PA_STREAM_READY:
	    sLog.debug ("Capture stream ready");
	    // Pulse may negotiate a format different from the requested monitor
	    // format. Rebuild before any read callback feeds frames in that format.
	    if (const pa_sample_spec* actual = pa_stream_get_sample_spec (stream);
		!actual || !pa_sample_spec_valid (actual)
		|| actual->format != PA_SAMPLE_FLOAT32NE || actual->channels != 2) {
		sLog.error ("Capture stream has an unsupported negotiated format");
		recorder->refreshRequested = true;
		recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
		recorder->clearPublished = true;
	    } else if (actual->rate != recorder->spectrum.sampleRate ()) {
		try { recorder->spectrum.configureSampleRate (actual->rate); }
		catch (const std::exception& error) {
		    sLog.error ("Cannot configure capture spectrum for negotiated rate: ", error.what ());
		    recorder->refreshRequested = true;
		    recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
		    recorder->clearPublished = true;
		}
	    }
	    if (const char* actual = pa_stream_get_device_name (stream);
		actual && recorder->monitorName != actual) {
		recorder->refreshRequested = true;
	    }
	    break;
	default:
	    break;
    }
}

void pa_stream_moved_cb (pa_stream* stream, void* userdata) {
    auto* recorder = static_cast<PulseAudioData*> (userdata);
    if (const char* actual = pa_stream_get_device_name (stream);
	actual && recorder->monitorName != actual) {
	recorder->refreshRequested = true;
    }
}

void pa_stream_suspended_cb (pa_stream* stream, void* userdata) {
    if (pa_stream_is_suspended (stream) > 0) {
	auto* recorder = static_cast<PulseAudioData*> (userdata);
	recorder->spectrum.reset ();
	recorder->clearPublished = true;
    }
}

void pa_stream_read_cb (pa_stream* stream, const size_t /*nbytes*/, void* userdata) {
    auto* recorder = static_cast<PulseAudioData*> (userdata);
    const pa_sample_spec* actual = pa_stream_get_sample_spec (stream);
    const bool matchingRate = actual && pa_sample_spec_valid (actual)
	&& actual->format == PA_SAMPLE_FLOAT32NE && actual->channels == 2
	&& actual->rate == recorder->spectrum.sampleRate ();
    // Drain every queued fragment. A single read notification can cover
    // multiple memblocks; retaining older blocks delays the visualizer.
    for (;;) {
	const size_t available = pa_stream_readable_size (stream);
	if (available == 0 || available == static_cast<size_t> (-1)) break;
	const uint8_t* data = nullptr;
	size_t currentSize = 0;
	if (pa_stream_peek (stream, reinterpret_cast<const void**> (&data), &currentSize) != 0) {
	    sLog.error ("Failed to peek at stream data...");
	    break;
	}
	if (currentSize == 0) break;
	// A peek can stop in the middle of a stereo frame. The analyzer carries
	// every byte until a complete window exists.
	if (matchingRate) {
	    if (data) recorder->spectrum.feed (data, currentSize);
	    else recorder->spectrum.feedSilence (currentSize);
	}
	if (pa_stream_drop (stream) != 0) {
	    sLog.error ("Failed to drop data after peeking");
	    break;
	}
    }
}

void pa_sink_info_cb (pa_context* ctx, const pa_sink_info* info, int eol, void* userdata) {
    auto* recorder = static_cast<PulseAudioData*> (userdata);
    if (eol) {
	recorder->queryPending = false;
	if (eol < 0 || !recorder->sawSinkInfo) {
	    releaseCapture (*recorder);
	    recorder->refreshRequested = true;
	    recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	}
	return;
    }
    if (!info || !info->monitor_source_name || !*info->monitor_source_name) return;
    recorder->sawSinkInfo = true;

    const std::string monitorName (info->monitor_source_name);
    if (recorder->captureStream && recorder->monitorName == monitorName) {
	const auto state = pa_stream_get_state (recorder->captureStream);
	const char* actual = pa_stream_get_device_name (recorder->captureStream);
	const pa_sample_spec* streamSpec = pa_stream_get_sample_spec (recorder->captureStream);
	if (state == PA_STREAM_CREATING
	    || (state == PA_STREAM_READY && (!actual || monitorName == actual)
		&& streamSpec && pa_sample_spec_valid (streamSpec)
		&& streamSpec->format == PA_SAMPLE_FLOAT32NE && streamSpec->channels == 2
		&& streamSpec->rate == recorder->spectrum.sampleRate ())) return;
    }

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_FLOAT32NE;
    // Request the selected sink monitor's device rate; Pulse otherwise
    // resamples a 48 kHz monitor to the former fixed 44.1 kHz request.
    spec.rate = info->sample_spec.rate != 0 ? info->sample_spec.rate : 44100;
    spec.channels = 2;

    releaseCapture (*recorder);
    try { recorder->spectrum.configureSampleRate (spec.rate); }
    catch (const std::exception& error) {
	sLog.error ("Cannot configure output monitor spectrum: ", error.what ());
	recorder->refreshRequested = true;
	recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	return;
    }

    pa_stream* stream = pa_stream_new (ctx, "output monitor", &spec, nullptr);
    if (!stream) {
	sLog.error ("Failed to create output monitor capture stream");
	recorder->refreshRequested = true;
	recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	return;
    }

    pa_stream_set_state_callback (stream, &pa_stream_notify_cb, userdata);
    pa_stream_set_read_callback (stream, &pa_stream_read_cb, userdata);
    pa_stream_set_moved_callback (stream, &pa_stream_moved_cb, userdata);
    pa_stream_set_suspended_callback (stream, &pa_stream_suspended_cb, userdata);

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t> (pa_bytes_per_second (&spec) * 750 / 1000);
    attr.tlength = static_cast<uint32_t> (-1);
    attr.prebuf = static_cast<uint32_t> (-1);
    attr.minreq = static_cast<uint32_t> (-1);
    attr.fragsize = static_cast<uint32_t> (pa_bytes_per_second (&spec) * 10 / 1000);

    // A monitor stream must not reconfigure the output device's latency.
    if (pa_stream_connect_record (stream, monitorName.c_str (), &attr, PA_STREAM_NOFLAGS) != 0) {
	sLog.error ("Failed to connect to input for recording");
	pa_stream_set_state_callback (stream, nullptr, nullptr);
	pa_stream_set_read_callback (stream, nullptr, nullptr);
	pa_stream_set_moved_callback (stream, nullptr, nullptr);
	pa_stream_set_suspended_callback (stream, nullptr, nullptr);
	pa_stream_unref (stream);
	recorder->refreshRequested = true;
	recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	return;
    }
    recorder->captureStream = stream;
    recorder->monitorName = monitorName;
    recorder->clearPublished = true;
}

void pa_server_info_cb (pa_context* ctx, const pa_server_info* info, void* userdata) {
    auto* recorder = static_cast<PulseAudioData*> (userdata);
    if (!info || !info->default_sink_name || !*info->default_sink_name) {
	releaseCapture (*recorder);
	recorder->queryPending = false;
	recorder->refreshRequested = true;
	recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
	return;
    }
    recorder->sawSinkInfo = false;
    pa_operation* operation = pa_context_get_sink_info_by_name (
	ctx, info->default_sink_name, &pa_sink_info_cb, userdata
    );
    if (operation) {
	pa_operation_unref (operation);
    } else {
	recorder->queryPending = false;
	recorder->refreshRequested = true;
	recorder->retryAfter = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    }
}

void pa_context_subscribe_cb (pa_context*, pa_subscription_event_type_t type, uint32_t, void* userdata) {
    if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER) {
	auto* recorder = static_cast<PulseAudioData*> (userdata);
	recorder->refreshRequested = true;
	recorder->retryAfter = {};
    }
}

void pa_context_notify_cb (pa_context* ctx, void* userdata) {
    switch (pa_context_get_state (ctx)) {
	case PA_CONTEXT_READY:
	    {
		// set callback
		pa_context_set_subscribe_callback (ctx, pa_context_subscribe_cb, userdata);
		// set events mask and enable event callback.
		pa_operation* o = pa_context_subscribe (ctx, PA_SUBSCRIPTION_MASK_SERVER, nullptr, nullptr);

		if (o) {
		    pa_operation_unref (o);
		}

		static_cast<PulseAudioData*> (userdata)->refreshRequested = true;

		break;
	    }
	case PA_CONTEXT_FAILED:
	    sLog.error ("PulseAudio context initialization failed. Audio processing is disabled");
	    break;
	default:
	    break;
    }
}

PulseAudioPlaybackRecorder::PulseAudioPlaybackRecorder () : m_captureData {} {
    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine-audioprocessing");

    pa_context_set_state_callback (this->m_context, &pa_context_notify_cb, &this->m_captureData);

    if (pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
	sLog.error ("PulseAudio connection failed! Audio processing is disabled");
	return;
    }

    // wait until the context is ready
    while (pa_context_get_state (this->m_context) != PA_CONTEXT_READY) {
	const auto state = pa_context_get_state (this->m_context);
	if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED
	    || pa_mainloop_iterate (this->m_mainloop, 1, nullptr) < 0) {
	    sLog.error ("PulseAudio capture setup failed; audio processing is disabled");
	    return;
	}
    }
}

PulseAudioPlaybackRecorder::~PulseAudioPlaybackRecorder () {
    releaseCapture (m_captureData);

    pa_context_disconnect (this->m_context);
    pa_context_unref (this->m_context);
    pa_mainloop_free (this->m_mainloop);
}

void PulseAudioPlaybackRecorder::update () {
    // Capture may produce several fragments per rendered frame. One event
    // dispatch per frame lets unread audio lag increasingly behind playback.
    for (int dispatch = 0; dispatch < 64; ++dispatch) {
	if (pa_mainloop_iterate (this->m_mainloop, 0, nullptr) <= 0) break;
    }
    if (m_captureData.refreshRequested && !m_captureData.queryPending
	&& std::chrono::steady_clock::now () >= m_captureData.retryAfter) {
	requestServerInfo (m_context, m_captureData);
    }
    if (m_captureData.clearPublished) {
	m_destination = {};
	m_published = {};
	m_captureData.clearPublished = false;
    }
    if (m_captureData.captureStream
	&& pa_stream_get_state (m_captureData.captureStream) == PA_STREAM_READY
	&& pa_stream_is_suspended (m_captureData.captureStream) == 0) {
	(void) m_captureData.spectrum.take (m_destination);
    } else {
	m_destination = {};
    }

    std::copy (m_destination.audio64[0].begin (), m_destination.audio64[0].end (), this->audio64RawLeft);
    std::copy (m_destination.audio64[1].begin (), m_destination.audio64[1].end (), this->audio64RawRight);

    StereoSpectrum::advancePublished (m_published, m_destination, 0.3f);
    for (size_t i = 0; i < 64; ++i) {
	this->audio64Left[i] = m_published.audio64[0][i];
	this->audio64Right[i] = m_published.audio64[1][i];
	this->audio64[i] = m_published.combined64[i];
    }
    for (size_t i = 0; i < 32; ++i) {
	this->audio32Left[i] = m_published.audio32[0][i];
	this->audio32Right[i] = m_published.audio32[1][i];
	this->audio32[i] = m_published.combined32[i];
    }
    for (size_t i = 0; i < 16; ++i) {
	this->audio16Left[i] = m_published.audio16[0][i];
	this->audio16Right[i] = m_published.audio16[1][i];
	this->audio16[i] = m_published.combined16[i];
    }
}

} // namespace WallpaperEngine::Audio::Drivers::Recorders
