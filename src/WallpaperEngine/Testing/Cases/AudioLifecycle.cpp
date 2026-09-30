#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/ParticleAudioResponse.h"
#include "WallpaperEngine/Audio/SoundPlaybackPolicy.h"
#include "WallpaperEngine/Audio/Drivers/Detectors/AudioPlayingDetector.h"
#include "WallpaperEngine/Audio/Drivers/Recorders/PlaybackRecorder.h"
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

    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    Policy policy ("pool", false, 0.0f, 0.0f);
    policy.clipStarted ();
    policy.pause ();
    REQUIRE_FALSE (policy.canConsumeCompletion ());
    REQUIRE (policy.state () == Policy::State::Paused);
    policy.resume ();
    REQUIRE (policy.canConsumeCompletion ());
    policy.clipFinished (0.0f);
    REQUIRE (policy.ready ());
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
    REQUIRE (sound->attenuation == 0.75f);
    REQUIRE (sound->minDistance == 12.0f);

    auto single = data;
    single["sound"] = "one.wav";
    const auto one = ObjectParser::parse (single, project);
    const auto* oneSound = dynamic_cast<const Sound*> (one.get ());
    REQUIRE (oneSound != nullptr);
    REQUIRE (oneSound->sounds == std::vector<std::string> { "one.wav" });
}

TEST_CASE ("Sound playback modes preserve start and interval gates", "[audio][schedule]") {
    using Policy = WallpaperEngine::Audio::SoundPlaybackPolicy;
    Policy pool ("pool", false, 1.0f, 3.0f);
    REQUIRE (pool.ready ());
    pool.pause ();
    REQUIRE_FALSE (pool.ready ());
    pool.resume ();
    REQUIRE (pool.ready ());
    pool.clipStarted ();
    REQUIRE (pool.state () == Policy::State::Playing);
    pool.clipFinished (0.5f);
    REQUIRE (pool.ready ());

    Policy random ("random", false, 1.0f, 3.0f);
    random.clipStarted ();
    random.clipFinished (0.5f);
    REQUIRE (random.state () == Policy::State::Waiting);
    random.advance (0.75f);
    random.pause ();
    random.advance (10.0f);
    REQUIRE (random.state () == Policy::State::PausedWaiting);
    random.resume ();
    random.advance (1.0f);
    REQUIRE_FALSE (random.ready ());
    random.advance (0.25f);
    REQUIRE (random.ready ());

    Policy single ("single", false, 0.0f, 0.0f);
    single.clipStarted ();
    single.clipFinished (0.0f);
    REQUIRE (single.state () == Policy::State::Stopped);
    single.play ();
    REQUIRE (single.ready ());

    Policy silent ("pool", true, 0.0f, 0.0f);
    REQUIRE (silent.state () == Policy::State::Stopped);
    silent.play ();
    REQUIRE (silent.ready ());
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
    const bool invalidOnly = WallpaperEngine::Audio::startOneOf (2, failed, rng, tryStart);
    const bool reachedValid = WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart);
    const bool failedOnlyOnce = attempts == 3 && failed.contains (0) && failed.contains (1);
    const auto oldAttempts = attempts;
    const bool stillValid = WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart);
    std::filesystem::remove (invalid);
    REQUIRE_FALSE (invalidOnly);
    REQUIRE (reachedValid);
    REQUIRE (failedOnlyOnce);
    REQUIRE (stillValid);
    REQUIRE (attempts == oldAttempts + 1);

    failed.insert (2);
    const auto beforeExhausted = attempts;
    REQUIRE_FALSE (WallpaperEngine::Audio::startOneOf (paths.size (), failed, rng, tryStart));
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
    // Literal results of divss/mulss/subss/cvttss2si at 1400d1491–14a0.
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
    NativeSpectrumSmoother firstScene;
    StereoSpectrum::Bands raw {};
    raw.audio64[0][0] = 1.0f;
    auto first = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (first.audio64[0][0] - 0.2f) < 0.000001f);
    REQUIRE (first.audio64[1][0] == 0.0f);
    REQUIRE (std::abs (first.combined32[0] - 0.1f) < 0.000001f);

    raw.audio64[0][0] = 0.0f;
    raw.audio64[0][1] = 1.0f;
    const auto crossed = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (crossed.audio64[0][0] - 0.16f) < 0.000001f);
    REQUIRE (std::abs (crossed.audio64[0][1] - 0.2f) < 0.000001f);
    REQUIRE (std::abs (crossed.audio32[0][0] - 0.2f) < 0.000001f);
    REQUIRE (std::abs (crossed.combined32[0] - 0.1f) < 0.000001f);

    NativeSpectrumSmoother secondScene;
    StereoSpectrum::Bands otherRaw {};
    otherRaw.audio64[1][7] = 1.0f;
    const auto second = secondScene.update (otherRaw, 0.01f, 0.5f, 1.0f);
    REQUIRE (second.audio64[0][0] == 0.0f);
    REQUIRE (std::abs (second.audio64[1][7] - 0.1f) < 0.000001f);
    const auto silent = firstScene.update (StereoSpectrum::Bands {}, 0.01f, 1.0f, 1.0f);
    REQUIRE (silent.audio64[0][0] == 0.0f);
    REQUIRE (silent.audio64[0][1] == 0.0f);
    const auto resumed = firstScene.update (raw, 0.01f, 1.0f, 1.0f);
    REQUIRE (std::abs (resumed.audio64[0][1] - 0.36f) < 0.000001f);

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

TEST_CASE ("Scene spectrum handoff retains stable shader and particle arrays across viewports", "[audio][spectrum]") {
    using WallpaperEngine::Audio::Drivers::Recorders::SceneSpectrumState;
    using WallpaperEngine::Audio::Drivers::Recorders::StereoSpectrum;
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
    REQUIRE (std::abs (shaderLeft[0] - 0.2f) < 0.000001f);
    REQUIRE (std::abs (shaderRight[7] - 0.1f) < 0.000001f);

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
    REQUIRE (std::abs (firstScene.bands ().audio64[0][0] - 0.2f) < 0.000001f);
    REQUIRE (std::abs (secondScene.bands ().audio64[1][7] - 0.1f) < 0.000001f);
    REQUIRE (firstScene.advance (StereoSpectrum::Bands {}, 2,
                                 firstFrame + std::chrono::milliseconds (10), 1.0f, 1.0f));
    REQUIRE (firstScene.bands ().audio64[0][0] == 0.0f);
    REQUIRE (std::abs (secondScene.bands ().audio64[1][7] - 0.1f) < 0.000001f);
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
