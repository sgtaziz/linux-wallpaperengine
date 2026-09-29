#include "AudioStream.h"
#include "WallpaperEngine/Logging/Log.h"
#include <cassert>
#include <cmath>
#include <iostream>

// maximum size of the queue to prevent reading too much data

using namespace WallpaperEngine::Audio;

int audio_read_thread (void* arg) {
    auto* stream = static_cast<AudioStream*> (arg);
    AVPacket* packet = av_packet_alloc ();
    if (packet == nullptr) {
	stream->markReaderFinished ();
	return AVERROR (ENOMEM);
    }
    int ret = 0;
    bool queuedSinceSeek = false;

    while (ret >= 0 && stream->getAudioContext ().getApplicationContext ().state.general.keepRunning
	   && stream->isInitialized ()) {
	// give the cpu some time to play the queued frames if there's enough info there
	if (stream->getQueueSize () >= MAX_QUEUE_SIZE
	    || (stream->getQueuePacketCount () > MIN_FRAMES
		&& (av_q2d (stream->getTimeBase ()) * stream->getQueueDuration () > 1.0))) {
	    SDL_Delay (10);
	    continue;
	}

	ret = av_read_frame (stream->getFormatContext (), packet);

	if (ret == AVERROR_EOF) {
	    if (stream->isRepeat () && queuedSinceSeek
	        && avformat_seek_file (stream->getFormatContext (), stream->getAudioStream (), 0, 0, 0,
	                               ~AVSEEK_FLAG_FRAME) >= 0) {
	        // The marker orders decoder draining and reset after all packets
	        // from this pass; the reader never touches the codec context.
	        stream->queueLoopBoundary ();
	        queuedSinceSeek = false;
	        ret = 0;
	        continue;
	    }
	    break;
	}
	if (ret < 0) break;

	// TODO: PROPERLY IMPLEMENT THIS
	if (packet->stream_index == stream->getAudioStream ()) {
	    stream->queuePacket (packet);
	    queuedSinceSeek = true;
	} else {
	    av_packet_unref (packet);
	}
    }

    av_packet_free (&packet);
    stream->markReaderFinished ();

    return 0;
}

static int audio_read_data_callback (void* streamarg, uint8_t* buffer, int buffer_size) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    // check if we're at eof and return the right value
    if (stream->getBuffer ()->eof ()) {
	return AVERROR_EOF;
    }

    stream->getBuffer ()->read (reinterpret_cast<std::istream::char_type*> (buffer), buffer_size);

    if (stream->getBuffer ()->fail () && !stream->getBuffer ()->eof ()) {
	return AVERROR_INVALIDDATA;
    }

    // return read bytes only
    return stream->getBuffer ()->gcount ();
}

int64_t audio_seek_data_callback (void* streamarg, int64_t offset, int whence) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    // reset error state
    stream->getBuffer ()->clear ();

    if (whence & AVSEEK_SIZE) {
	const auto current = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (0, std::ios_base::end);
	const auto end = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (current, std::ios_base::beg);
	return end;
    }

    switch (whence) {
	case SEEK_CUR:
	    stream->getBuffer ()->seekg (offset, std::ios_base::cur);
	    break;
	case SEEK_SET:
	    stream->getBuffer ()->seekg (offset, std::ios_base::beg);
	    break;
	case SEEK_END:
	    stream->getBuffer ()->seekg (offset, std::ios_base::end);
	    break;
    }

    return 0;
}

AudioStream::AudioStream (AudioContext& context, const std::string& filename, bool repeat) :
    m_audioContext (context), m_repeat (repeat) {
    this->loadCustomContent (filename.c_str ());
}

AudioStream::AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer, bool repeat) :
    m_audioContext (context), m_repeat (repeat) {
    // setup a custom context first
    this->m_formatContext = avformat_alloc_context ();

    if (this->m_formatContext == nullptr) {
	sLog.exception ("Cannot allocate ffmpeg format context");
    }

    this->m_buffer = buffer;

    // setup custom io for it
    this->m_formatContext->pb = avio_alloc_context (
	static_cast<uint8_t*> (av_malloc (4096)), 4096, 0, this, &audio_read_data_callback, nullptr,
	&audio_seek_data_callback
    );

    if (this->m_formatContext->pb == nullptr) {
	sLog.exception ("Cannot create avio context");
    }

    // continue the normal load procedure
    this->loadCustomContent ();
}

AudioStream::AudioStream (AudioContext& audioContext, AVCodecContext* context) :
    m_audioContext (audioContext), m_context (context), m_queue (new PacketQueue) {
    this->initialize ();
}

AudioStream::~AudioStream () {
    // stop the audio
    this->stop ();

    if (this->m_queue != nullptr) {
	// The driver removes this stream before destruction, so no callback can
	// access the queue. The read thread has now joined; nothing remains to
	// wait for here.
	if (this->m_queue->packetList != nullptr) {
	    MyAVPacketList queued {};
#if FF_API_FIFO_OLD_API
	    while (av_fifo_size (this->m_queue->packetList) >= static_cast<int> (sizeof (queued))) {
		if (av_fifo_generic_read (this->m_queue->packetList, &queued, sizeof (queued), nullptr) < 0) {
		    break;
		}
		av_packet_free (&queued.packet);
	    }
#else
	    while (av_fifo_read (this->m_queue->packetList, &queued, 1) >= 0) {
		av_packet_free (&queued.packet);
	    }
#endif
	}
    }

    if (this->m_swrctx != nullptr && swr_is_initialized (this->m_swrctx) == true) {
	swr_close (this->m_swrctx);
    }
    if (this->m_swrctx != nullptr) {
	swr_free (&this->m_swrctx);
    }
    if (this->m_decodePacket != nullptr) {
	av_packet_free (&this->m_decodePacket);
    }
    if (this->m_decodeFrame != nullptr) {
	av_frame_free (&this->m_decodeFrame);
    }
    if (this->m_queue != nullptr && this->m_queue->packetList != nullptr) {
#if FF_API_FIFO_OLD_API
	av_fifo_free (this->m_queue->packetList);
	this->m_queue->packetList = nullptr;
#else
	av_fifo_freep2 (&this->m_queue->packetList);
#endif /* FF_API_FIFO_OLD_API */
    }

    if (this->m_queue != nullptr) {
	if (this->m_queue->mutex != nullptr) SDL_DestroyMutex (this->m_queue->mutex);
	delete this->m_queue;
    }

    if (this->m_formatContext != nullptr) {
	avformat_free_context (this->m_formatContext);
    }

    if (this->m_context != nullptr) {
	avcodec_free_context (&this->m_context);
    }
}

void AudioStream::loadCustomContent (const char* filename) {
    if (avformat_open_input (&this->m_formatContext, filename, nullptr, nullptr) != 0) {
	sLog.exception ("Cannot open audio file: ", filename);
    }
    if (avformat_find_stream_info (this->m_formatContext, nullptr) < 0) {
	sLog.exception ("Cannot determine file format: ", filename);
    }

    // find the audio stream
    for (unsigned int i = 0; i < this->m_formatContext->nb_streams; i++) {
	if (this->m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
	    && this->m_audioStream == NO_AUDIO_STREAM) {
	    this->m_audioStream = i;
	}
    }

    if (this->m_audioStream == NO_AUDIO_STREAM) {
	sLog.exception ("Cannot find an audio stream in file ", filename);
    }

    // get the decoder for it and alloc the required context
    const AVCodec* aCodec
	= avcodec_find_decoder (this->m_formatContext->streams[this->m_audioStream]->codecpar->codec_id);

    if (aCodec == nullptr) {
	sLog.exception ("Cannot initialize audio decoder for file: ", filename);
    }

    // alocate context
    AVCodecContext* avCodecContext = avcodec_alloc_context3 (aCodec);

    if (avcodec_parameters_to_context (avCodecContext, this->m_formatContext->streams[this->m_audioStream]->codecpar)
	!= 0) {
	sLog.exception ("Cannot initialize audio decoder parameters");
    }

    // finally open
    avcodec_open2 (avCodecContext, aCodec, nullptr);

    // initialize default data
    this->m_context = avCodecContext;
    this->m_queue = new PacketQueue;

    this->initialize ();

    // initialize an SDL thread to read the file
    this->m_audioThread = SDL_CreateThread (audio_read_thread, filename, this);
}

void AudioStream::initialize () {
// allocate the FIFO buffer
#if FF_API_FIFO_OLD_API
    this->m_queue->packetList = av_fifo_alloc (sizeof (MyAVPacketList));
#else
    this->m_queue->packetList = av_fifo_alloc2 (1, sizeof (MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
#endif

#if FF_API_OLD_CHANNEL_LAYOUT
    int64_t out_channel_layout;

    // set output audio channels based on the input audio channels
    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_layout = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_layout = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_layout = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    // initialize swrctx
    this->m_swrctx = swr_alloc_set_opts (
	nullptr, out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	this->getContext ()->channel_layout, this->getContext ()->sample_fmt, this->getContext ()->sample_rate, 0,
	nullptr
    );
#else
    AVChannelLayout out_channel_layout;
    int64_t out_channel_mask;

    // set output audio channels based on the input audio channels
    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_mask = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_mask = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_mask = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    if (av_channel_layout_from_mask (&out_channel_layout, out_channel_mask) != 0) {
	sLog.exception ("Cannot get channel layout from mask");
    }

    swr_alloc_set_opts2 (
	&this->m_swrctx, &out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	&this->m_context->ch_layout, this->m_context->sample_fmt, this->m_context->sample_rate, 0, nullptr
    );
#endif

    if (this->m_swrctx == nullptr) {
	sLog.exception ("Cannot initialize swrctx for audio resampling");
    }

    // initialize the context
    if (swr_init (this->m_swrctx) < 0) {
	sLog.exception ("Failed to initialize the resampling context.");
    }

    // setup the queue information
    this->m_queue->mutex = SDL_CreateMutex ();

    this->m_decodeFrame = av_frame_alloc ();
    this->m_decodePacket = av_packet_alloc ();

    if (!this->m_decodeFrame) {
	sLog.exception ("Could not allocate AVFrame.\n");
    }
    if (!this->m_decodePacket) {
	sLog.exception ("Could not allocate AVPacket.\n");
    }

    this->m_initialized = true;
}

void AudioStream::queuePacket (AVPacket* pkt) {
    // clone the packet
    AVPacket* clone = av_packet_alloc ();

    if (clone == nullptr) {
	av_packet_unref (clone);
	return;
    }

    av_packet_move_ref (clone, pkt);

    SDL_LockMutex (this->m_queue->mutex);
    const bool gotQueued = this->doQueue (clone);
    SDL_UnlockMutex (this->m_queue->mutex);

    if (!gotQueued) {
	av_packet_free (&clone);
    }
}

void AudioStream::queueLoopBoundary () {
    SDL_LockMutex (this->m_queue->mutex);
    this->doQueue (nullptr);
    SDL_UnlockMutex (this->m_queue->mutex);
}

bool AudioStream::doQueue (AVPacket* pkt) {
    MyAVPacketList entry { pkt };

#if FF_API_FIFO_OLD_API
    if (av_fifo_space (this->m_queue->packetList) < static_cast<int> (sizeof (entry))) {
	if (av_fifo_grow (this->m_queue->packetList, sizeof (entry)) < 0) {
	    return false;
	}
    }

    av_fifo_generic_write (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
#else
    // write the entry if possible
    if (av_fifo_write (this->m_queue->packetList, &entry, 1) < 0) {
	return false;
    }
#endif

    this->m_queue->nb_packets++;
    this->m_queue->size += (entry.packet ? entry.packet->size : 0) + sizeof (entry);
    if (entry.packet) this->m_queue->duration += entry.packet->duration;

    return true;
}

bool AudioStream::dequeuePacket () {
    MyAVPacketList entry {};

    SDL_LockMutex (this->m_queue->mutex);

#if FF_API_FIFO_OLD_API
	int ret = -1;

	if (av_fifo_size (this->m_queue->packetList) >= static_cast<int> (sizeof (entry))) {
	    ret = av_fifo_generic_read (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
	}
#else
	const int ret = av_fifo_read (this->m_queue->packetList, &entry, 1);
#endif

	if (ret >= 0) {
	    this->m_queue->nb_packets--;
	    this->m_queue->size -= (entry.packet ? entry.packet->size : 0) + sizeof (entry);
	    if (entry.packet) this->m_queue->duration -= entry.packet->duration;

	    if (entry.packet) {
		av_packet_move_ref (this->m_decodePacket, entry.packet);
		av_packet_free (&entry.packet);
	    } else {
		this->m_loopBoundaryPending = true;
	    }
	}

    SDL_UnlockMutex (this->m_queue->mutex);
    return ret >= 0;
}

AVCodecContext* AudioStream::getContext () const { return this->m_context; }

AVFormatContext* AudioStream::getFormatContext () const { return this->m_formatContext; }

int AudioStream::getAudioStream () const { return this->m_audioStream; }

bool AudioStream::isInitialized () const { return this->m_initialized; }

void AudioStream::setRepeat (const bool newRepeat) { this->m_repeat = newRepeat; }

bool AudioStream::isRepeat () const { return this->m_repeat; }

ReadStreamSharedPtr& AudioStream::getBuffer () { return this->m_buffer; }

size_t AudioStream::getQueueSize () const {
    SDL_LockMutex (this->m_queue->mutex);
    const auto result = this->m_queue->size;
    SDL_UnlockMutex (this->m_queue->mutex);
    return result;
}

int AudioStream::getQueuePacketCount () const {
    SDL_LockMutex (this->m_queue->mutex);
    const auto result = this->m_queue->nb_packets;
    SDL_UnlockMutex (this->m_queue->mutex);
    return result;
}

AVRational AudioStream::getTimeBase () const {
    if (this->m_audioStream == NO_AUDIO_STREAM) {
	return { 0, 0 };
    }

    return this->m_formatContext->streams[this->m_audioStream]->time_base;
}

int64_t AudioStream::getQueueDuration () const {
    SDL_LockMutex (this->m_queue->mutex);
    const auto result = this->m_queue->duration;
    SDL_UnlockMutex (this->m_queue->mutex);
    return result;
}

bool AudioStream::isQueueEmpty () const { return this->getQueuePacketCount () == 0; }

SDL_mutex* AudioStream::getMutex () const { return this->m_queue->mutex; }

void AudioStream::stop () {
    this->m_initialized = false;
    if (this->m_audioThread != nullptr) {
	SDL_WaitThread (this->m_audioThread, nullptr);
	this->m_audioThread = nullptr;
    }
}

void AudioStream::markReaderFinished () { this->m_readerFinished = true; }

bool AudioStream::hasReaderFinished () const { return this->m_readerFinished; }

uint64_t AudioStream::getCompletedLoopCount () const { return this->m_completedLoops; }

void AudioStream::setVolume (float volume) {
    this->m_volume = std::isfinite (volume) ? std::clamp (volume, 0.0f, 1.0f) : 0.0f;
}

float AudioStream::getMixerGain () const {
    const float volume = this->m_volume;
    return volume * volume;
}

void AudioStream::setPaused (bool paused) { this->m_paused = paused; }

bool AudioStream::isPaused () const { return this->m_paused; }

bool AudioStream::isPlaybackFinished () const { return this->m_playbackFinished; }

int AudioStream::resampleAudio (uint8_t* out_buf, const int out_size, bool drain) {
    const int inputSamples = drain ? 0 : this->m_decodeFrame->nb_samples;
    const int inputRate = this->m_context->sample_rate;
    const int outputChannels = this->m_audioContext.getChannels ();
    if ((!drain && inputSamples <= 0) || inputRate <= 0 || outputChannels <= 0 || out_size <= 0) return -1;

    const int outputSamples = av_rescale_rnd (
	swr_get_delay (this->m_swrctx, inputRate) + inputSamples,
	this->m_audioContext.getSampleRate (), inputRate, AV_ROUND_UP
    );
    if (outputSamples <= 0) return -1;

    uint8_t** converted = nullptr;
    int lineSize = 0;
    const int allocated = av_samples_alloc_array_and_samples (
	&converted, &lineSize, outputChannels, outputSamples, this->m_audioContext.getFormat (), 0
    );
    if (allocated < 0) return -1;

    const int produced = swr_convert (
	this->m_swrctx, converted, outputSamples,
	drain ? nullptr : const_cast<const uint8_t**> (this->m_decodeFrame->data), inputSamples
    );
    const int bytes = produced < 0 ? produced : av_samples_get_buffer_size (
	&lineSize, outputChannels, produced, this->m_audioContext.getFormat (), 1
    );
    if (bytes > 0) {
	const int copied = std::min (bytes, out_size);
	memcpy (out_buf, converted[0], copied);
	if (copied < bytes) {
	    this->m_pendingOutput.assign (converted[0] + copied, converted[0] + bytes);
	    this->m_pendingOutputOffset = 0;
	}
    }

    av_freep (&converted[0]);
    av_freep (&converted);
    return bytes > 0 ? std::min (bytes, out_size) : -1;
}

int AudioStream::decodeFrame (uint8_t* audioBuffer, const int bufferSize) {
    // The audio callback must never wait for the asynchronous read thread
    // while holding the driver's stream-list mutex.
    while (this->isInitialized () && this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	if (this->m_pendingOutputOffset < this->m_pendingOutput.size ()) {
	    const int copied = std::min<size_t> (bufferSize, this->m_pendingOutput.size () - this->m_pendingOutputOffset);
	    memcpy (audioBuffer, this->m_pendingOutput.data () + this->m_pendingOutputOffset, copied);
	    this->m_pendingOutputOffset += copied;
	    if (this->m_pendingOutputOffset == this->m_pendingOutput.size ()) {
		this->m_pendingOutput.clear ();
		this->m_pendingOutputOffset = 0;
	    }
	    return copied;
	}
	const int received = avcodec_receive_frame (this->m_context, this->m_decodeFrame);
	if (received == 0) return this->resampleAudio (audioBuffer, bufferSize);
	if (received == AVERROR_EOF) {
	    const int tailBytes = this->resampleAudio (audioBuffer, bufferSize, true);
	    if (tailBytes > 0) return tailBytes;
	    if (!this->m_loopBoundaryPending) {
		this->m_playbackFinished = true;
		return -1;
	    }
	    avcodec_flush_buffers (this->m_context);
	    swr_close (this->m_swrctx);
	    if (swr_init (this->m_swrctx) < 0) return -1;
	    this->m_decoderDraining = false;
	    this->m_loopBoundaryPending = false;
	    ++this->m_completedLoops;
	    continue;
	}
	if (received != AVERROR (EAGAIN)) return -1;

	if (this->dequeuePacket ()) {
	    if (this->m_loopBoundaryPending) {
		this->m_decoderDraining = true;
		if (avcodec_send_packet (this->m_context, nullptr) < 0) return -1;
		continue;
	    }
	    const int sent = avcodec_send_packet (this->m_context, this->m_decodePacket);
	    av_packet_unref (this->m_decodePacket);
	    if (sent < 0) return -1;
	    continue;
	}

	if (!this->m_readerFinished || this->m_decoderDraining) return -1;
	this->m_decoderDraining = true;
	if (avcodec_send_packet (this->m_context, nullptr) < 0) return -1;
    }

    return 0;
}

AudioContext& AudioStream::getAudioContext () const { return this->m_audioContext; }
