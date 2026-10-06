#include <SDL.h>

#include "CSound.h"

#include "WallpaperEngine/Audio/SoundSpatialization.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <random>

using namespace WallpaperEngine::Render::Objects;

CSound::CSound (Wallpapers::CScene& scene, const Sound& sound) :
    CObject (scene, sound), ScriptableObject (scene, sound), m_sound (sound),
    m_controller (
	Audio::SoundPlaybackPolicy (sound.playbackmode.value_or ("loop"), sound.startSilent, sound.minTime,
	                            sound.maxTime, sound.sounds.size ()),
	sound.sounds.size (), *this, std::random_device {} ()
    ) {
    for (const auto& binding : Scripting::scriptPropertyBindings (sound))
	this->registerProperty (binding.name, binding.value);
    this->latchDistance ();
    if (this->audioEnabled ()) m_controller.initialize (this->currentVolume ());
}

CSound::~CSound () { this->releaseClip (); }

bool CSound::audioEnabled () const { return this->getContext ().getApp ().getContext ().settings.audio.enabled; }

void CSound::latchDistance () {
    m_distance.play (m_sound.attenuation->value->getFloat (), m_sound.minDistance->value->getFloat ());
}

float CSound::currentVolume () const { return m_sound.volume ? m_sound.volume->value->getFloat () : 1.0f; }

void CSound::releaseClip () {
    if (!m_stream) return;
    m_stream->stop ();
    if (m_streamId) this->getScene ().getAudioContext ().removeStream (*m_streamId);
    m_streamId.reset ();
    m_stream.reset ();
}

bool CSound::startClip (size_t index, bool repeat, float volume) {
    this->releaseClip ();
    try {
	auto stream = std::make_unique<Audio::AudioStream> (
	    this->getScene ().getAudioContext (), this->getAssetLocator ().read (m_sound.sounds[index]), repeat
	);
	if (!stream->isInitialized ()) {
	    sLog.error ("Sound layer ", this->getId (), " cannot initialize decoder for ", m_sound.sounds[index]);
	    return false;
	}
	stream->setVolume (volume);
	this->updateMonoRouting (*stream);
	m_streamId = this->getScene ().getAudioContext ().addStream (stream.get ());
	m_stream = std::move (stream);
	return true;
    } catch (const std::exception& error) {
	sLog.error ("Sound layer ", this->getId (), " cannot play ", m_sound.sounds[index], ": ", error.what ());
	return false;
    }
}

bool CSound::hasClip () const { return m_stream != nullptr; }

bool CSound::clipFinished () const {
    return !m_streamId || this->getScene ().getAudioContext ().isStreamFinished (*m_streamId);
}

void CSound::setClipPaused (bool paused) {
    if (m_stream) m_stream->setPaused (paused);
}

void CSound::setClipVolume (float volume) {
    if (m_stream) m_stream->setVolume (volume);
}

std::optional<float> CSound::clipDuration () const {
    return m_stream ? m_stream->getDuration () : std::nullopt;
}

void CSound::updateMonoRouting (Audio::AudioStream& stream) const {
    if (!stream.isMonoSource ()) return;
    if (!m_sound.spatialization) {
	stream.setMonoChannelGains (Audio::kNonSpatialMonoGains.left, Audio::kNonSpatialMonoGains.right);
	return;
    }
    glm::vec3 origin;
    try {
	origin = Wallpapers::resolveSceneTransform (m_sound, [this] (int parentId) -> const Object* {
	    const auto* parent = this->getScene ().getObject (parentId);
	    return parent ? &parent->getObject () : nullptr;
	}).origin;
    } catch (const std::exception&) {
	origin = m_sound.origin->value->getVec3 ();
    }
    const auto& camera = this->getScene ().getCamera ();
    const auto listener = Audio::soundListenerForCamera (
	camera.getEye (), camera.getCenter (), camera.getUp (), camera.isOrthogonal (), camera.getWidth (),
	camera.getHeight ()
    );
    const auto gains = Audio::spatialMonoGains (origin, listener, m_distance.minDistance, m_distance.attenuation);
    stream.setMonoChannelGains (gains.left, gains.right);
}

void CSound::tick (float sceneDelta) {
    if (!this->audioEnabled ()) return;
    m_controller.tick (sceneDelta, this->currentVolume ());
    // Native moves spatialized voices every frame.
    if (m_stream && m_sound.spatialization) this->updateMonoRouting (*m_stream);
}

void CSound::play () {
    this->latchDistance ();
    if (this->audioEnabled ()) m_controller.play (this->currentVolume ());
}

void CSound::pause () { m_controller.pause (); }

void CSound::resume () {
    if (this->audioEnabled ()) m_controller.resume (this->currentVolume ());
}

void CSound::stop () { m_controller.stop (); }

bool CSound::isPlaying () const { return m_controller.isPlaying (); }

void CSound::render () { }
