#pragma once

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Audio {

/** Clip scheduling shared by sound layers; the decoder remains in AudioStream. */
class SoundPlaybackPolicy {
public:
    enum class Mode { Pool, Random, Single };
    enum class State { Ready, PausedReady, Playing, Paused, Waiting, PausedWaiting, Stopped };

    SoundPlaybackPolicy (std::string_view mode, bool startSilent, float minTime, float maxTime) :
        m_mode (mode == "random" ? Mode::Random : mode == "single" ? Mode::Single : Mode::Pool),
        m_state (startSilent ? State::Stopped : State::Ready),
        m_minTime (std::isfinite (minTime) ? std::max (0.0f, minTime) : 0.0f),
        m_maxTime (std::isfinite (maxTime) ? std::max (m_minTime, maxTime) : m_minTime) { }

    [[nodiscard]] Mode mode () const { return m_mode; }
    [[nodiscard]] State state () const { return m_state; }
    [[nodiscard]] bool ready () const { return m_state == State::Ready; }
    [[nodiscard]] bool canConsumeCompletion () const { return m_state == State::Playing; }

    void clipStarted () { m_state = State::Playing; }
    void clipFinished (float randomUnit) {
        if (m_state != State::Playing) return;
        if (m_mode == Mode::Single) {
            m_state = State::Stopped;
        } else if (m_mode == Mode::Pool) {
            m_state = State::Ready;
        } else {
            m_remaining = m_minTime + std::clamp (randomUnit, 0.0f, 1.0f) * (m_maxTime - m_minTime);
            m_state = m_remaining > 0.0f ? State::Waiting : State::Ready;
        }
    }
    void advance (float sceneDelta) {
        if (m_state != State::Waiting || !std::isfinite (sceneDelta) || sceneDelta <= 0.0f) return;
        m_remaining = std::max (0.0f, m_remaining - sceneDelta);
        if (m_remaining == 0.0f) m_state = State::Ready;
    }
    void play () { m_state = State::Ready; m_remaining = 0.0f; }
    void pause () {
        if (m_state == State::Playing) m_state = State::Paused;
        else if (m_state == State::Waiting) m_state = State::PausedWaiting;
        else if (m_state == State::Ready) m_state = State::PausedReady;
    }
    void resume () {
        if (m_state == State::Paused) m_state = State::Playing;
        else if (m_state == State::PausedWaiting) m_state = State::Waiting;
        else if (m_state == State::PausedReady) m_state = State::Ready;
    }
    void stop () { m_state = State::Stopped; m_remaining = 0.0f; }

private:
    Mode m_mode;
    State m_state;
    float m_minTime;
    float m_maxTime;
    float m_remaining = 0.0f;
};

/** Try each still-usable clip at most once; remember failures across cycles. */
template <typename Rng, typename Starter>
bool startOneOf (size_t count, std::set<size_t>& failed, Rng& rng, Starter&& tryStart) {
    std::vector<size_t> candidates;
    for (size_t index = 0; index < count; ++index) {
        if (!failed.contains (index)) candidates.push_back (index);
    }
    std::shuffle (candidates.begin (), candidates.end (), rng);
    for (const size_t index : candidates) {
        if (tryStart (index)) return true;
        failed.insert (index);
    }
    return false;
}

} // namespace WallpaperEngine::Audio
