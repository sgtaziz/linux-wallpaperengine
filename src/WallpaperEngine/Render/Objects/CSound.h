#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"
#include "WallpaperEngine/Audio/SoundPlaybackPolicy.h"
#include "WallpaperEngine/Render/CObject.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <memory>
#include <optional>
#include <random>
#include <set>

using namespace WallpaperEngine;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

class CSound final : public Scripting::ScriptableObject {
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

protected:
    void startSelectedClip ();
    void releaseActiveClip ();

private:
    const Sound& m_sound;
    Audio::SoundPlaybackPolicy m_policy;
    std::mt19937 m_rng { std::random_device {} () };
    std::optional<int> m_streamId;
    std::unique_ptr<Audio::AudioStream> m_stream;
    std::set<size_t> m_unavailableClips;
};
} // namespace WallpaperEngine::Render::Objects
