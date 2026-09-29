#pragma once

namespace WallpaperEngine::Audio::Drivers::Recorders {
class PlaybackRecorder {
public:
    virtual ~PlaybackRecorder () = default;

    virtual void update ();

    float audio16[16] = { 0 };
    float audio32[32] = { 0 };
    float audio64[64] = { 0 };
    float audio16Left[16] = { 0 };
    float audio16Right[16] = { 0 };
    float audio32Left[32] = { 0 };
    float audio32Right[32] = { 0 };
    float audio64Left[64] = { 0 };
    float audio64Right[64] = { 0 };
    // Unsmooth service output for each scene's independent temporal filter.
    float audio64RawLeft[64] = { 0 };
    float audio64RawRight[64] = { 0 };
};
} // namespace WallpaperEngine::Audio::Drivers::Recorders
