#pragma once

#include <map>
#include <vector>

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/Drivers/AudioDriver.h"

#include <SDL.h>

#define MAX_AUDIO_FRAME_SIZE 192000

namespace WallpaperEngine::Audio::Drivers {
/**
 * SDL mix volume for a stream: native AL_GAIN = min(1, vol^2 * gain), so the
 * clamp follows the master gain. Non-finite or non-positive products (for
 * example a zero master with an infinite vol^2) are silent; +inf saturates.
 */
inline int sdlMixVolume (int masterVolume, float mixerGain) {
    const float product = static_cast<float> (masterVolume) * mixerGain;
    if (!(product > 0.0f)) return 0;
    if (!(product < static_cast<float> (SDL_MIX_MAXVOLUME))) return SDL_MIX_MAXVOLUME;
    return static_cast<int> (product);
}

/**
 * Audio output buffers for streams being played under SDL
 */
struct SDLAudioBuffer {
    AudioStream* stream = nullptr;
    uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2] = { 0 };
    unsigned int audio_buf_size = 0;
    unsigned int audio_buf_index = 0;
};

/**
 * SDL's audio driver implementation
 */
class SDLAudioDriver final : public AudioDriver {
public:
    SDLAudioDriver (
	Application::ApplicationContext& applicationContext, Detectors::AudioPlayingDetector& detector,
	Recorders::PlaybackRecorder& recorder
    );
    ~SDLAudioDriver () override;

    /** @inheritdoc */
    int addStream (AudioStream* stream) override;
    /** @inheritdoc */
    void removeStream (int streamId) override;
    [[nodiscard]] bool isStreamFinished (int streamId) const override;
    /**
     * @return All the registered audio streams
     */
    const std::map<int, SDLAudioBuffer*>& getStreams ();

    /** @inheritdoc */
    [[nodiscard]] AVSampleFormat getFormat () const override;
    /** @inheritdoc */
    [[nodiscard]] int getSampleRate () const override;
    /** @inheritdoc */
    [[nodiscard]] int getChannels () const override;
    /**
     * @return The SDL's audio driver settings
     */
    [[nodiscard]] const SDL_AudioSpec& getSpec () const;

    [[nodiscard]] SDL_mutex* getStreamMutex () const;

private:
    /** The mutex lock used to access the stream list mutex */
    SDL_mutex* m_streamListMutex = nullptr;
    /** The last streamID used */
    int m_lastStreamID = 0;
    /** The device's ID */
    SDL_AudioDeviceID m_deviceID = 0;
    /** SDL subsystem was initialized, even if opening the device failed. */
    bool m_audioSubsystemInitialized = false;
    /** If the driver is initialized or not */
    bool m_initialized = false;
    /** The sound output configuration */
    SDL_AudioSpec m_audioSpec {};
    /** All the playable steams */
    std::map<int, SDLAudioBuffer*> m_streams {};
};
} // namespace WallpaperEngine::Audio::Drivers
