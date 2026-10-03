#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/SoundPlaybackPolicy.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <memory>
#include <optional>

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

class CSound final : public Scripting::ScriptableObject, private Audio::SoundClipPlayer {
public:
    CSound (Wallpapers::CScene& scene, const Sound& sound);
    ~CSound () override;

    void render () override;
    void tick (float sceneDelta);
    void play ();
    void pause ();
    void resume ();
    void stop ();
    [[nodiscard]] bool isPlaying () const;

private:
    bool startClip (size_t index, bool repeat, float volume) override;
    void releaseClip () override;
    [[nodiscard]] bool hasClip () const override;
    [[nodiscard]] bool clipFinished () const override;
    void setClipPaused (bool paused) override;
    void setClipVolume (float volume) override;
    [[nodiscard]] std::optional<float> clipDuration () const override;

    [[nodiscard]] float currentVolume () const;
    [[nodiscard]] bool audioEnabled () const;
    /** Mono clips: native spatial pan and distance gain, or the non-spatial routing level. */
    void updateMonoRouting (Audio::AudioStream& stream) const;

    const Sound& m_sound;
    std::optional<int> m_streamId;
    std::unique_ptr<Audio::AudioStream> m_stream;
    Audio::SoundLayerController m_controller;
};
} // namespace WallpaperEngine::Render::Objects
