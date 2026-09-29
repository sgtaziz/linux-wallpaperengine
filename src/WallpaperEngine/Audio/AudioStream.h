#pragma once

#include <atomic>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL.h>
#include <SDL_thread.h>

#include "WallpaperEngine/Audio/AudioContext.h"

// TODO: FIND A BETTER PLACE TO DO THIS? OLD_API MIGHT EXIST BUT THIS DEFINE MIGHT NOT BE DEFINED...
#ifndef FF_API_FIFO_OLD_API
#define FF_API_FIFO_OLD_API (LIBAVUTIL_VERSION_MAJOR < 59)
#endif
#ifndef FF_API_OLD_CHANNEL_LAYOUT
#define FF_API_OLD_CHANNEL_LAYOUT (LIBAVUTIL_VERSION_MAJOR < 59)
#endif

#define MAX_QUEUE_SIZE (5 * 1024 * 1024)
#define MIN_FRAMES (25)
#define NO_AUDIO_STREAM (-1)

namespace WallpaperEngine::Audio {
class AudioContext;

using namespace WallpaperEngine::FileSystem;

/**
 * Represents a playable audio stream for the audio driver
 */
class AudioStream {
public:
    AudioStream (AudioContext& context, const std::string& filename, bool repeat = false);
    AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer, bool repeat = false);
    AudioStream (AudioContext& audioContext, AVCodecContext* context);
    ~AudioStream ();

    void queuePacket (AVPacket* pkt);
    /** Enqueue an EOF boundary so only the callback resets the decoder. */
    void queueLoopBoundary ();

    /**
     * Gets the next packet in the queue
     *
     * Returns false immediately if the reader has not queued a packet.
     */
    bool dequeuePacket ();

    /**
     * @return The audio context in use for this audio stream
     */
    [[nodiscard]] AudioContext& getAudioContext () const;

    /**
     * @return to the codec context, which provides information on the audio stream's format
     */
    [[nodiscard]] AVCodecContext* getContext () const;
    /**
     * @returns the format context, which controls how data is read off the audio stream
     */
    [[nodiscard]] AVFormatContext* getFormatContext () const;
    /**
     * @return The audio stream index of the given file
     */
    [[nodiscard]] int getAudioStream () const;
    /**
     * @return If the audio stream can be played or not
     */
    [[nodiscard]] bool isInitialized () const;
    /**
     * @param newRepeat true = repeat, false = no repeat
     */
    void setRepeat (bool newRepeat = true);
    /**
     * @return If the stream is to be repeated at the end or not
     */
    [[nodiscard]] bool isRepeat () const;
    /**
     * Stops decoding and playback of the stream
     */
    void stop ();
    /** Called by the reader when no more packets will be queued. */
    void markReaderFinished ();
    [[nodiscard]] bool hasReaderFinished () const;
    [[nodiscard]] uint64_t getCompletedLoopCount () const;
    void setVolume (float volume);
    [[nodiscard]] float getMixerGain () const;
    void setPaused (bool paused);
    [[nodiscard]] bool isPaused () const;
    [[nodiscard]] bool isPlaybackFinished () const;
    /**
     * @return The file data buffer
     */
    [[nodiscard]] ReadStreamSharedPtr& getBuffer ();
    /**
     * @return The data queue size
     */
    [[nodiscard]] size_t getQueueSize () const;
    /**
     * @return The amount of packets ready to be converted and played
     */
    [[nodiscard]] int getQueuePacketCount () const;
    /**
     * @return The duration (in seconds) of the queued data to be played
     */
    [[nodiscard]] int64_t getQueueDuration () const;
    /**
     * @return Time unit used for packet playback
     */
    [[nodiscard]] AVRational getTimeBase () const;
    /**
     * @return If the data queue is empty or not
     */
    [[nodiscard]] bool isQueueEmpty () const;
    /**
     * @return The SDL_mutex used for thread synchronization
     */
    [[nodiscard]] SDL_mutex* getMutex () const;

    /**
     * Reads a frame from the audio stream, resamples it to the driver's settings
     * and returns the data ready to be played
     *
     * @param audioBuffer
     * @param bufferSize
     *
     * @return The amount of bytes available or < 0 for error
     */
    int decodeFrame (uint8_t* audioBuffer, int bufferSize);

private:
    /**
     * Initializes ffmpeg to read the given file
     *
     * @param filename
     */
    void loadCustomContent (const char* filename = nullptr);
    /**
     * Converts the audio frame from the original format to one supported by the audio driver
     *
     * @param out_buf
     * @return
     */
    int resampleAudio (uint8_t* out_buf, int out_size, bool drain = false);
    /**
     * Queues a packet into the play queue
     *
     * @param pkt
     * @return
     */
    bool doQueue (AVPacket* pkt);
    /**
     * Initializes queues and ffmpeg resampling
     */
    void initialize ();

    /** The SwrContext that handles resampling */
    SwrContext* m_swrctx = nullptr;
    /** The audio context this stream will be played under */
    AudioContext& m_audioContext;
    /** If this stream was properly initialized or not */
    std::atomic_bool m_initialized { false };
    /** Repeat enabled? */
    std::atomic_bool m_repeat { false };
    /** The reader has reached the end of a non-repeating input. */
    std::atomic_bool m_readerFinished { false };
    /** The callback has submitted the decoder drain packet. */
    bool m_decoderDraining = false;
    bool m_loopBoundaryPending = false;
    std::atomic_uint64_t m_completedLoops { 0 };
    std::atomic<float> m_volume { 1.0f };
    std::atomic_bool m_paused { false };
    std::atomic_bool m_playbackFinished { false };
    /** The codec context that contains the original audio format information */
    AVCodecContext* m_context = nullptr;
    /** The format context that controls how data is read off the file */
    AVFormatContext* m_formatContext = nullptr;
    /** The stream index for the audio being played */
    int m_audioStream = NO_AUDIO_STREAM;
    /** File data pointer */
    ReadStreamSharedPtr m_buffer = nullptr;

    struct MyAVPacketList {
	AVPacket* packet;
    };

    /** The packet used while decoding this stream */
    AVPacket* m_decodePacket = nullptr;
    /** The AV frame used while decoding this stream */
    AVFrame* m_decodeFrame = nullptr;
    /** Converted frame bytes left over when the driver's buffer is smaller. */
    std::vector<uint8_t> m_pendingOutput;
    size_t m_pendingOutputOffset = 0;

    /**
     * Packet queue information
     */
    struct PacketQueue {
#if FF_API_FIFO_OLD_API
	AVFifoBuffer* packetList = nullptr;
#else
	AVFifo* packetList = nullptr;
#endif
	int nb_packets = 0;
	size_t size = 0;
	int64_t duration = 0;
	SDL_mutex* mutex = nullptr;
    }* m_queue {};

    SDL_Thread* m_audioThread = nullptr;
};
} // namespace WallpaperEngine::Audio
