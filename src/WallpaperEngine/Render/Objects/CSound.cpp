#include <SDL.h>

#include "CSound.h"

#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Render::Objects;

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) :
    CObject (scene, sound), ScriptableObject (scene, sound), m_sound (sound),
    m_policy (sound.playbackmode.value_or ("pool"), sound.startSilent, sound.minTime, sound.maxTime) {
    if (sound.volume) this->registerProperty ("volume", *sound.volume->value);
    if (this->getContext ().getApp ().getContext ().settings.audio.enabled && m_policy.ready ())
	this->startSelectedClip ();
}

CSound::~CSound () { this->releaseActiveClip (); }

void CSound::releaseActiveClip () {
    if (!m_stream) return;
    m_stream->stop ();
    if (m_streamId) this->getScene ().getAudioContext ().removeStream (*m_streamId);
    m_streamId.reset ();
    m_stream.reset ();
}

void CSound::startSelectedClip () {
    if (!m_policy.ready () || m_sound.sounds.empty ()) return;
    const bool started = Audio::startOneOf (m_sound.sounds.size (), m_unavailableClips, m_rng, [&](size_t index) {
	try {
	    auto stream = std::make_unique<Audio::AudioStream> (
		this->getScene ().getAudioContext (), this->getAssetLocator ().read (m_sound.sounds[index])
	    );
	    if (!stream->isInitialized ()) {
		sLog.error ("Sound layer ", this->getId (), " cannot initialize decoder for ", m_sound.sounds[index]);
		return false;
	    }
	    if (m_sound.volume) stream->setVolume (m_sound.volume->value->getFloat ());
	    const int id = this->getScene ().getAudioContext ().addStream (stream.get ());
	    m_streamId = id;
	    m_stream = std::move (stream);
	    return true;
	} catch (const std::exception& error) {
	    sLog.error ("Sound layer ", this->getId (), " cannot play ", m_sound.sounds[index], ": ", error.what ());
	    return false;
	}
    });
    if (started) m_policy.clipStarted ();
    else m_policy.stop ();
}

void CSound::tick (float sceneDelta) {
    if (!this->getContext ().getApp ().getContext ().settings.audio.enabled) return;
    if (m_stream && m_sound.volume) m_stream->setVolume (m_sound.volume->value->getFloat ());
    m_policy.advance (sceneDelta);
    if (m_streamId && m_policy.canConsumeCompletion ()
	&& this->getScene ().getAudioContext ().isStreamFinished (*m_streamId)) {
	this->releaseActiveClip ();
	const float randomUnit = std::generate_canonical<float, 24> (m_rng);
	m_policy.clipFinished (randomUnit);
    }
    if (m_policy.ready ()) this->startSelectedClip ();
}

void CSound::play () {
    if (!this->getContext ().getApp ().getContext ().settings.audio.enabled) return;
    // SceneScript runs before the sound tick. A clip can finish between ticks;
    // a user play at that boundary must not be mistaken for repeated play.
    if (m_streamId && m_policy.canConsumeCompletion ()
	&& this->getScene ().getAudioContext ().isStreamFinished (*m_streamId)) {
	this->releaseActiveClip ();
	m_policy.stop ();
    }
    // Native play dispatch resumes a paused layer. A random-mode interval has
    // no stream, but its remaining wait must survive that resume as well.
    if (m_policy.state () == Audio::SoundPlaybackPolicy::State::PausedWaiting) {
        m_policy.resume ();
        return;
    }
    if (m_policy.state () == Audio::SoundPlaybackPolicy::State::PausedReady) {
        m_policy.resume ();
        if (m_policy.ready ()) this->startSelectedClip ();
        return;
    }
    // Leave an already running stream in place.
    if (m_stream && m_policy.state () == Audio::SoundPlaybackPolicy::State::Playing) return;
    if (m_stream && m_policy.state () == Audio::SoundPlaybackPolicy::State::Paused) {
        m_policy.resume ();
        m_stream->setPaused (false);
        return;
    }
    this->releaseActiveClip ();
    m_policy.play ();
    this->startSelectedClip ();
}
void CSound::pause () {
    m_policy.pause ();
    if (m_stream) m_stream->setPaused (true);
}
void CSound::resume () {
    m_policy.resume ();
    if (m_stream) m_stream->setPaused (false);
}
void CSound::stop () {
    this->releaseActiveClip ();
    m_policy.stop ();
}
bool CSound::isPlaying () const {
    return m_stream && m_streamId && m_policy.state () == Audio::SoundPlaybackPolicy::State::Playing
	&& !this->getScene ().getAudioContext ().isStreamFinished (*m_streamId);
}

void CSound::render () { }
