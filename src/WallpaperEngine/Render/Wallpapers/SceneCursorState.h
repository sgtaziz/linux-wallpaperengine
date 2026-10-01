#pragma once

#include <unordered_set>

namespace WallpaperEngine::Render::Wallpapers {

// Native 140189e10 retains hover and button capture separately for every
// layer. Visit candidates in reverse draw order; skipped solid/blocked layers
// retain their state until visited again or destroyed.
class SceneCursorState {
public:
    void beginFrame (bool moved, bool leftDown) {
        m_moved = moved;
        m_pressed = leftDown && !m_leftDown;
        m_released = !leftDown && m_leftDown;
        m_leftDown = leftDown;
        m_dragging = leftDown && !m_buttonLayers.empty ();
    }

    template <typename Dispatch>
    void visit (int layer, bool inside, Dispatch&& dispatch) {
        if (m_dragging) {
            if (m_moved && m_buttonLayers.contains (layer)) dispatch ("cursorMove");
            return;
        }
        if (inside) {
            if (m_hoverLayers.insert (layer).second) dispatch ("cursorEnter");
            if (m_moved) dispatch ("cursorMove");
            if (m_pressed) {
                dispatch ("cursorDown");
                m_buttonLayers.insert (layer);
            }
            if (m_released) {
                dispatch ("cursorUp");
                if (m_buttonLayers.contains (layer)) dispatch ("cursorClick");
            }
        } else {
            if (m_released && m_buttonLayers.contains (layer)) dispatch ("cursorUp");
            if (m_hoverLayers.erase (layer)) dispatch ("cursorLeave");
        }
    }

    void finishFrame () {
        if (!m_leftDown) m_buttonLayers.clear ();
    }

    [[nodiscard]] bool hasButtonCapture () const { return m_dragging; }

    void forget (int layer) {
        m_hoverLayers.erase (layer);
        m_buttonLayers.erase (layer);
    }

private:
    std::unordered_set<int> m_hoverLayers;
    std::unordered_set<int> m_buttonLayers;
    bool m_leftDown = false;
    bool m_moved = false;
    bool m_pressed = false;
    bool m_released = false;
    bool m_dragging = false;
};

} // namespace WallpaperEngine::Render::Wallpapers
