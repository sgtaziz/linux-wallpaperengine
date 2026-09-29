#pragma once

#include <array>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Model {
// Authored property timelines are independent of puppet and texture animations.
class PropertyAnimation {
public:
    struct Handle { bool enabled = false; float x = 0.0f; float y = 0.0f; };
    struct Key { int frame = 0; float value = 0.0f; bool constant = false;
                 Handle back; Handle front; };
    enum class Mode { Loop, Mirror, Single };

    std::string name;
    float fps = 0.0f;
    int length = 0;
    float rate = 1.0f;
    Mode mode = Mode::Loop;
    bool paused = false;
    bool finished = false;
    bool reverse = false;
    std::array<std::vector<Key>, 4> channels;
    float time = 0.0f;

    [[nodiscard]] float duration () const { return fps > 0.0f ? length / fps : 0.0f; }
    [[nodiscard]] float frame () const { return time * fps; }
    [[nodiscard]] bool isPlaying () const { return !paused && !finished; }
    void play ();
    void pause () { paused = true; }
    void stop ();
    bool setFrame (float value);
    void advance (float seconds);
    [[nodiscard]] float sample (size_t channel) const;
    [[nodiscard]] float sampleFrame (size_t channel, int frame) const;
};
}
