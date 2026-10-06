#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/ParticleAudioResponse.h"
#include "WallpaperEngine/Audio/SoundPlaybackPolicy.h"
#include "WallpaperEngine/Audio/SoundSpatialization.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/CaptureWorker.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PulseAudioPlaybackRecorder.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/NativeSpectrumMapping.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/NativeSpectrumSmoother.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/SceneSpectrumState.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/StereoSpectrum.h"
#include "WallpaperEngine/Audio/Drivers/SDLAudioDriver.h"
#include "WallpaperEngine/Data/JSON.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Render/Drivers/Detectors/FullScreenDetector.h"

#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <optional>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

void audio_callback (void* userdata, uint8_t* streamData, int length);

namespace {
void write16 (std::ofstream& output, const uint16_t value) {
    output.put (static_cast<char> (value & 0xff));
    output.put (static_cast<char> ((value >> 8) & 0xff));
}

void write32 (std::ofstream& output, const uint32_t value) {
    write16 (output, static_cast<uint16_t> (value & 0xffff));
    write16 (output, static_cast<uint16_t> (value >> 16));
}

struct TinyWav {
    std::filesystem::path path = std::filesystem::temp_directory_path ()
        / ("wallpaperengine-audio-lifecycle-" + std::to_string (SDL_GetTicks64 ()) + ".wav");

    explicit TinyWav (uint32_t samples = 4800, uint32_t sampleRate = 48000) {
	std::ofstream output (path, std::ios::binary);
        output.write ("RIFF", 4);
        write32 (output, 36 + samples * 2);
        output.write ("WAVEfmt ", 8);
        write32 (output, 16);
        write16 (output, 1);
        write16 (output, 1);
	write32 (output, sampleRate);
	write32 (output, sampleRate * 2);
        write16 (output, 2);
        write16 (output, 16);
        output.write ("data", 4);
        write32 (output, samples * 2);
        for (uint32_t i = 0; i < samples; ++i) {
            write16 (output, static_cast<uint16_t> ((i % 48 < 24) ? 12000 : -12000));
        }
    }

    ~TinyWav () { std::filesystem::remove (path); }
};

struct DummyAudioEnvironment {
    std::optional<std::string> previous;
    DummyAudioEnvironment () {
        if (const char* value = std::getenv ("SDL_AUDIODRIVER")) previous = value;
        SDL_setenv ("SDL_AUDIODRIVER", "dummy", 1);
    }
    ~DummyAudioEnvironment () {
        if (previous) SDL_setenv ("SDL_AUDIODRIVER", previous->c_str (), 1);
        else unsetenv ("SDL_AUDIODRIVER");
    }
};

/** 48 kHz PCM16 WAV with an arbitrary channel count and sample function. */
struct PcmWav {
    std::filesystem::path path;

    template <typename Sample>
    PcmWav (const std::string& name, uint16_t channels, uint32_t frames, Sample&& sample) :
        path (std::filesystem::temp_directory_path ()
              / ("wallpaperengine-sound-" + name + "-" + std::to_string (SDL_GetTicks64 ()) + ".wav")) {
        std::ofstream output (path, std::ios::binary);
        const uint32_t bytes = frames * channels * 2;
        output.write ("RIFF", 4);
        write32 (output, 36 + bytes);
        output.write ("WAVEfmt ", 8);
        write32 (output, 16);
        write16 (output, 1);
        write16 (output, channels);
        write32 (output, 48000);
        write32 (output, 48000 * channels * 2);
        write16 (output, channels * 2);
        write16 (output, 16);
        output.write ("data", 4);
        write32 (output, bytes);
        for (uint32_t frame = 0; frame < frames; ++frame)
            for (uint16_t channel = 0; channel < channels; ++channel)
                write16 (output, static_cast<uint16_t> (static_cast<int16_t> (sample (frame, channel))));
    }
    ~PcmWav () { std::filesystem::remove (path); }
};

/** An SDL dummy-device driver and context for decoder and mixer tests. */
struct AudioRig {
    DummyAudioEnvironment environment;
    char program[11] = "audio-test";
    char* argv[1] = { program };
    WallpaperEngine::Application::ApplicationContext application { 1, argv };
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen { application };
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector { application, fullscreen };
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver { application, detector, recorder };
    WallpaperEngine::Audio::AudioContext context { driver };

    explicit AudioRig (int masterVolume = 128) {
        application.state.general.keepRunning = true;
        application.state.audio.volume = masterVolume;
    }
};

/** Decodes interleaved float frames until @p samples values are available. */
std::vector<float> decodeSamples (WallpaperEngine::Audio::AudioStream& stream, size_t samples) {
    std::vector<float> output;
    std::array<uint8_t, 1024> chunk {};
    for (int attempt = 0; attempt < 5000 && output.size () < samples; ++attempt) {
        const int decoded = stream.decodeFrame (chunk.data (), static_cast<int> (chunk.size ()));
        if (decoded <= 0) {
            if (stream.isPlaybackFinished ()) break;
            SDL_Delay (1);
            continue;
        }
        const size_t count = static_cast<size_t> (decoded) / sizeof (float);
        const size_t start = output.size ();
        output.resize (start + count);
        std::memcpy (output.data () + start, chunk.data (), count * sizeof (float));
    }
    return output;
}

float peakOfChannel (const std::vector<float>& interleaved, size_t channel) {
    float peak = 0.0f;
    for (size_t index = channel; index < interleaved.size (); index += 2)
        peak = std::max (peak, std::abs (interleaved[index]));
    return peak;
}

/** Records the controller's decoder requests. */
struct FakeClipPlayer final : WallpaperEngine::Audio::SoundClipPlayer {
    struct Start {
        size_t index;
        bool repeat;
        float volume;
    };
    std::vector<Start> starts;
    bool active = false;
    bool finished = false;
    bool paused = false;
    float volume = -1.0f;
    std::optional<float> duration = 0.5f;

    bool failStarts = false;

    bool startClip (size_t index, bool repeat, float startVolume) override {
        starts.push_back ({ index, repeat, startVolume });
        if (failStarts) return false;
        active = true;
        finished = false;
        paused = false;
        volume = startVolume;
        return true;
    }
    void releaseClip () override { active = false; }
    [[nodiscard]] bool hasClip () const override { return active; }
    [[nodiscard]] bool clipFinished () const override { return finished; }
    void setClipPaused (bool clipPaused) override { paused = clipPaused; }
    void setClipVolume (float clipVolume) override { volume = clipVolume; }
    [[nodiscard]] std::optional<float> clipDuration () const override { return duration; }
};
} // namespace

TEST_CASE ("Audio callback releases a stream while a reader is active", "[audio][lifecycle]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    application.state.audio.volume = 128;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    TinyWav wav;

    auto* stream = new WallpaperEngine::Audio::AudioStream (context, wav.path.string ());
    REQUIRE (stream->isInitialized ());
    const int id = driver.addStream (stream);
    std::atomic_bool running { true };
    std::atomic_int callbackCount { 0 };
    std::atomic_bool heardPcm { false };
    std::thread callback ([&] {
        std::array<uint8_t, 4096> samples {};
        while (running.load ()) {
            audio_callback (&driver, samples.data (), static_cast<int> (samples.size ()));
            if (std::any_of (samples.begin (), samples.end (), [] (uint8_t byte) { return byte != 0; })) {
                heardPcm = true;
            }
            ++callbackCount;
        }
    });

    for (int i = 0; i < 100 && !heardPcm.load (); ++i) SDL_Delay (1);
    const bool decodedPcmBeforeRemoval = heardPcm.load ();
    const int callbacksBeforeRemoval = callbackCount.load ();
    stream->stop ();
    driver.removeStream (id);
    delete stream;
    SDL_Delay (10);
    running = false;
    callback.join ();
    REQUIRE (decodedPcmBeforeRemoval);
    REQUIRE (callbackCount.load () > callbacksBeforeRemoval);
    REQUIRE (driver.getStreams ().empty ());
}

TEST_CASE ("Empty audio packet queue does not block the callback", "[audio][lifecycle]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    application.state.audio.volume = 128;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    const AVCodec* codec = avcodec_find_decoder (AV_CODEC_ID_PCM_S16LE);
    REQUIRE (codec != nullptr);
    AVCodecContext* codecContext = avcodec_alloc_context3 (codec);
    REQUIRE (codecContext != nullptr);
    codecContext->sample_rate = 48000;
    codecContext->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default (&codecContext->ch_layout, 1);
    REQUIRE (codecContext->ch_layout.nb_channels == 1);
    REQUIRE (avcodec_open2 (codecContext, codec, nullptr) == 0);
    auto* stream = new WallpaperEngine::Audio::AudioStream (context, codecContext);
    stream->setVolume (0.5f);
    REQUIRE (stream->getMixerGain () == 0.25f);
    stream->setPaused (true);
    REQUIRE (stream->isPaused ());
    stream->setPaused (false);
    const auto started = std::chrono::steady_clock::now ();
    REQUIRE_FALSE (stream->dequeuePacket ());
    std::array<uint8_t, 4096> samples {};
    REQUIRE (stream->decodeFrame (samples.data (), static_cast<int> (samples.size ())) < 0);
    REQUIRE (std::chrono::steady_clock::now () - started < std::chrono::milliseconds (100));
    stream->stop ();
    delete stream;

    TinyWav wav;
    WallpaperEngine::Audio::AudioStream fileStream (context, wav.path.string ());
    REQUIRE (fileStream.isInitialized ());
    fileStream.stop (); // joins the reader, so the queue cannot refill after draining
    while (fileStream.dequeuePacket ()) {}
    const auto drainedAt = std::chrono::steady_clock::now ();
    REQUIRE_FALSE (fileStream.dequeuePacket ());
    SDL_Delay (5);
    REQUIRE_FALSE (fileStream.dequeuePacket ());
    REQUIRE (std::chrono::steady_clock::now () - drainedAt < std::chrono::milliseconds (100));
}

TEST_CASE ("Driver notices finite audio after a partial final mix", "[audio][lifecycle]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    application.state.audio.volume = 128;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    TinyWav wav (4801, 48000); // 38,408 output bytes, not divisible by the mix buffer
    auto stream = std::make_unique<WallpaperEngine::Audio::AudioStream> (context, wav.path.string ());
    const int id = driver.addStream (stream.get ());
    std::array<uint8_t, 4096> samples {};
    bool heardPcm = false;
    bool finished = false;
    for (int i = 0; i < 200 && !finished; ++i) {
	audio_callback (&driver, samples.data (), static_cast<int> (samples.size ()));
	heardPcm |= std::any_of (samples.begin (), samples.end (), [] (uint8_t byte) { return byte != 0; });
	finished = driver.isStreamFinished (id);
	if (!finished) SDL_Delay (1);
    }
    stream->stop ();
    driver.removeStream (id);
    stream.reset ();
    REQUIRE (heardPcm);
    REQUIRE (finished);
}

TEST_CASE ("Decoded PCM survives smaller output buffers", "[audio][decode]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    TinyWav wav (4410, 44100);
    WallpaperEngine::Audio::AudioStream stream (context, wav.path.string ());
    REQUIRE (stream.isInitialized ());

    std::array<uint8_t, 1024> samples {};
    size_t totalBytes = 0;
    bool heardPcm = false;
    bool reachedEnd = false;
    for (int i = 0; i < 200; ++i) {
	const int decoded = stream.decodeFrame (samples.data (), static_cast<int> (samples.size ()));
	if (decoded < 0) {
	    if (stream.hasReaderFinished () && stream.isQueueEmpty ()) {
		reachedEnd = true;
		break;
	    }
	    SDL_Delay (1);
	    continue;
	}
	REQUIRE (decoded <= static_cast<int> (samples.size ()));
	totalBytes += decoded;
	heardPcm |= std::any_of (samples.begin (), samples.begin () + decoded,
	                         [] (uint8_t byte) { return byte != 0; });
    }
    stream.stop ();
    REQUIRE (reachedEnd);
    REQUIRE (heardPcm);
    REQUIRE (totalBytes == 4800 * 2 * sizeof (float));
}

TEST_CASE ("Repeating PCM keeps its sample sequence across a decoder reset", "[audio][decode]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    TinyWav wav;
    WallpaperEngine::Audio::AudioStream stream (context, wav.path.string (), true);
    REQUIRE (stream.isInitialized ());

    constexpr size_t bytesPerCycle = 4800 * 2 * sizeof (float);
    std::vector<uint8_t> output;
    std::array<uint8_t, 1024> chunk {};
    for (int i = 0; i < 500 && output.size () < 2 * bytesPerCycle; ++i) {
	const int decoded = stream.decodeFrame (chunk.data (), static_cast<int> (chunk.size ()));
	if (decoded < 0) {
	    SDL_Delay (1);
	    continue;
	}
	output.insert (output.end (), chunk.begin (), chunk.begin () + decoded);
    }
    stream.stop ();
    REQUIRE (output.size () >= 2 * bytesPerCycle);
    REQUIRE (std::equal (output.begin (), output.begin () + bytesPerCycle,
	                output.begin () + bytesPerCycle));
}

TEST_CASE ("Repeating compressed audio drains and resets in queue order", "[audio][decode]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    const auto fixture = std::filesystem::path (__FILE__).parent_path ().parent_path () / "Fixtures/audio-loop-sine.mp3";
    WallpaperEngine::Audio::AudioStream stream (context, fixture.string (), true);
    REQUIRE (stream.isInitialized ());

    std::array<std::vector<uint8_t>, 3> cycles;
    std::array<uint8_t, 1024> chunk {};
    for (int i = 0; i < 1000 && stream.getCompletedLoopCount () < 2; ++i) {
	const int decoded = stream.decodeFrame (chunk.data (), static_cast<int> (chunk.size ()));
	if (decoded < 0) {
	    SDL_Delay (1);
	    continue;
	}
	const auto cycle = std::min<uint64_t> (stream.getCompletedLoopCount (), 2);
	cycles[cycle].insert (cycles[cycle].end (), chunk.begin (), chunk.begin () + decoded);
    }
    stream.stop ();
    REQUIRE (stream.getCompletedLoopCount () >= 2);
    REQUIRE (cycles[0].size () == 5760 * 2 * sizeof (float));
    REQUIRE (std::any_of (cycles[0].begin (), cycles[0].end (),
	                 [] (uint8_t byte) { return byte != 0; }));
    REQUIRE (cycles[0] == cycles[1]);
}

TEST_CASE ("Sound object retains authored playback settings", "[audio][parser]") {
    using WallpaperEngine::Data::JSON::parseAuthoringJson;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Sound;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    const auto data = parseAuthoringJson (R"({
      "id": 21, "name": "two clips", "sound": ["a.wav", "b.wav"],
      "playbackmode": "random", "volume": 0.4, "mintime": 1.25,
      "maxtime": 2.5, "startsilent": true, "spatialization": true,
      "attenuation": 0.75, "mindistance": 12.0
    })", "sound.json");
    const auto object = ObjectParser::parse (data, project);
    const auto* sound = dynamic_cast<const Sound*> (object.get ());
    REQUIRE (sound != nullptr);
    REQUIRE (sound->sounds == std::vector<std::string> { "a.wav", "b.wav" });
    REQUIRE (sound->playbackmode == "random");
    REQUIRE (sound->volume->value->getFloat () == 0.4f);
    REQUIRE (sound->minTime == 1.25f);
    REQUIRE (sound->maxTime == 2.5f);
    REQUIRE (sound->startSilent);
    REQUIRE (sound->spatialization);
    REQUIRE (sound->attenuation->value->getFloat () == 0.75f);
    REQUIRE (sound->minDistance->value->getFloat () == 12.0f);

    auto single = data;
    single["sound"] = "one.wav";
    const auto one = ObjectParser::parse (single, project);
    const auto* oneSound = dynamic_cast<const Sound*> (one.get ());
    REQUIRE (oneSound != nullptr);
    REQUIRE (oneSound->sounds == std::vector<std::string> { "one.wav" });
}

TEST_CASE ("Sound object defaults follow the native constructor", "[audio][parser]") {
    using WallpaperEngine::Data::JSON::parseAuthoringJson;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Sound;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    // Native: mintime 1, maxtime 5, mindistance 1, attenuation 1.
    const auto object = ObjectParser::parse (
        parseAuthoringJson (R"({ "id": 3, "name": "bare", "sound": ["a.wav"] })", "sound.json"), project);
    const auto* sound = dynamic_cast<const Sound*> (object.get ());
    REQUIRE (sound != nullptr);
    REQUIRE_FALSE (sound->playbackmode.has_value ());
    REQUIRE (sound->minTime == 1.0f);
    REQUIRE (sound->maxTime == 5.0f);
    REQUIRE (sound->minDistance->value->getFloat () == 1.0f);
    REQUIRE (sound->attenuation->value->getFloat () == 1.0f);
    REQUIRE_FALSE (sound->spatialization);
    REQUIRE_FALSE (sound->startSilent);
}

TEST_CASE ("Random sound schedule runs from clip start and gates play", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    using Action = Policy::Action;
    // c03: 0.5 s clip, fixed 1.0 s wait -> 1.5 s period from clip start.
    Policy random ("random", true, 1.0f, 1.0f, 1);
    REQUIRE (random.initialize (true) == Action::None);
    REQUIRE_FALSE (random.isPlaying ());
    REQUIRE (random.play (true) == Action::StartNew);
    random.clipStarted (0.5f, 0.3f);
    REQUIRE (random.timer () == 1.5f);
    REQUIRE (random.isPlaying ());
    REQUIRE (random.advance (0.4f, true) == Action::None);
    REQUIRE (random.play (true) == Action::None);
    REQUIRE (random.clipEnded (true) == Action::None);
    REQUIRE (random.advance (0.4f, true) == Action::None);
    // In the wait: play() is ignored and isPlaying stays true.
    REQUIRE (random.play (true) == Action::None);
    REQUIRE (random.isPlaying ());
    REQUIRE (random.advance (0.65f, true) == Action::None);
    REQUIRE (random.advance (0.1f, true) == Action::StartNew);

    // The interval is min + u * (max - min), added to the clip duration.
    Policy ranged ("random", false, 0.5f, 2.0f, 1);
    REQUIRE (ranged.initialize (true) == Action::StartNew);
    ranged.clipStarted (0.25f, 0.5f);
    REQUIRE (ranged.timer () == 1.5f);

    // c16: the timer is frozen while vol^2 is zero.
    Policy muted ("random", false, 1.0f, 1.0f, 1);
    REQUIRE (muted.initialize (true) == Action::StartNew);
    muted.clipStarted (0.5f, 0.0f);
    REQUIRE (muted.advance (0.3f, true) == Action::None);
    REQUIRE (muted.advance (5.0f, false) == Action::None);
    REQUIRE (std::abs (muted.timer () - 1.2f) < 1e-5f);
    REQUIRE (muted.isPlaying ());
    REQUIRE (muted.advance (1.2f, true) == Action::StartNew);

    // Paused and stopped layers report not playing.
    muted.clipStarted (0.5f, 0.0f);
    REQUIRE (muted.pause ());
    REQUIRE_FALSE (muted.isPlaying ());
    REQUIRE (muted.advance (10.0f, true) == Action::None);
    REQUIRE (muted.timer () == 1.5f);
    muted.stop ();
    REQUIRE_FALSE (muted.isPlaying ());
    REQUIRE (muted.timer () == 0.0f);
    REQUIRE (muted.play (true) == Action::StartNew);
}

TEST_CASE ("Random pause in the wait replays the clip without rescheduling", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    // c07: 2 s wait; pause at 1.2 s (after the clip ended), play at 2.0 s.
    FakeClipPlayer player;
    player.duration = 0.5f;
    WallpaperEngine::Audio::SoundLayerController layer (Policy ("random", true, 2.0f, 2.0f, 2), 2, player, 7);
    layer.initialize (1.0f);
    REQUIRE (player.starts.empty ());
    layer.play (1.0f);
    REQUIRE (player.starts.size () == 1);
    const size_t first = player.starts[0].index;
    layer.tick (0.6f, 1.0f);
    player.finished = true;
    layer.tick (0.3f, 1.0f);
    REQUIRE_FALSE (player.hasClip ());
    layer.pause ();
    const float frozen = layer.policy ().timer ();
    layer.tick (0.8f, 1.0f);
    REQUIRE (layer.policy ().timer () == frozen);
    layer.play (1.0f);
    REQUIRE (player.starts.size () == 2);
    REQUIRE (player.starts[1].index == first);
    REQUIRE (player.hasClip ());
    REQUIRE (layer.policy ().timer () == frozen);
    REQUIRE (layer.isPlaying ());
    // The original schedule continues from the remaining wait.
    layer.tick (frozen - 0.01f, 1.0f);
    REQUIRE (player.starts.size () == 2);
    layer.tick (0.02f, 1.0f);
    REQUIRE (player.starts.size () == 3);
}

TEST_CASE ("A failed replay of a paused voice leaves the layer stopped", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    FakeClipPlayer player;
    WallpaperEngine::Audio::SoundLayerController layer (Policy ("random", false, 1.0f, 1.0f, 2), 2, player, 9);
    layer.initialize (1.0f);
    REQUIRE (player.starts.size () == 1);
    const size_t first = player.starts[0].index;
    player.finished = true;
    layer.tick (0.6f, 1.0f);
    REQUIRE_FALSE (player.hasClip ());
    layer.pause ();
    // The finished clip's file can no longer be decoded when play() replays it.
    player.failStarts = true;
    layer.play (1.0f);
    REQUIRE (player.starts.size () == 2);
    REQUIRE (player.starts[1].index == first);
    REQUIRE_FALSE (player.hasClip ());
    REQUIRE_FALSE (layer.isPlaying ());
    REQUIRE (layer.policy ().stopped ());
    REQUIRE (layer.policy ().voice () == Policy::Voice::None);
    REQUIRE (layer.policy ().timer () == 0.0f);
    // A later play() starts the remaining usable clip.
    player.failStarts = false;
    layer.play (1.0f);
    REQUIRE (player.starts.size () == 3);
    REQUIRE (player.starts[2].index != first);
    REQUIRE (layer.isPlaying ());
}

TEST_CASE ("Loop and single play restart from the beginning", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    for (const char* mode : { "loop", "single" }) {
        CAPTURE (mode);
        FakeClipPlayer player;
        WallpaperEngine::Audio::SoundLayerController layer (Policy (mode, true, 1.0f, 5.0f, 1), 1, player, 3);
        layer.initialize (1.0f);
        layer.play (1.0f);
        REQUIRE (player.starts.size () == 1);
        layer.tick (0.5f, 1.0f);
        // c05: a repeated play() stops the voice and starts a new decoder.
        layer.play (1.0f);
        REQUIRE (player.starts.size () == 2);
        REQUIRE (player.active);
        REQUIRE (layer.isPlaying ());
    }

    // c06/c08: play() on a paused single resumes in place; after a natural
    // end, pause + play replays the same clip; the next play() repicks.
    FakeClipPlayer player;
    player.duration = 0.5f;
    WallpaperEngine::Audio::SoundLayerController single (Policy ("single", true, 1.0f, 5.0f, 2), 2, player, 11);
    single.initialize (1.0f);
    single.play (1.0f);
    single.tick (0.2f, 1.0f);
    single.pause ();
    REQUIRE (player.paused);
    single.play (1.0f);
    REQUIRE (player.starts.size () == 1);
    REQUIRE_FALSE (player.paused);
    single.tick (0.4f, 1.0f);
    REQUIRE_FALSE (single.isPlaying ());
    player.finished = true;
    single.tick (0.1f, 1.0f);
    single.pause ();
    single.play (1.0f);
    REQUIRE (player.starts.size () == 2);
    REQUIRE (player.starts[1].index == player.starts[0].index);
    single.play (1.0f);
    REQUIRE (player.starts.size () == 3);
}

TEST_CASE ("Single-file loops repeat inside the decoder", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    FakeClipPlayer one;
    WallpaperEngine::Audio::SoundLayerController loop (Policy ("loop", false, 1.0f, 5.0f, 1), 1, one, 5);
    loop.initialize (1.0f);
    REQUIRE (one.starts.size () == 1);
    REQUIRE (one.starts[0].repeat);
    for (int frame = 0; frame < 600; ++frame) loop.tick (1.0f / 30.0f, 1.0f);
    REQUIRE (one.starts.size () == 1);
    REQUIRE (loop.isPlaying ());

    // c02: several files stay EOF-driven and repick at each clip end.
    FakeClipPlayer two;
    WallpaperEngine::Audio::SoundLayerController pool (Policy ("loop", false, 1.0f, 5.0f, 2), 2, two, 5);
    pool.initialize (1.0f);
    REQUIRE (two.starts.size () == 1);
    REQUIRE_FALSE (two.starts[0].repeat);
    two.finished = true;
    pool.tick (1.0f / 30.0f, 1.0f);
    REQUIRE (two.starts.size () == 2);
    REQUIRE_FALSE (two.starts[1].repeat);
}

TEST_CASE ("Multi-file loops wait out the inaudible time before repicking", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    // A 3 s clip starts at frame 0 of a 30 fps scene. Native repicks when a
    // clip-length timer expires, and that timer only runs while audible
    // a muted voice keeps playing to EOF meanwhile.
    constexpr float frame = 1.0f / 30.0f;
    FakeClipPlayer player;
    player.duration = 3.0f;
    WallpaperEngine::Audio::SoundLayerController layer (Policy ("loop", false, 1.0f, 5.0f, 2), 2, player, 13);
    layer.initialize (1.0f);
    REQUIRE (player.starts.size () == 1);
    // Ticks frames [from, to] with the clip ending on endFrame; returns the
    // frame on which the next clip starts, or -1.
    const auto nextStart = [&] (int from, int to, int endFrame, const auto& volumeAt) {
        const size_t started = player.starts.size ();
        for (int index = from; index <= to; ++index) {
            if (index == endFrame) player.finished = true;
            layer.tick (frame, volumeAt (index));
            if (player.starts.size () != started) return index;
        }
        return -1;
    };

    SECTION ("Unmuted before EOF") {
        // Muted over 1-2 s: the clip still ends at 3 s, the next starts at 4 s.
        const int next = nextStart (1, 200, 90, [] (int index) { return index > 30 && index <= 60 ? 0.0f : 1.0f; });
        REQUIRE (next >= 119);
        REQUIRE (next <= 121);
    }

    SECTION ("Unmuted after EOF") {
        // Muted from 1 s, the clip ends at 3 s, unmuted at 7 s: the 2 s of
        // muted playback still run after the unmute, so the next starts at 9 s.
        const int next = nextStart (1, 400, 90, [] (int index) { return index > 30 && index <= 210 ? 0.0f : 1.0f; });
        REQUIRE (next >= 269);
        REQUIRE (next <= 271);
    }

    SECTION ("A pause holds the voice and accrues nothing") {
        // Paused over 1-2 s, the clip ends at 4 s and hands off on that update.
        int next = -1;
        for (int index = 1; index <= 200 && next < 0; ++index) {
            if (index == 31) layer.pause ();
            if (index == 61) layer.resume (1.0f);
            if (index == 120) player.finished = true;
            layer.tick (frame, 1.0f);
            if (player.starts.size () == 2) next = index;
        }
        REQUIRE (next == 120);
    }

    SECTION ("play() drops the carried wait") {
        REQUIRE (nextStart (1, 104, 90, [] (int index) { return index > 30 && index <= 60 ? 0.0f : 1.0f; }) == -1);
        layer.play (1.0f);
        REQUIRE (player.starts.size () == 2);
        REQUIRE (layer.policy ().timer () == 0.0f);
        // The replacement clip plays fully audible and hands off at its EOF.
        REQUIRE (nextStart (105, 300, 195, [] (int) { return 1.0f; }) == 195);
    }
}

TEST_CASE ("Silent sound layers start on a later volume raise", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    // c10: volume 0 at start, raised to 1 by a script at 2.2 s.
    for (const char* mode : { "loop", "random" }) {
        CAPTURE (mode);
        FakeClipPlayer player;
        WallpaperEngine::Audio::SoundLayerController layer (Policy (mode, false, 1.0f, 1.0f, 1), 1, player, 1);
        layer.initialize (0.0f);
        float now = 0.0f;
        for (; now < 2.2f; now += 1.0f / 30.0f) layer.tick (1.0f / 30.0f, 0.0f);
        REQUIRE (player.starts.empty ());
        layer.tick (1.0f / 30.0f, 1.0f);
        REQUIRE (player.starts.size () == 1);
        REQUIRE (player.starts[0].volume == 1.0f);
        layer.tick (1.0f / 30.0f, 1.0f);
        REQUIRE (player.starts.size () == 1);
    }

    // Single mode never starts on a volume change; play() does.
    FakeClipPlayer player;
    WallpaperEngine::Audio::SoundLayerController single (Policy ("single", false, 1.0f, 5.0f, 1), 1, player, 1);
    single.initialize (0.0f);
    single.tick (0.1f, 1.0f);
    single.tick (0.1f, 0.5f);
    REQUIRE (player.starts.empty ());
    single.play (0.5f);
    REQUIRE (player.starts.size () == 1);

    // play() at zero volume loads nothing but clears startsilent.
    FakeClipPlayer silent;
    WallpaperEngine::Audio::SoundLayerController quiet (Policy ("loop", true, 1.0f, 5.0f, 1), 1, silent, 1);
    quiet.initialize (0.0f);
    quiet.tick (0.1f, 1.0f);
    REQUIRE (silent.starts.empty ());
    quiet.tick (0.1f, 0.0f);
    quiet.play (0.0f);
    REQUIRE (silent.starts.empty ());
    REQUIRE_FALSE (quiet.policy ().startSilent ());
    quiet.tick (0.1f, 1.0f);
    REQUIRE (silent.starts.size () == 1);

    // A live voice is not restarted by later volume changes.
    quiet.tick (0.1f, 0.0f);
    quiet.tick (0.1f, 1.0f);
    REQUIRE (silent.starts.size () == 1);
}

TEST_CASE ("Sound decoder reports the clip duration", "[audio][decode]") {
    AudioRig rig;
    PcmWav wav ("duration", 2, 24000, [] (uint32_t, uint16_t) { return 0; });
    WallpaperEngine::Audio::AudioStream stream (rig.context, wav.path.string ());
    REQUIRE (stream.getDuration ().has_value ());
    REQUIRE (std::abs (*stream.getDuration () - 0.5f) < 1e-4f);
    stream.stop ();
}

TEST_CASE ("Looped clips wrap without inserted silence", "[audio][decode]") {
    AudioRig rig;
    // c01: an exact-period stereo sine; 100 periods of 480 Hz per 0.2 s.
    constexpr uint32_t frames = 9600;
    constexpr float amplitude = 0.25f;
    const auto sine = [] (uint32_t frame) {
        return std::sin (2.0 * std::numbers::pi * 480.0 * frame / 48000.0);
    };
    PcmWav wav ("loop", 2, frames, [&] (uint32_t frame, uint16_t) {
        return static_cast<int> (std::lround (sine (frame) * amplitude * 32767.0));
    });
    WallpaperEngine::Audio::AudioStream stream (rig.context, wav.path.string (), true);
    REQUIRE (stream.isInitialized ());
    const auto samples = decodeSamples (stream, frames * 2 * 3);
    stream.stop ();
    REQUIRE (samples.size () >= frames * 2 * 3);
    REQUIRE (stream.getCompletedLoopCount () >= 2);
    float worst = 0.0f;
    for (size_t frame = 0; frame < frames * 3; ++frame) {
        const float expected = static_cast<float> (sine (frame % frames) * amplitude);
        worst = std::max ({ worst, std::abs (samples[frame * 2] - expected),
                            std::abs (samples[frame * 2 + 1] - expected) });
    }
    REQUIRE (worst < 1e-3f);
}

TEST_CASE ("Non-spatial mono clips play at the native routing level", "[audio][decode]") {
    AudioRig rig;
    // c12: mono = 0.5957 x the stereo per-channel level, no crossfeed.
    const auto tone = [] (uint32_t frame, uint16_t) {
        return static_cast<int> (std::lround (std::sin (2.0 * std::numbers::pi * frame / 48.0) * 16000.0));
    };
    PcmWav mono ("mono", 1, 9600, tone);
    PcmWav stereo ("stereo", 2, 9600, tone);
    PcmWav leftOnly ("left", 2, 9600, [&] (uint32_t frame, uint16_t channel) {
        return channel == 0 ? tone (frame, 0) : 0;
    });
    WallpaperEngine::Audio::AudioStream monoStream (rig.context, mono.path.string (), true);
    WallpaperEngine::Audio::AudioStream stereoStream (rig.context, stereo.path.string ());
    WallpaperEngine::Audio::AudioStream leftStream (rig.context, leftOnly.path.string ());
    REQUIRE (monoStream.isMonoSource ());
    REQUIRE_FALSE (stereoStream.isMonoSource ());
    // Two loops, so the routing must survive the decoder reset.
    const auto monoSamples = decodeSamples (monoStream, 9600 * 2 * 2);
    const auto stereoSamples = decodeSamples (stereoStream, 9600 * 2);
    const auto leftSamples = decodeSamples (leftStream, 9600 * 2);
    REQUIRE (monoSamples.size () >= 9600 * 2 * 2);
    const std::vector<float> secondLoop (monoSamples.begin () + 9600 * 2, monoSamples.begin () + 9600 * 4);
    const float reference = peakOfChannel (stereoSamples, 0);
    REQUIRE (reference > 0.4f);
    REQUIRE (std::abs (peakOfChannel (monoSamples, 0) / reference - 0.5957f) < 1e-3f);
    REQUIRE (std::abs (peakOfChannel (monoSamples, 1) / reference - 0.5957f) < 1e-3f);
    REQUIRE (std::abs (peakOfChannel (secondLoop, 0) / reference - 0.5957f) < 1e-3f);
    REQUIRE (std::abs (peakOfChannel (stereoSamples, 1) - reference) < 1e-4f);
    REQUIRE (peakOfChannel (leftSamples, 1) == 0.0f);

    // Spatial routing replaces the per-channel level of later output.
    monoStream.setMonoChannelGains (0.2f, 0.0f);
    const auto panned = decodeSamples (monoStream, 9600 * 2 * 2);
    const std::vector<float> pannedTail (panned.end () - 9600 * 2, panned.end ());
    REQUIRE (std::abs (peakOfChannel (pannedTail, 0) / reference - 0.2f) < 1e-3f);
    REQUIRE (peakOfChannel (pannedTail, 1) == 0.0f);
    monoStream.stop ();
    stereoStream.stop ();
    leftStream.stop ();
}

TEST_CASE ("Sound gain clamps after the master gain", "[audio][mixer]") {
    // c09: native AL_GAIN = min(1, vol^2 * G); Linux G is --volume / 128.
    const auto mixedPeak = [] (int master, float volume) {
        AudioRig rig (master);
        PcmWav wav ("gain", 2, 48000, [] (uint32_t frame, uint16_t) {
            return static_cast<int> (std::lround (std::sin (2.0 * std::numbers::pi * frame / 48.0) * 8000.0));
        });
        WallpaperEngine::Audio::AudioStream stream (rig.context, wav.path.string ());
        stream.setVolume (volume);
        const int id = rig.driver.addStream (&stream);
        std::array<float, 2048> mix {};
        float peak = 0.0f;
        for (int attempt = 0; attempt < 200 && peak == 0.0f; ++attempt) {
            audio_callback (&rig.driver, reinterpret_cast<uint8_t*> (mix.data ()), sizeof (mix));
            for (const float sample : mix) peak = std::max (peak, std::abs (sample));
            if (peak == 0.0f) SDL_Delay (1);
        }
        stream.stop ();
        rig.driver.removeStream (id);
        return peak;
    };
    REQUIRE (mixedPeak (128, 1.0f) > 0.2f);
    const float half = mixedPeak (64, 1.0f);
    REQUIRE (std::abs (mixedPeak (64, 2.0f) / half - 2.0f) < 0.02f);
    REQUIRE (std::abs (mixedPeak (128, 2.0f) / mixedPeak (128, 1.0f) - 1.0f) < 0.01f);
    REQUIRE (std::abs (mixedPeak (128, 0.5f) / mixedPeak (128, 1.0f) - 0.25f) < 0.01f);
}

TEST_CASE ("Sound mix volume stays defined for extreme gains", "[audio][mixer]") {
    using WallpaperEngine::Audio::Drivers::sdlMixVolume;
    constexpr float inf = std::numeric_limits<float>::infinity ();
    REQUIRE (sdlMixVolume (64, 1.0f) == 64);
    REQUIRE (sdlMixVolume (64, 4.0f) == SDL_MIX_MAXVOLUME);
    REQUIRE (sdlMixVolume (128, 0.25f) == 32);
    REQUIRE (sdlMixVolume (128, inf) == SDL_MIX_MAXVOLUME);
    // A zero master with an unbounded vol^2 is 0 * inf = NaN: silent, not UB.
    REQUIRE (sdlMixVolume (0, inf) == 0);
    REQUIRE (sdlMixVolume (128, std::numeric_limits<float>::quiet_NaN ()) == 0);
    REQUIRE (sdlMixVolume (0, 1.0f) == 0);

    AudioRig rig;
    PcmWav wav ("extreme", 2, 4800, [] (uint32_t, uint16_t) { return 1000; });
    WallpaperEngine::Audio::AudioStream stream (rig.context, wav.path.string ());
    stream.setVolume (std::numeric_limits<float>::max ());
    REQUIRE (std::isinf (stream.getMixerGain ()));
    REQUIRE (sdlMixVolume (0, stream.getMixerGain ()) == 0);
    REQUIRE (sdlMixVolume (15, stream.getMixerGain ()) == SDL_MIX_MAXVOLUME);
    stream.stop ();
}

TEST_CASE ("Spatialized mono gains follow the recovered native law", "[audio][spatial]") {
    using namespace WallpaperEngine::Audio;
    const auto db = [] (const MonoChannelGains& gains) { return 20.0f * std::log10 (gains.left / gains.right); };
    const auto amplitude = [] (const MonoChannelGains& gains) {
        return std::sqrt (gains.left * gains.left + gains.right * gains.right);
    };
    // c13/c14: 1280x720 orthographic scene, authored eye 0 0 0 looking at -z.
    const auto flat = soundListenerForCamera ({ 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, true, 1280, 720);
    REQUIRE (flat.position == glm::vec3 (640, 360, 2000));
    REQUIRE (std::abs (db (spatialMonoGains ({ 160, 360, 0 }, flat, 1, 1)) - 5.12f) < 0.02f);
    REQUIRE (std::abs (db (spatialMonoGains ({ 1120, 360, 0 }, flat, 1, 1)) + 5.12f) < 0.02f);
    REQUIRE (std::abs (db (spatialMonoGains ({ -480, 360, 0 }, flat, 1, 1)) - 11.68f) < 0.02f);
    const auto upper = spatialMonoGains ({ 640, 700, 0 }, flat, 1, 1);
    REQUIRE (std::abs (db (upper)) < 1e-4f);
    const auto reference = spatialMonoGains ({ -480, 360, 0 }, flat, 1, 1);
    const auto near = spatialMonoGains ({ -480, 360, 0 }, flat, 0.1f, 1);
    const auto steep = spatialMonoGains ({ -480, 360, 0 }, flat, 0.1f, 2);
    const auto raised = spatialMonoGains ({ -480, 360, 250 }, flat, 0.1f, 1);
    REQUIRE (std::abs (amplitude (near) / amplitude (reference) - 0.1f) < 1e-4f);
    REQUIRE (std::abs (amplitude (steep) / amplitude (reference) - 0.05263f) < 1e-4f);
    REQUIRE (std::abs (inverseDistanceClampedGain (0.5f, 0.1f, 1.0f) - 0.2f) < 1e-6f);
    REQUIRE (std::abs (db (raised) - 13.24f) < 0.02f);

    // c15: perspective scene, eye 0 0 10 looking at the origin; world units.
    const auto deep = soundListenerForCamera ({ 0, 0, 10 }, { 0, 0, 0 }, { 0, 1, 0 }, false, 1920, 1080);
    const auto p3 = spatialMonoGains ({ 0, 0, 8 }, deep, 1, 1);
    const auto p4 = spatialMonoGains ({ 0, 0, 6 }, deep, 1, 1);
    const auto p1 = spatialMonoGains ({ -5, 0, 10 }, deep, 1, 1);
    const auto p2 = spatialMonoGains ({ 5, 0, 10 }, deep, 1, 1);
    const auto p5 = spatialMonoGains ({ 0, 0, 12 }, deep, 1, 1);
    REQUIRE (std::abs (p4.left - 0.25f * (0.5f - 0.0957f)) < 1e-5f);
    REQUIRE (std::abs (amplitude (p4) / amplitude (p3) - 0.5f) < 1e-4f);
    REQUIRE (std::abs (p1.left - 0.2f) < 1e-5f);
    REQUIRE (std::abs (p1.right) < 1e-6f);
    REQUIRE (std::abs (p2.left) < 1e-6f);
    REQUIRE (std::abs (amplitude (p1) / amplitude (p3) - 0.700f) < 0.005f);
    REQUIRE (std::abs (amplitude (p5) / amplitude (p3) - 1.473f) < 0.005f);

    // The orthographic listener follows the scene camera eye.
    const auto moved = soundListenerForCamera ({ 100, -50, 3 }, { 100, -50, -1 }, { 0, 1, 0 }, true, 1280, 720);
    REQUIRE (moved.position == glm::vec3 (740, 310, 2000));
    // A source vector of zero length sits at the non-spatial default.
    const auto onTop = spatialMonoGains ({ 0, 0, 600 }, flat, 1, 1);
    REQUIRE (onTop.left == kNonSpatialMonoGains.left);
    REQUIRE (onTop.right == kNonSpatialMonoGains.right);
}

TEST_CASE ("Script writes to attenuation and mindistance apply at the next play", "[audio][spatial]") {
    using namespace WallpaperEngine::Audio;
    // ZR-2 on native 2.8.42: S1 at (-480, 360, 0) in the c14 scene, authored
    // mindistance 0.1 and attenuation 1. The script writes attenuation 4,
    // plays, writes mindistance 1, plays; the measured levels were 0.1, 0.1,
    // 0.0270, 0.0270 and 1.0 times the unattenuated clip.
    const auto flat = soundListenerForCamera ({ 0, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, true, 1280, 720);
    const auto level = [&] (const SoundDistanceLatch& latch) {
        const auto gains = spatialMonoGains ({ -480, 360, 0 }, flat, latch.minDistance, latch.attenuation);
        const auto open = spatialMonoGains ({ -480, 360, 0 }, flat, 1000.0f, 1.0f);
        return std::sqrt (gains.left * gains.left + gains.right * gains.right)
            / std::sqrt (open.left * open.left + open.right * open.right);
    };
    float attenuation = 1.0f;
    float minDistance = 0.1f;
    SoundDistanceLatch latch;
    latch.play (attenuation, minDistance);
    REQUIRE (std::abs (level (latch) - 0.1f) < 1e-3f);
    attenuation = 4.0f; // the write alone changes nothing
    REQUIRE (std::abs (level (latch) - 0.1f) < 1e-3f);
    latch.play (attenuation, minDistance);
    REQUIRE (std::abs (level (latch) - 0.02703f) < 1e-3f);
    minDistance = 1.0f;
    REQUIRE (std::abs (level (latch) - 0.02703f) < 1e-3f);
    latch.play (attenuation, minDistance);
    REQUIRE (std::abs (level (latch) - 1.0f) < 1e-3f);
}

TEST_CASE ("Clip selection skips missing and invalid files without retrying forever", "[audio][schedule]") {
    DummyAudioEnvironment audioEnvironment;
    char program[] = "audio-test";
    char* argv[] = { program };
    WallpaperEngine::Application::ApplicationContext application (1, argv);
    application.state.general.keepRunning = true;
    WallpaperEngine::Render::Drivers::Detectors::FullScreenDetector fullscreen (application);
    WallpaperEngine::Audio::Drivers::Detectors::AudioPlayingDetector detector (application, fullscreen);
    WallpaperEngine::Audio::Drivers::Recorders::PlaybackRecorder recorder;
    WallpaperEngine::Audio::Drivers::SDLAudioDriver driver (application, detector, recorder);
    WallpaperEngine::Audio::AudioContext context (driver);
    TinyWav wav;
    const auto missing = wav.path.string () + ".missing";
    const auto invalid = wav.path.string () + ".invalid";
    { std::ofstream output (invalid); output << "not an audio file"; }
    const std::array<std::string, 3> paths { missing, invalid, wav.path.string () };
    std::set<size_t> failed;
    std::mt19937 rng { 1 };
    size_t attempts = 0;
    auto tryStart = [&](size_t index) {
	++attempts;
	try {
	    WallpaperEngine::Audio::AudioStream candidate (context, paths[index]);
	    return candidate.isInitialized ();
	} catch (const std::exception&) {
	    return false;
	}
    };
    const auto invalidOnly = WallpaperEngine::Audio::startOneOf (2, failed, rng, tryStart);
    const auto reachedValid = WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart);
    const bool failedOnlyOnce = attempts == 3 && failed.contains (0) && failed.contains (1);
    const auto oldAttempts = attempts;
    const auto stillValid = WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart);
    std::filesystem::remove (invalid);
    REQUIRE_FALSE (invalidOnly.has_value ());
    REQUIRE (reachedValid == std::optional<size_t> { 2 });
    REQUIRE (failedOnlyOnce);
    REQUIRE (stillValid == std::optional<size_t> { 2 });
    REQUIRE (attempts == oldAttempts + 1);

    failed.insert (2);
    const auto beforeExhausted = attempts;
    REQUIRE_FALSE (WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart).has_value ());
    REQUIRE (attempts == beforeExhausted);
}

TEST_CASE ("Stereo spectrum keeps channels separate across partial windows and holes", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    StereoSpectrum analyzer;
    StereoSpectrum::Bands left {};
    StereoSpectrum::Bands right {};
    StereoSpectrum::Bands silence {};
    std::array<uint8_t, StereoSpectrum::WindowBytes> signal {};
    for (size_t sample = 0; sample < StereoSpectrum::Samples; ++sample) {
        const float phase = 2.0f * std::numbers::pi_v<float> * 1000.0f
            * static_cast<float> (sample) / 44100.0f;
        const float leftValue = 0.8f * std::sin (phase);
        std::memcpy (signal.data () + sample * 2 * sizeof (float), &leftValue, sizeof (float));
    }
    analyzer.feed (signal.data (), 777); // split inside one interleaved stereo frame
    REQUIRE_FALSE (analyzer.take (left));
    analyzer.feed (signal.data () + 777, signal.size () - 777);
    REQUIRE (analyzer.take (left));
    REQUIRE_FALSE (analyzer.take (left));

    const auto hasEnergy = [] (const auto& bands) {
        return std::any_of (bands.begin (), bands.end (), [] (float value) { return value > 0.2f; });
    };
    const auto isSilent = [] (const auto& bands) {
        // Native's unnormalized complex FFT starts from a constant (127,
        // 1/127) on silence. Finite-precision leakage outside DC is tiny,
        // but need not be exactly zero.
        return std::all_of (bands.begin (), bands.end (), [] (float value) {
            return std::isfinite (value) && std::abs (value) < 0.001f;
        });
    };
    REQUIRE (hasEnergy (left.audio16[0]));
    REQUIRE (hasEnergy (left.audio32[0]));
    REQUIRE (hasEnergy (left.audio64[0]));
    REQUIRE (isSilent (left.audio16[1]));
    REQUIRE (isSilent (left.audio32[1]));
    REQUIRE (isSilent (left.audio64[1]));

    for (size_t sample = 0; sample < StereoSpectrum::Samples; ++sample)
        for (size_t byte = 0; byte < sizeof (float); ++byte)
            std::swap (signal[(sample * 2) * sizeof (float) + byte],
                       signal[(sample * 2 + 1) * sizeof (float) + byte]);
    analyzer.feed (signal.data (), signal.size ());
    REQUIRE (analyzer.take (right));
    REQUIRE (right.audio16[1] == left.audio16[0]);
    REQUIRE (right.audio32[1] == left.audio32[0]);
    REQUIRE (right.audio64[1] == left.audio64[0]);
    REQUIRE (isSilent (right.audio16[0]));
    REQUIRE (isSilent (right.audio32[0]));
    REQUIRE (isSilent (right.audio64[0]));

    const float nan = std::numeric_limits<float>::quiet_NaN ();
    const float infinity = std::numeric_limits<float>::infinity ();
    std::memcpy (signal.data () + 3 * 2 * sizeof (float), &nan, sizeof (float));
    std::memcpy (signal.data () + 7 * 2 * sizeof (float), &infinity, sizeof (float));
    analyzer.feed (signal.data (), signal.size ());
    REQUIRE (analyzer.take (right));
    REQUIRE (isSilent (right.audio64[0]));
    REQUIRE (right.audio64[1] == left.audio64[0]);

    std::array<uint8_t, StereoSpectrum::WindowBytes> highTone {};
    for (size_t sample = 0; sample < StereoSpectrum::Samples; ++sample) {
        const float phase = 2.0f * std::numbers::pi_v<float> * 12000.0f
            * static_cast<float> (sample) / 44100.0f;
        const float value = 0.8f * std::sin (phase);
        std::memcpy (highTone.data () + sample * 2 * sizeof (float), &value, sizeof (float));
    }
    analyzer.feed (highTone.data (), highTone.size ());
    REQUIRE (analyzer.take (right));
    const auto highBandHasEnergy = [] (const auto& bands) {
        return std::any_of (bands.begin () + bands.size () / 2, bands.end (),
                            [] (float value) { return value > 0.2f; });
    };
    REQUIRE (highBandHasEnergy (right.audio16[0]));
    REQUIRE (highBandHasEnergy (right.audio32[0]));
    REQUIRE (highBandHasEnergy (right.audio64[0]));
    REQUIRE (isSilent (right.audio64[1]));

    analyzer.feedSilence (signal.size ()); // PulseAudio hole preserves frame alignment
    REQUIRE (analyzer.take (silence));
    REQUIRE (isSilent (silence.audio16[0]));
    REQUIRE (isSilent (silence.audio16[1]));
    REQUIRE (isSilent (silence.audio32[0]));
    REQUIRE (isSilent (silence.audio32[1]));
    REQUIRE (isSilent (silence.audio64[0]));
    REQUIRE (isSilent (silence.audio64[1]));

    analyzer.feed (signal.data (), 17);
    analyzer.reset (); // a new PulseAudio monitor must discard partial old bytes
    analyzer.feed (signal.data (), signal.size ());
    REQUIRE (analyzer.take (right));
    REQUIRE (right.audio64[1] == left.audio64[0]);
}

TEST_CASE ("Native audio band hierarchy combines channels before pairwise maxima", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    left[0] = 1.0f;
    right[1] = 1.0f;
    left[7] = 0.4f;
    right[7] = 0.8f;
    const auto bands = StereoSpectrum::from64 (left, right);
    REQUIRE (bands.audio64[0][0] == 1.0f);
    REQUIRE (bands.audio64[1][0] == 0.0f);
    REQUIRE (bands.combined64[0] == 0.5f);
    REQUIRE (bands.combined64[1] == 0.5f);
    REQUIRE (bands.audio32[0][0] == 1.0f);
    REQUIRE (bands.audio32[1][0] == 1.0f);
    REQUIRE (bands.combined32[0] == 0.5f);
    REQUIRE (bands.audio16[0][0] == 1.0f);
    REQUIRE (bands.audio16[1][0] == 1.0f);
    REQUIRE (bands.combined16[0] == 0.5f);
    REQUIRE (bands.audio32[0][3] == 0.4f);
    REQUIRE (bands.audio32[1][3] == 0.8f);
    REQUIRE (bands.combined32[3] == 0.6f);
    REQUIRE (bands.audio16[0][1] == 0.4f);
    REQUIRE (bands.audio16[1][1] == 0.8f);
    REQUIRE (bands.combined16[1] == 0.6f);
}

TEST_CASE ("Script audio average is the combined block, not the mean of reduced channels", "[audio][spectrum][script]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    std::array<float, 64> left {};
    std::array<float, 64> right {};
    // Stereo peaks in different bins of one 16-band group: the reduced
    // channels both read 1, but no single bin has a stereo mean above 0.5.
    left[4] = 1.0f;
    right[7] = 1.0f;
    // Coincident peaks keep the mean of the channels.
    left[63] = 0.25f;
    right[63] = 0.75f;
    const auto bands = StereoSpectrum::from64 (left, right);
    for (const uint32_t resolution : {16u, 32u, 64u}) {
        const auto blocks = StereoSpectrum::scriptBlocks (bands, resolution);
        const size_t width = 64 / resolution;
        REQUIRE (blocks.left[4 / width] == 1.0f);
        REQUIRE (blocks.right[7 / width] == 1.0f);
        REQUIRE (blocks.average[4 / width] == 0.5f);
        REQUIRE (blocks.average[7 / width] == 0.5f);
        REQUIRE (blocks.average[resolution - 1] == 0.5f);
    }
    const auto sixteen = StereoSpectrum::scriptBlocks (bands, 16);
    REQUIRE ((sixteen.left[1] + sixteen.right[1]) * 0.5f == 1.0f);
    REQUIRE (sixteen.left == bands.audio16[0].data ());
    REQUIRE (sixteen.right == bands.audio16[1].data ());
    REQUIRE (sixteen.average == bands.combined16.data ());
}

TEST_CASE ("Published spectrum reduces channels after temporal 64-bin smoothing", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    std::array<float, 64> oldLeft {};
    std::array<float, 64> oldRight {};
    std::array<float, 64> newLeft {};
    std::array<float, 64> newRight {};
    oldLeft[0] = 1.0f;
    oldRight[1] = 1.0f;
    newLeft[1] = 1.0f;
    newRight[0] = 1.0f;

    auto published = StereoSpectrum::from64 (oldLeft, oldRight);
    const auto destination = StereoSpectrum::from64 (newLeft, newRight);
    REQUIRE (published.audio32[0][0] == 1.0f);
    REQUIRE (destination.audio32[0][0] == 1.0f);
    StereoSpectrum::advancePublished (published, destination, 0.3f);

    REQUIRE (std::abs (published.audio64[0][0] - 0.7f) < 0.00001f);
    REQUIRE (std::abs (published.audio64[0][1] - 0.3f) < 0.00001f);
    REQUIRE (std::abs (published.audio64[1][0] - 0.3f) < 0.00001f);
    REQUIRE (std::abs (published.audio64[1][1] - 0.7f) < 0.00001f);
    REQUIRE (std::abs (published.audio32[0][0] - 0.7f) < 0.00001f);
    REQUIRE (std::abs (published.audio32[1][0] - 0.7f) < 0.00001f);
    REQUIRE (std::abs (published.audio16[0][0] - 0.7f) < 0.00001f);
    REQUIRE (std::abs (published.audio16[1][0] - 0.7f) < 0.00001f);
    REQUIRE (published.combined32[0] == 0.5f);
    REQUIRE (published.combined16[0] == 0.5f);
}

TEST_CASE ("Native 64-band mapping preserves sequential cap and excludes endpoint wrap", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumBand;
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumMappedBand;
    constexpr unsigned binCount = 640; // v2.8.42 default: int(10.0f * 64.0f)
    unsigned band = 0;
    std::array<unsigned, 7> observed {};
    for (unsigned bin = 1; bin < binCount; ++bin) {
        band = nativeSpectrumBand (band, bin, binCount);
        switch (bin) {
            case 1: observed[0] = band; break;
            case 2: observed[1] = band; break;
            case 32: observed[2] = band; break;
            case 64: observed[3] = band; break;
            case 128: observed[4] = band; break;
            case 320: observed[5] = band; break;
            case 639: observed[6] = band; break;
        }
    }
    REQUIRE (observed == std::array<unsigned, 7> {0, 1, 30, 35, 42, 53, 63});
    REQUIRE (band == 63);
    // A bin at ratio 1 would modulo-wrap to zero, but the native loop stops at 639.
    REQUIRE (nativeSpectrumMappedBand (640, binCount) == 0);
}

TEST_CASE ("Native 64-band producer weights finite complex bins without output clipping", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumScale;
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumReduce;
    constexpr unsigned binCount = 640;
    constexpr unsigned fftLength = 1920;
    std::array<kiss_fft_cpx, binCount + 1> transformed {};
    transformed[1] = { 2.0f, 0.0f };
    transformed[2] = { 3.0f, 4.0f };
    transformed[64] = { std::numeric_limits<float>::infinity (), 0.0f };
    transformed[638] = { 1000000.0f, 0.0f };
    transformed[639] = { 3.0f, 0.0f };
    transformed[640] = { 1000000.0f, 0.0f }; // excluded ratio-one wrap

    const auto bands = nativeSpectrumReduce (std::span<const kiss_fft_cpx> (transformed),
                                             binCount, fftLength);
    const float scale = 0.001f * 640.0f / 960.0f;
    REQUIRE (std::abs (bands[0] - std::sqrt (0.008f) * scale) < 0.000001f);
    REQUIRE (bands[1] > bands[0]);
    REQUIRE (bands[35] == 0.0f); // nonfinite power was replaced with zero
    REQUIRE (bands[63] > 600.0f);
    REQUIRE (bands[63] < 700.0f);
    REQUIRE (bands[0] < 0.001f); // excluded bin 640 did not wrap into band zero
    REQUIRE (std::all_of (bands.begin (), bands.end (), [] (float value) {
        return std::isfinite (value) && value >= 0.0f;
    }));
    // The old multiply-then-divide order rounded one ULP lower for this gain.
    REQUIRE (std::bit_cast<uint32_t> (nativeSpectrumScale (0.7f, 640, 1920)) == 0x39f4aaf2u);
}

TEST_CASE ("Native complex FFT input preserves sample boundaries", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumInput;
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumFftLength;
    REQUIRE (nativeSpectrumFftLength (44100) == 1920);
    REQUIRE (nativeSpectrumFftLength (48000) == 2089);
    REQUIRE (nativeSpectrumFftLength (22050) == 1920); // native rate factor has a floor of one
    REQUIRE (nativeSpectrumFftLength (88200) == 3840);
    const auto silence = nativeSpectrumInput (0.0f);
    REQUIRE (silence.r == 127.0f);
    REQUIRE (std::abs (silence.i - 1.0f / 127.0f) < 0.0000001f);

    const auto maximum = nativeSpectrumInput (1.0f);
    REQUIRE (maximum.r == 254.0f);
    REQUIRE (std::abs (maximum.i - 1.0f / 254.0f) < 0.0000001f);

    const auto minimum = nativeSpectrumInput (-1.0f);
    REQUIRE (minimum.r == 0.0f);
    REQUIRE (std::isinf (minimum.i));
    REQUIRE (minimum.i > 0.0f);
}

TEST_CASE ("Native capture prefix retains the full transform length", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumCaptureLength;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    // Literal results of divss/mulss/subss/cvttss2si.
    REQUIRE (nativeSpectrumCaptureLength (1920) == 1280);
    REQUIRE (nativeSpectrumCaptureLength (2089) == 1392);
    REQUIRE (nativeSpectrumCaptureLength (3840) == 2560);
    REQUIRE (nativeSpectrumCaptureLength (4179) == 2786);
    StereoSpectrum analyzer (48000);
    REQUIRE (analyzer.captureSamples () == 1392);
    REQUIRE (analyzer.captureBytes () == 1392 * 2 * sizeof (float));
    REQUIRE (analyzer.samples () == 2089);
}

TEST_CASE ("Native capture discards released packet excess and retains incomplete prefixes", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    StereoSpectrum analyzer (48000), reference (48000);
    std::vector<float> packet (2089 * 2, 0.0f);
    for (size_t n = 0; n < 1392; ++n)
        packet[n * 2] = 0.1f * std::sin (2.0f * std::numbers::pi_v<float> * 80.0f * n / 48000.0f);
    // Audio beyond the native ceiling must neither fill the silent FFT tail
    // nor become the next capture, even when it arrives in a separate packet.
    for (size_t n = 1392; n < 2089; ++n) packet[n * 2 + 1] = 0.75f;
    const auto* bytes = reinterpret_cast<const uint8_t*> (packet.data ());
    analyzer.feed (bytes, 1392 * 8 - 1);
    StereoSpectrum::Bands actual {}, expected {};
    REQUIRE_FALSE (analyzer.take (actual));
    analyzer.feed (bytes + 1392 * 8 - 1, packet.size () * sizeof (float) - (1392 * 8 - 1));
    analyzer.feed (bytes + 1392 * 8, (2089 - 1392) * 8);
    reference.feed (bytes, 1392 * 8);
    REQUIRE (reference.take (expected));
    REQUIRE (analyzer.take (actual));
    REQUIRE (actual.audio64 == expected.audio64);
    REQUIRE (std::all_of (actual.audio64[1].begin (), actual.audio64[1].end (),
                         [] (float value) { return std::abs (value) < 0.001f; }));
    std::vector<uint8_t> silence (1392 * 8, 0);
    analyzer.feed (silence.data (), 15);
    REQUIRE_FALSE (analyzer.take (actual));
    analyzer.feed (silence.data () + 15, silence.size () - 16);
    REQUIRE_FALSE (analyzer.take (actual));
    analyzer.feed (silence.data () + silence.size () - 1, 1);
    REQUIRE (analyzer.take (actual));
    for (const auto& channel : actual.audio64)
        REQUIRE (std::all_of (channel.begin (), channel.end (),
                             [] (float value) { return std::abs (value) < 0.001f; }));
}

TEST_CASE ("Captured stereo PCM preserves the native partial-window leakage shape", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    // One exact 2 kHz cycle from the controlled 48 kHz float32 monitor capture,
    // frame144000. PCM SHA256: 027f3815526aae97d2fdaa4c77d35ea887805e19e34a209471c6c6900fe8d371.
    // Both client capture formats and input levels were independently checked.
    constexpr std::array<float, 24> cycle {
        0.0f, 0.0005880712415f, 0.00113711122f, 0.001608088613f,
        0.00196891115f, 0.002196159912f, 0.002273355145f, 0.002196159912f,
        0.00196891115f, 0.001608088613f, 0.00113711122f, 0.0005880712415f,
        0.0f, -0.0005880712415f, -0.00113711122f, -0.001608088613f,
        -0.00196891115f, -0.002196159912f, -0.002273355145f, -0.002196159912f,
        -0.00196891115f, -0.001608088613f, -0.00113711122f, -0.0005880712415f
    };
    std::vector<float> packet (2089 * 2);
    for (size_t n = 0; n < 2089; ++n) packet[n * 2] = packet[n * 2 + 1] = cycle[n % cycle.size ()];
    StereoSpectrum analyzer (48000);
    analyzer.feed (reinterpret_cast<const uint8_t*> (packet.data ()), packet.size () * sizeof (float));
    StereoSpectrum::Bands bands;
    REQUIRE (analyzer.take (bands));
    // Independent double DFT of the literal PCM prefix1392 + silent tail697,
    // followed by the recovered native bin weighting/reduction. These side
    // bands distinguish the observed prefix from a full2089-sample tone.
    constexpr std::array<size_t, 6> indices {35, 37, 38, 39, 40, 50};
    constexpr std::array<double, 6> expected {
        0.00042205169314, 0.00141705445658, 0.02636764108288,
        0.00295341666901, 0.00094489651173, 0.00011277150229
    };
    REQUIRE (bands.audio64[0] == bands.audio64[1]);
    for (size_t i = 0; i < indices.size (); ++i)
        REQUIRE (std::abs (bands.audio64[0][indices[i]] - expected[i]) < 0.000002 + expected[i] * 0.005);
}

TEST_CASE ("Stereo spectrum sizes each window from the negotiated capture rate", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumInput;
    StereoSpectrum analyzer (48000);
    REQUIRE (analyzer.sampleRate () == 48000);
    REQUIRE (analyzer.samples () == 2089);
    REQUIRE (analyzer.windowBytes () == 2089 * 2 * sizeof (float));
    REQUIRE_THROWS_AS (analyzer.configureSampleRate (0), std::invalid_argument);
    REQUIRE (analyzer.sampleRate () == 48000);

    constexpr size_t toneBin = 32;
    std::vector<uint8_t> window (analyzer.windowBytes ());
    std::complex<double> directBin {};
    for (size_t n = 0; n < analyzer.samples (); ++n) {
        const double phase = 2.0 * std::numbers::pi_v<double> * static_cast<double> (toneBin * n)
            / static_cast<double> (analyzer.samples ());
        const float value = static_cast<float> (0.1 * std::sin (phase));
        std::memcpy (window.data () + n * 2 * sizeof (float), &value, sizeof (value));
        const auto input = nativeSpectrumInput (n < analyzer.captureSamples () ? value : 0.0f);
        directBin += std::complex<double> (input.r, input.i) * std::polar (1.0, -phase);
    }
    StereoSpectrum::Bands bands;
    analyzer.feed (window.data (), analyzer.captureBytes () - 1);
    REQUIRE_FALSE (analyzer.take (bands));
    analyzer.feed (window.data () + analyzer.captureBytes () - 1, 1);
    REQUIRE (analyzer.take (bands));
    const double ratio = static_cast<double> (toneBin - 1) / 639.0;
    const double weight = 0.501 - std::cos (std::numbers::pi_v<double> * ratio) * (1.0 - 0.501);
    const double expected = std::sqrt (std::norm (directBin) * weight)
        * (0.001 * 640.0 / (static_cast<double> (analyzer.samples ()) * 0.5));
    REQUIRE (expected > 0.2);
    REQUIRE (std::abs (static_cast<double> (bands.audio64[0][30]) - expected) / expected < 0.01);
    REQUIRE (std::all_of (bands.audio64[1].begin (), bands.audio64[1].end (),
                         [] (float value) { return std::isfinite (value) && value < 0.001f; }));

    analyzer.feed (window.data (), 17); // stale partial frame from the old rate
    analyzer.configureSampleRate (44100);
    REQUIRE (analyzer.sampleRate () == 44100);
    REQUIRE (analyzer.samples () == StereoSpectrum::Samples);
    REQUIRE (analyzer.windowBytes () == StereoSpectrum::WindowBytes);
    REQUIRE (analyzer.take (bands)); // rate switch publishes a cleared window
    REQUIRE_FALSE (analyzer.take (bands));
    std::vector<uint8_t> silence (analyzer.windowBytes ());
    analyzer.feed (silence.data (), analyzer.captureBytes () - 1);
    REQUIRE_FALSE (analyzer.take (bands));
    analyzer.feed (silence.data () + analyzer.captureBytes () - 1, 1);
    REQUIRE (analyzer.take (bands));
    REQUIRE (std::all_of (bands.audio64[0].begin (), bands.audio64[0].end (),
                         [] (float value) { return std::isfinite (value) && value < 0.001f; }));
}

TEST_CASE ("Recovered nonpower callback equals forward complex DFT", "[audio][spectrum]") {
    using Complex = std::complex<double>;
    const auto forwardDft = [] (const std::vector<Complex>& values) {
        std::vector<Complex> result (values.size ());
        for (size_t k = 0; k < values.size (); ++k) {
            for (size_t n = 0; n < values.size (); ++n) {
                const double phase = -2.0 * std::numbers::pi_v<double> * static_cast<double> (k * n)
                    / static_cast<double> (values.size ());
                result[k] += values[n] * std::polar (1.0, phase);
            }
        }
        return result;
    };
    // The five complex samples are asymmetric and have nonzero imaginary
    // parts; a real FFT, reverse-sign FFT or missing 1/M all fail this case.
    const std::vector<Complex> input {
        {1.0, 2.0}, {-0.3, 0.8}, {3.0, -1.0}, {0.1, -0.2}, {2.2, 0.4}
    };
    constexpr size_t length = 5;
    constexpr size_t workLength = 16;
    std::vector<Complex> firstInput (workLength);
    std::vector<Complex> kernel (workLength);
    std::array<Complex, length> chirp {};
    for (size_t n = 0; n < length; ++n) {
        chirp[n] = std::polar (1.0, std::numbers::pi_v<double> * static_cast<double> (n * n)
            / static_cast<double> (length));
        firstInput[n] = input[n] * std::conj (chirp[n]);
        kernel[n] = chirp[n] / static_cast<double> (workLength);
        if (n) kernel[workLength - n] = kernel[n];
    }
    auto first = forwardDft (firstInput);
    const auto transformedKernel = forwardDft (kernel);
    const Complex imaginaryUnit {0.0, 1.0};
    for (size_t k = 0; k < workLength; ++k) {
        // The middle callback arithmetic is i*conj(first*kernel), allowing
        // a second forward transform to implement the convolution.
        first[k] = imaginaryUnit * std::conj (first[k] * transformedKernel[k]);
    }
    const auto second = forwardDft (first);
    const auto direct = forwardDft (input);
    REQUIRE (std::abs (direct[1] - Complex (-1.01062599, 4.01470485)) < 0.00000001);
    for (size_t k = 0; k < length; ++k) {
        // Native final callback arithmetic is i*conj(second*chirp).
        const Complex recovered = imaginaryUnit * std::conj (second[k] * chirp[k]);
        REQUIRE (std::abs (recovered - direct[k]) < 0.000000001);
    }
}

TEST_CASE ("Production complex analyzer places an exact-bin tone at its numeric native band", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumInput;
    constexpr size_t toneBin = 32;
    constexpr size_t outputBand = 30;
    std::array<uint8_t, StereoSpectrum::WindowBytes> samples {};
    std::complex<double> directBin {};
    for (size_t n = 0; n < StereoSpectrum::Samples; ++n) {
        const double phase = 2.0 * std::numbers::pi_v<double> * static_cast<double> (toneBin * n)
            / static_cast<double> (StereoSpectrum::Samples);
        const float value = static_cast<float> (0.1 * std::sin (phase));
        std::memcpy (samples.data () + n * 2 * sizeof (float), &value, sizeof (value));
        const auto input = nativeSpectrumInput (n < 1280 ? value : 0.0f);
        directBin += std::complex<double> (input.r, input.i) * std::polar (1.0, -phase);
    }

    StereoSpectrum analyzer;
    analyzer.feed (samples.data (), samples.size ());
    StereoSpectrum::Bands bands;
    REQUIRE (analyzer.take (bands));
    const double ratio = static_cast<double> (toneBin - 1) / 639.0;
    const double weight = 0.501 - std::cos (std::numbers::pi_v<double> * ratio) * (1.0 - 0.501);
    const double expected = std::sqrt (std::norm (directBin) * weight) * (0.001 * 640.0 / 960.0);
    REQUIRE (expected > 0.2);
    REQUIRE (std::abs (static_cast<double> (bands.audio64[0][outputBand]) - expected) / expected < 0.005);
    REQUIRE (std::all_of (bands.audio64[1].begin (), bands.audio64[1].end (), [] (float value) {
        return std::isfinite (value) && value < 0.001f;
    }));

    // An exact -1 input yields +Inf reciprocal in the native preprocessing.
    // The reducer must keep every published band finite and isolated to its channel.
    samples.fill (0);
    const float negativeUnity = -1.0f;
    std::memcpy (samples.data (), &negativeUnity, sizeof (negativeUnity));
    analyzer.feed (samples.data (), samples.size ());
    REQUIRE (analyzer.take (bands));
    for (const auto& channel : bands.audio64) {
        REQUIRE (std::all_of (channel.begin (), channel.end (), [] (float value) {
            return std::isfinite (value);
        }));
    }
}

TEST_CASE ("Per-scene native spectrum filter retains independent histories before reduction", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::NativeSpectrumSmoother;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
#if defined(__SSE__)
    constexpr float normalizedUnity = 0.999755859375f; // Native RCPPS(1).
#else
    constexpr float normalizedUnity = 1.0f;
#endif
    NativeSpectrumSmoother firstScene;
    StereoSpectrum::Bands raw {};
    raw.audio64[0][0] = 1.0f;
    auto first = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (first.audio64[0][0] - 0.2f * normalizedUnity) < 0.000001f);
    REQUIRE (first.audio64[1][0] == 0.0f);
    REQUIRE (std::abs (first.combined32[0] - 0.1f * normalizedUnity) < 0.000001f);

    raw.audio64[0][0] = 0.0f;
    raw.audio64[0][1] = 1.0f;
    const auto crossed = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (crossed.audio64[0][0] - 0.16f * normalizedUnity) < 0.000001f);
    REQUIRE (std::abs (crossed.audio64[0][1] - 0.2f * normalizedUnity) < 0.000001f);
    REQUIRE (std::abs (crossed.audio32[0][0] - 0.2f * normalizedUnity) < 0.000001f);
    REQUIRE (std::abs (crossed.combined32[0] - 0.1f * normalizedUnity) < 0.000001f);

    NativeSpectrumSmoother secondScene;
    StereoSpectrum::Bands otherRaw {};
    otherRaw.audio64[1][7] = 1.0f;
    const auto second = secondScene.update (otherRaw, 0.01f, 0.5f, 1.0f);
    REQUIRE (second.audio64[0][0] == 0.0f);
    REQUIRE (std::abs (second.audio64[1][7] - 0.1f * normalizedUnity) < 0.000001f);
    const auto silent = firstScene.update (StereoSpectrum::Bands {}, 0.01f, 1.0f, 1.0f);
    REQUIRE (silent.audio64[0][0] == 0.0f);
    REQUIRE (silent.audio64[0][1] == 0.0f);
    const auto resumed = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (resumed.audio64[0][1] - 0.36f * normalizedUnity) < 0.000001f);

    NativeSpectrumSmoother normalizedScene;
    StereoSpectrum::Bands loud {};
    loud.audio64[0][0] = 2.0f;
    const auto normalized = normalizedScene.update (loud, 0.01f, 1.0f, 1.0f);
    // The adaptive group coefficient becomes 1.01; native divides raw by it
    // via rcpps before the 0.2 intermediate step. Multiplication would hit
    // the published 0.4 cap instead of producing about 0.396.
    REQUIRE (std::abs (normalized.audio64[0][0] - (2.0f / 1.01f) * 0.2f) < 0.0001f);
    REQUIRE (normalized.audio64[0][0] < 0.4f);
}

#if defined(__SSE__)
TEST_CASE ("Native spectrum normalization preserves reciprocal estimate through history", "[audio][spectrum][native-rcp]") {
    using WallpaperEngine::Audio::Drivers::Recorders::NativeSpectrumSmoother;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    // Original v2.8.42 bytes (RCPPS) and
    // (MULPS/SUBPS/MULPS/ADDPS), executed
    // independently with controlled registers and the native 0.01*20 step.
    // These words discriminate DIV even when the coefficient is exactly one.
    NativeSpectrumSmoother scene;
    StereoSpectrum::Bands raw {};
    raw.audio64[0][0] = 1.0f;
    const auto activated = scene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (activated.audio64[0][0]) == 1045217279u);
    REQUIRE (activated.audio64[1][0] == 0.0f);

    // Keep the group's peak at one while the former band decays.
    raw.audio64[0][0] = 0.0f;
    raw.audio64[0][1] = 1.0f;
    const auto crossed = scene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (crossed.audio64[0][0]) == 1042533580u);
    REQUIRE (std::bit_cast<uint32_t> (crossed.audio64[0][1]) == 1045217279u);
    REQUIRE (std::bit_cast<uint32_t> (crossed.audio32[0][0]) == 1045217279u);

    REQUIRE (scene.update ({}, 0.01f, 1.0f, 1.0f).audio64[0][0] == 0.0f);
    raw.audio64[0][0] = 1.0f;
    raw.audio64[0][1] = 0.0f;
    const auto resumed = scene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (resumed.audio64[0][0]) == 1051190558u);

    NativeSpectrumSmoother loudScene;
    raw = {};
    raw.audio64[0][0] = 2.0f;
    const auto loud = loudScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (loud.audio64[0][0]) == 1053478092u);
    REQUIRE (loud.audio64[0][0] < 0.4f); // Remains below the publication cap.
    // Recompute after both a full-speed coefficient rise (1.01 -> 1.02)
    // and a half-speed fall (1.02 -> 1.015); a stale reciprocal fails.
    const auto rising = loudScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (rising.audio64[0][0]) == 1060472094u);
    raw.audio64[0][0] = 0.5f;
    const auto falling = loudScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::bit_cast<uint32_t> (falling.audio64[0][0]) == 1059745816u);
}
#endif

TEST_CASE ("Scene spectrum handoff retains stable shader and particle arrays across viewports", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::SceneSpectrumState;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
#if defined(__SSE__)
    constexpr float normalizedUnity = 0.999755859375f; // Native RCPPS(1).
#else
    constexpr float normalizedUnity = 1.0f;
#endif
    SceneSpectrumState firstScene;
    SceneSpectrumState secondScene;
    StereoSpectrum::Bands leftRaw {};
    StereoSpectrum::Bands rightRaw {};
    leftRaw.audio64[0][0] = 1.0f;
    rightRaw.audio64[1][7] = 1.0f;
    const auto start = std::chrono::steady_clock::time_point {};
    const auto firstFrame = start + std::chrono::milliseconds (10);
    const float* shaderLeft = firstScene.bands ().audio64[0].data ();
    const float* shaderRight = secondScene.bands ().audio64[1].data ();
    REQUIRE (firstScene.advance ({}, 0, start, 1.0f, 1.0f));
    REQUIRE (secondScene.advance ({}, 0, start, 1.0f, 1.0f));
    REQUIRE (firstScene.advance (leftRaw, 1, firstFrame, 1.0f, 1.0f));
    REQUIRE (secondScene.advance (rightRaw, 1, firstFrame, 0.5f, 1.0f));
    REQUIRE (firstScene.bands ().audio64[0].data () == shaderLeft);
    REQUIRE (secondScene.bands ().audio64[1].data () == shaderRight);
    REQUIRE (std::abs (shaderLeft[0] - 0.2f * normalizedUnity) < 0.000001f);
    REQUIRE (std::abs (shaderRight[7] - 0.1f * normalizedUnity) < 0.000001f);

    WallpaperEngine::Audio::ParticleAudioSettings response {
        .mode = 1, .lowerBound = 0.0f, .upperBound = 1.0f,
        .exponent = 1.0f, .firstBand = 0, .lastBand = 0};
    REQUIRE (WallpaperEngine::Audio::particleAudioResponse (
        firstScene.bands ().audio16[0], firstScene.bands ().audio16[1], response) > 0.0f);
    REQUIRE (WallpaperEngine::Audio::particleAudioResponse (
        secondScene.bands ().audio16[0], secondScene.bands ().audio16[1], response) == 0.0f);

    // Repeated viewports at the same time and a paused scene do not advance
    // either filter, even if the shared recorder receives a different window.
    REQUIRE_FALSE (firstScene.advance (rightRaw, 1, firstFrame, 1.0f, 1.0f));
    REQUIRE (std::abs (firstScene.bands ().audio64[0][0] - 0.2f * normalizedUnity) < 0.000001f);
    REQUIRE (std::abs (secondScene.bands ().audio64[1][7] - 0.1f * normalizedUnity) < 0.000001f);
    REQUIRE (firstScene.advance (StereoSpectrum::Bands {}, 2,
                                 firstFrame + std::chrono::milliseconds (10), 1.0f, 1.0f));
    REQUIRE (firstScene.bands ().audio64[0][0] == 0.0f);
    REQUIRE (std::abs (secondScene.bands ().audio64[1][7] - 0.1f * normalizedUnity) < 0.000001f);
    REQUIRE (firstScene.advance (leftRaw, 3,
                                 firstFrame + std::chrono::milliseconds (20), 1.0f, 1.0f));
    REQUIRE (firstScene.bands ().audio64[0][0] > 0.2f);
    REQUIRE (firstScene.bands ().audio64[0].data () == shaderLeft);
}

TEST_CASE ("Scene spectrum rate follows native percent floor", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::sceneSpectrumRate;
    REQUIRE (sceneSpectrumRate (0.0f) == 0.1f);
    REQUIRE (sceneSpectrumRate (5.0f) == 0.1f);
    REQUIRE (sceneSpectrumRate (50.0f) == 0.5f);
    REQUIRE (sceneSpectrumRate (100.0f) == 1.0f);
    REQUIRE (sceneSpectrumRate (200.0f) == 2.0f);
    REQUIRE (sceneSpectrumRate (std::numeric_limits<float>::infinity ()) == 1.0f);
}

TEST_CASE ("Native spectrum bounds host elapsed before applying rate and fade", "[audio][spectrum][host-delta]") {
    using WallpaperEngine::Audio::Drivers::Recorders::nativeSpectrumFilterDelta;
    // Original v2.8.42 instruction windows,
    // executed with controlled
    // host fields/registers. These discriminate a single clamp after scaling.
    REQUIRE (nativeSpectrumFilterDelta (1.0f, 0.5f, 1.0f) == 0.125f);
    REQUIRE (nativeSpectrumFilterDelta (2.0f, 1.0f, 0.25f) == 0.0625f);
    REQUIRE (std::bit_cast<uint32_t> (nativeSpectrumFilterDelta (0.00001f, 10.0f, 1.0f)) == 981668462u);
    REQUIRE (nativeSpectrumFilterDelta (0.5f, 1.0f, 1.0f) == 0.25f);
    REQUIRE (nativeSpectrumFilterDelta (0.01f, 1.0f, 1.0f) == 0.01f);
    // Native multiplies rate*fade before elapsed. Reassociation differs by
    // one ULP even when neither clamp is active.
    REQUIRE (std::bit_cast<uint32_t> (nativeSpectrumFilterDelta (
        std::bit_cast<float> (1046349214u), std::bit_cast<float> (1040238641u),
        std::bit_cast<float> (1046781144u))) == 1002928928u);

    using WallpaperEngine::Audio::Drivers::Recorders::NativeSpectrumSmoother;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
    NativeSpectrumSmoother delayed;
    NativeSpectrumSmoother bounded;
    StereoSpectrum::Bands raw {};
    raw.audio64[0][0] = 1.0f;
    // At 10% host rate, a long stalled frame advances as a bounded .25s
    // frame. It must not snap the complete filter history to the new raw bank.
    const auto afterGap = delayed.update (raw, 2.0f, 0.1f, 1.0f);
    const auto afterBound = bounded.update (raw, 0.25f, 0.1f, 1.0f);
    REQUIRE (afterGap.audio64 == afterBound.audio64);
    REQUIRE (afterGap.audio64[0][0] > 0.49f);
    REQUIRE (afterGap.audio64[0][0] < 0.51f);
    // Consequential retained history matches on the next ordinary frame too.
    raw.audio64[0][0] = 0.0f;
    raw.audio64[0][1] = 1.0f;
    REQUIRE (delayed.update (raw, 0.01f, 0.1f, 1.0f).audio64
             == bounded.update (raw, 0.01f, 0.1f, 1.0f).audio64);
}

TEST_CASE ("Particle audio response selects stereo mode and shapes peak once", "[audio][particle]") {
    using WallpaperEngine::Audio::ParticleAudioSettings;
    using WallpaperEngine::Audio::particleAudioResponse;
    std::array<float, 16> left {};
    std::array<float, 16> right {};
    left[3] = 0.5f;
    left[7] = 0.8f;
    right[3] = 1.0f;
    ParticleAudioSettings settings {.mode = 1, .lowerBound = 0.0f, .upperBound = 1.0f,
                                    .exponent = 2.0f, .firstBand = 7, .lastBand = 3};
    REQUIRE (std::abs (particleAudioResponse (left, right, settings) - 0.802816f) < 1e-5f);
    settings.mode = 2;
    REQUIRE (particleAudioResponse (left, right, settings) == 1.0f);
    settings.mode = 3;
    REQUIRE (std::abs (particleAudioResponse (left, right, settings) - 0.71191406f) < 1e-5f);
    settings.mode = 0;
    REQUIRE (particleAudioResponse (left, right, settings) == 0.0f);
    settings.mode = 1;
    settings.firstBand = 7;
    settings.lastBand = 7;
    settings.exponent = 0.5f;
    REQUIRE (std::abs (particleAudioResponse (left, right, settings) - std::sqrt (0.896f)) < 1e-5f);
    settings.lowerBound = 0.8f;
    settings.upperBound = 0.8f;
    REQUIRE (particleAudioResponse (left, right, settings) == 1.0f);
    settings.firstBand = 3;
    settings.lastBand = 3;
    REQUIRE (particleAudioResponse (left, right, settings) == 0.0f);
    settings.firstBand = 7;
    settings.lastBand = 7;
    settings.lowerBound = 0.7f;
    settings.upperBound = 0.7f;
    REQUIRE (particleAudioResponse (left, right, settings) == 1.0f);
    settings.firstBand = 3;
    settings.lastBand = 3;
    settings.lowerBound = 0.8f;
    settings.upperBound = 1.0f;
    settings.exponent = -1.0f;
    REQUIRE (particleAudioResponse (left, right, settings) == 1.0f);
}

TEST_CASE ("Particle emitter audio defaults remain inactive and exponent stays fractional", "[audio][particle]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto data = JSON::parse (R"({"id":1,"name":"audio-emitter","particle":{
        "emitter":[{"name":"boxrandom","rate":7}]}})");
    auto object = ObjectParser::parse (data, project);
    auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->emitters.size () == 1);
    REQUIRE (particle->emitters[0].audioProcessingMode == 0);
    REQUIRE (particle->emitters[0].audioProcessingExponent == 2.0f);

    data["particle"]["emitter"][0]["audioprocessingmode"] = 3;
    data["particle"]["emitter"][0]["audioprocessingexponent"] = 1.5f;
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->emitters[0].audioProcessingMode == 3);
    REQUIRE (particle->emitters[0].audioProcessingExponent == 1.5f);
}

TEST_CASE ("Vortex audio settings retain native defaults and authored frequency range", "[audio][particle]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::VortexOperator;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto data = JSON::parse (R"({"id":2,"name":"vortex-audio","particle":{
        "operator":[{"name":"vortex","speedinner":10,"speedouter":20}]}})");
    auto object = ObjectParser::parse (data, project);
    auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->operators.size () == 1);
    auto* vortex = dynamic_cast<const VortexOperator*> (particle->operators[0].get ());
    REQUIRE (vortex != nullptr);
    REQUIRE (vortex->audioProcessingMode->value->getInt () == 0);
    REQUIRE (vortex->audioProcessingBounds->value->getVec2 () == glm::vec2 (0.8f, 1.0f));
    REQUIRE (vortex->audioProcessingExponent->value->getFloat () == 2.0f);
    REQUIRE (vortex->audioProcessingFrequencyStart->value->getInt () == 0);
    REQUIRE (vortex->audioProcessingFrequencyEnd->value->getInt () == 1);

    data["particle"]["operator"][0]["audioprocessingmode"] = 2;
    data["particle"]["operator"][0]["audioprocessingexponent"] = 1.5f;
    data["particle"]["operator"][0]["audioprocessingfrequencystart"] = 4;
    data["particle"]["operator"][0]["audioprocessingfrequencyend"] = 9;
    object = ObjectParser::parse (data, project);
    particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    vortex = dynamic_cast<const VortexOperator*> (particle->operators[0].get ());
    REQUIRE (vortex != nullptr);
    REQUIRE (vortex->audioProcessingMode->value->getInt () == 2);
    REQUIRE (vortex->audioProcessingExponent->value->getFloat () == 1.5f);
    REQUIRE (vortex->audioProcessingFrequencyStart->value->getInt () == 4);
    REQUIRE (vortex->audioProcessingFrequencyEnd->value->getInt () == 9);
}

TEST_CASE ("Every particle audio consumer shares the native audio parser defaults", "[audio][particle]") {
    using WallpaperEngine::Audio::ParticleAudioSettings;
    using WallpaperEngine::Audio::particleAudioResponse;
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Particle;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::TurbulenceOperator;
    using WallpaperEngine::Data::Model::TurbulentVelocityRandomInitializer;
    using WallpaperEngine::Data::Model::VortexOperator;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    // inserts "0.8 1.0", exponent 2 and bins 0..1 for every caller in
    // emitters, turbulentvelocityrandom, turbulence, vortex, vortex_v2.
    Project project {};
    const auto data = JSON::parse (R"({"id":3,"name":"audio-defaults","particle":{
        "emitter":[{"name":"sphererandom"}],
        "initializer":[{"name":"turbulentvelocityrandom"}],
        "operator":[{"name":"turbulence","audioprocessingmode":2},{"name":"vortex"},{"name":"vortex_v2"}]}})");
    const auto object = ObjectParser::parse (data, project);
    const auto* particle = dynamic_cast<const Particle*> (object.get ());
    REQUIRE (particle != nullptr);
    REQUIRE (particle->emitters[0].audioProcessingBounds == glm::vec2 (0.8f, 1.0f));
    REQUIRE (particle->emitters[0].audioProcessingExponent == 2.0f);
    REQUIRE (particle->emitters[0].audioProcessingFrequencyStart == 0);
    REQUIRE (particle->emitters[0].audioProcessingFrequencyEnd == 1);
    const auto requireDefaults = [] (const auto& record) {
        REQUIRE (record.audioProcessingBounds->value->getVec2 () == glm::vec2 (0.8f, 1.0f));
        REQUIRE (record.audioProcessingExponent->value->getFloat () == 2.0f);
        REQUIRE (record.audioProcessingFrequencyStart->value->getInt () == 0);
        REQUIRE (record.audioProcessingFrequencyEnd->value->getInt () == 1);
    };
    requireDefaults (*particle->initializers[0]->as<TurbulentVelocityRandomInitializer> ());
    const auto& turbulence = *particle->operators[0]->as<TurbulenceOperator> ();
    requireDefaults (turbulence);
    requireDefaults (*particle->operators[1]->as<VortexOperator> ());
    requireDefaults (*particle->operators[2]->as<VortexOperator> ());

    // Original on the w08-audio "ramp" spectrum:
    // right-channel mode with the shared defaults returns 0x3e7ffff0; the old
    // turbulence defaults (0..1, exponent 1, bins 0..15) would give 0x3f78d4fd.
    constexpr std::array<uint32_t, 32> ramp {
        0x00000000u, 0x3d888889u, 0x3e088889u, 0x3e4ccccdu, 0x3e888889u, 0x3eaaaaabu, 0x3ecccccdu, 0x3eeeeeefu,
        0x3f088889u, 0x3f19999au, 0x3f2aaaabu, 0x3f3bbbbcu, 0x3f4ccccdu, 0x3f5ddddeu, 0x3f6eeeefu, 0x3f800000u,
        0x3f666666u, 0x3f570a3du, 0x3f47ae14u, 0x3f3851ecu, 0x3f28f5c3u, 0x3f19999au, 0x3f0a3d71u, 0x3ef5c28fu,
        0x3ed70a3du, 0x3eb851ecu, 0x3e99999au, 0x3e75c28fu, 0x3e3851ecu, 0x3df5c28fu, 0x3d75c28fu, 0x00000000u};
    std::array<float, 16> left {};
    std::array<float, 16> right {};
    for (size_t bin = 0; bin < 16; ++bin) {
        left[bin] = std::bit_cast<float> (ramp[bin]);
        right[bin] = std::bit_cast<float> (ramp[bin + 16]);
    }
    const glm::vec2 bounds = turbulence.audioProcessingBounds->value->getVec2 ();
    const ParticleAudioSettings settings {
        .mode = turbulence.audioProcessingMode->value->getInt (),
        .lowerBound = bounds.x,
        .upperBound = bounds.y,
        .exponent = turbulence.audioProcessingExponent->value->getFloat (),
        .firstBand = turbulence.audioProcessingFrequencyStart->value->getInt (),
        .lastBand = turbulence.audioProcessingFrequencyEnd->value->getInt (),
    };
    REQUIRE (std::abs (particleAudioResponse (left, right, settings) - std::bit_cast<float> (0x3e7ffff0u)) < 1.0e-6f);
}

TEST_CASE ("Capture cadence uses native FPS delay and relative waits after work", "[audio][capture-worker]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    REQUIRE (nativeCaptureDelay (15) == 33ms);
    REQUIRE (nativeCaptureDelay (30) == 16ms);
    REQUIRE (nativeCaptureDelay (60) == 8ms);
    REQUIRE (nativeCaptureDelay (144) == 6ms);
    REQUIRE (nativeCaptureDelay (1) == 100ms);
    REQUIRE (nativeCaptureDelay (29) == 17ms); // floor before halving
    REQUIRE (nativeCaptureDelay (0) == 100ms);
    REQUIRE (nativeCaptureDelay (-1) == 6ms);

    std::stop_source stop;
    std::chrono::milliseconds clock {0};
    std::vector<std::chrono::milliseconds> starts;
    runCapturePolling (stop.get_token (), 16ms, [&] {
        starts.push_back (clock);
        clock += starts.size () == 1 ? 250ms : 5ms;
    }, [&] (std::stop_token, auto delay) {
        clock += delay;
        if (starts.size () == 3) stop.request_stop ();
    });
    REQUIRE (starts == std::vector<std::chrono::milliseconds> {0ms, 266ms, 287ms});
}

TEST_CASE ("Capture idle and silent packet resets follow original poll boundaries", "[audio][capture-worker]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    CapturePollState poll;
    // Native compares the prior counter >1000 before adding configured Sleep.
    for (int i = 0; i < 63; ++i) {
        poll.begin ();
        REQUIRE_FALSE (poll.finish (16ms));
    }
    poll.begin ();
    REQUIRE (poll.finish (16ms));
    poll.begin ();
    poll.packet (false); // receiving any packet resets idle accumulation
    REQUIRE_FALSE (poll.finish (16ms));
    poll.begin ();
    REQUIRE_FALSE (poll.finish (16ms));
    poll.begin ();
    poll.packet (true);
    REQUIRE (poll.finish (16ms));
    poll.begin ();
    poll.packet (true);
    poll.packet (false); // last normal packet clears native silent reset
    REQUIRE_FALSE (poll.finish (16ms));
    poll.begin ();
    poll.packet (false);
    poll.packet (true);
    REQUIRE (poll.finish (16ms));
    poll.begin ();
    poll.failed ();
    poll.packet (false); // a capture API error still overrides normal data
    REQUIRE (poll.finish (16ms));

    // A 100ms cadence reaches exactly 1000 on poll10 and adds once more at
    // equality; the reset is poll12, proving the strict comparison.
    poll.reset ();
    for (int i = 0; i < 11; ++i) { poll.begin (); REQUIRE_FALSE (poll.finish (100ms)); }
    poll.begin ();
    REQUIRE (poll.finish (100ms));
}

TEST_CASE ("Capture polls retain prefixes and discard complete released packet excess", "[audio][capture-worker]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    StereoSpectrum spectrum (48000);
    StereoSpectrum reference (48000);
    CapturePollState poll;
    StereoSpectrum::Bands bands {};
    std::vector<float> first (spectrum.captureSamples () * 2, 0.0f);
    for (size_t i = 0; i < spectrum.captureSamples (); ++i)
        first[i * 2] = 0.1f * std::sin (2.0f * std::numbers::pi_v<float> * 320.0f * i / 48000.0f);
    std::vector<float> excess (first.size (), 0.75f);
    const auto* bytes = reinterpret_cast<const uint8_t*> (first.data ());
    const size_t half = spectrum.captureBytes () / 2;
    poll.begin ();
    poll.packet (false);
    spectrum.feed (bytes, half);
    REQUIRE_FALSE (poll.finish (16ms));
    REQUIRE_FALSE (spectrum.take (bands));
    // No packet poll leaves the prefix intact rather than inserting silence.
    poll.begin ();
    REQUIRE_FALSE (poll.finish (16ms));
    REQUIRE_FALSE (spectrum.take (bands));
    poll.begin ();
    poll.packet (false);
    std::vector<uint8_t> crossing (bytes + half, bytes + spectrum.captureBytes ());
    crossing.insert (crossing.end (), reinterpret_cast<const uint8_t*> (excess.data ()),
                     reinterpret_cast<const uint8_t*> (excess.data ()) + spectrum.captureBytes ());
    spectrum.feed (crossing.data (), crossing.size ());
    poll.packet (false);
    spectrum.feed (reinterpret_cast<const uint8_t*> (excess.data ()), spectrum.captureBytes ());
    REQUIRE_FALSE (poll.finish (16ms));
    REQUIRE (spectrum.take (bands));
    StereoSpectrum::Bands expected {};
    reference.feed (bytes, spectrum.captureBytes ());
    REQUIRE (reference.take (expected));
    REQUIRE (bands.audio64 == expected.audio64);
    // Excess from either packet was released; the next prefix needs all bytes.
    spectrum.feed (bytes, half);
    REQUIRE_FALSE (spectrum.take (bands));
    spectrum.discard (); // native reset discards a partial prefix and raw bank
    REQUIRE_FALSE (spectrum.take (bands));
    spectrum.feed (bytes, half);
    REQUIRE_FALSE (spectrum.take (bands));
    spectrum.feed (bytes + half, half);
    REQUIRE (spectrum.take (bands));
    REQUIRE (bands.audio64 == expected.audio64);
}

TEST_CASE ("Capture worker owns callbacks and cleans up initialization failure", "[audio][capture-worker][lifecycle]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    CaptureWorker worker (1);
    const auto caller = std::this_thread::get_id ();
    std::thread::id initialized, polled, cleaned;
    std::promise<void> firstPoll;
    worker.start ([&] (std::stop_token) { initialized = std::this_thread::get_id (); }, [&] {
        polled = std::this_thread::get_id ();
        firstPoll.set_value ();
    }, [&] { cleaned = std::this_thread::get_id (); }, [] (std::exception_ptr) {});
    REQUIRE (firstPoll.get_future ().wait_for (1s) == std::future_status::ready);
    const auto stopping = std::chrono::steady_clock::now ();
    worker.stop ();
    REQUIRE (std::chrono::steady_clock::now () - stopping < 500ms);
    REQUIRE (initialized != caller);
    REQUIRE (initialized == polled);
    REQUIRE (initialized == cleaned);
    worker.stop (); // repeated stop is harmless

    CaptureWorker failing (30);
    bool reported = false;
    bool cleanup = false;
    bool ranPoll = false;
    std::promise<void> finished;
    failing.start ([] (std::stop_token) { throw std::runtime_error ("controlled initialization failure"); },
                   [&] { ranPoll = true; }, [&] { cleanup = true; finished.set_value (); },
                   [&] (std::exception_ptr) { reported = true; });
    REQUIRE (finished.get_future ().wait_for (1s) == std::future_status::ready);
    failing.stop ();
    REQUIRE (reported);
    REQUIRE (cleanup);
    REQUIRE_FALSE (ranPoll);
}

TEST_CASE ("Capture keeps publishing without render updates and hands off untorn banks", "[audio][capture-worker][lifecycle]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    CaptureWorker worker (144);
    std::promise<void> captured;
    unsigned generation = 0;
    worker.start ([] (std::stop_token) {}, [&] {
        StereoSpectrum::Bands bands {};
        ++generation;
        for (auto& channel : bands.audio64) channel.fill (static_cast<float> (generation));
        bands = StereoSpectrum::from64 (bands.audio64[0], bands.audio64[1]);
        worker.publish (bands, generation == 1);
        if (generation == 4) captured.set_value ();
    }, [] {}, [] (std::exception_ptr) {});
    // No frame/snapshot calls occur while four capture polls complete.
    REQUIRE (captured.get_future ().wait_for (1s) == std::future_status::ready);
    bool untorn = true;
    for (int read = 0; read < 5000; ++read) {
        const auto snapshot = worker.snapshot ();
        const float value = snapshot.bands.audio64[0][0];
        for (const auto& channel : snapshot.bands.audio64)
            for (float band : channel) untorn &= band == value;
        for (float band : snapshot.bands.combined16) untorn &= band == value;
        untorn &= snapshot.resetGeneration == 1;
    }
    worker.stop ();
    REQUIRE (untorn);
    REQUIRE (worker.snapshot ().bands.audio64[0][0] >= 4.0f);
}

TEST_CASE ("Capture destruction waits for its active callback before owner cleanup", "[audio][capture-worker][lifecycle]") {
    using namespace WallpaperEngine::Audio::Drivers::Recorders;
    using namespace std::chrono_literals;
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    std::atomic_bool cleaned = false;
    auto worker = std::make_unique<CaptureWorker> (30);
    worker->start ([] (std::stop_token) {}, [&] { entered.set_value (); released.wait (); },
                  [&] { cleaned = true; }, [] (std::exception_ptr) {});
    REQUIRE (entered.get_future ().wait_for (1s) == std::future_status::ready);
    auto destroyed = std::async (std::launch::async, [&] { worker.reset (); });
    const bool stillOwned = !cleaned.load ();
    release.set_value ();
    REQUIRE (destroyed.wait_for (1s) == std::future_status::ready);
    REQUIRE (stillOwned);
    REQUIRE (cleaned.load ());
}

TEST_CASE ("Pulse capture wiring survives an absent server and rapid destruction", "[audio][capture-worker][lifecycle]") {
    using WallpaperEngine::Audio::Drivers::Recorders::PulseAudioPlaybackRecorder;
    std::optional<std::string> previous;
    if (const char* value = std::getenv ("PULSE_SERVER")) previous = value;
    struct RestoreServer {
        std::optional<std::string>& previous;
        ~RestoreServer () {
            if (previous) setenv ("PULSE_SERVER", previous->c_str (), 1);
            else unsetenv ("PULSE_SERVER");
        }
    } restore {previous};
    setenv ("PULSE_SERVER", "unix:/tmp/wallpaperengine-controlled-absent-pulse-server", 1);
    const auto start = std::chrono::steady_clock::now ();
    for (int lifetime = 0; lifetime < 10; ++lifetime) {
        PulseAudioPlaybackRecorder recorder (30);
        recorder.update ();
        REQUIRE (std::all_of (std::begin (recorder.audio64RawLeft), std::end (recorder.audio64RawLeft),
                             [] (float value) { return value == 0.0f; }));
        REQUIRE (std::all_of (std::begin (recorder.audio64RawRight), std::end (recorder.audio64RawRight),
                             [] (float value) { return value == 0.0f; }));
    }
    REQUIRE (std::chrono::steady_clock::now () - start < std::chrono::seconds (2));
}
