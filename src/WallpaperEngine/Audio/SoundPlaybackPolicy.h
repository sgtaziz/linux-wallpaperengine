#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <set>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Audio {

/**
 * Sound-layer scheduler recovered from native: play/schedule
 * update, volume callback, pause,
 * stop and isPlaying. The decoder remains in AudioStream.
 */
class SoundPlaybackPolicy {
public:
    /** Native mode names are loop (default), random and single. */
    enum class Mode { Loop, Random, Single };
    /**
     * Logical voice state. The native driver never resets it at EOF, so a
     * naturally finished clip is still Live until it is stopped or replaced.
     */
    enum class Voice { None, Live, Paused };
    enum class Action {
        None,
        /** Stop the current clip, pick one and start it from its beginning. */
        StartNew,
        /** Continue the logically paused clip; a finished one replays from its start. */
        ResumeVoice,
    };

    SoundPlaybackPolicy (std::string_view mode, bool startSilent, float minTime, float maxTime, size_t clipCount) :
        m_mode (mode == "random" ? Mode::Random : mode == "single" ? Mode::Single : Mode::Loop),
        m_clipCount (clipCount), m_startSilent (startSilent),
        m_minTime (std::isfinite (minTime) ? minTime : 0.0f),
        m_maxTime (std::isfinite (maxTime) ? maxTime : m_minTime) { }

    [[nodiscard]] Mode mode () const { return m_mode; }
    [[nodiscard]] Voice voice () const { return m_voice; }
    [[nodiscard]] bool paused () const { return m_paused; }
    [[nodiscard]] bool stopped () const { return m_stopped; }
    [[nodiscard]] bool startSilent () const { return m_startSilent; }
    [[nodiscard]] float timer () const { return m_timer; }
    [[nodiscard]] float remaining () const { return m_remaining; }
    /** Native loop mode with fewer than two files loops inside the decoder. */
    [[nodiscard]] bool loopsInDecoder () const { return m_mode == Mode::Loop && m_clipCount < 2; }

    /** Layer init runs play() unless startsilent is set. */
    Action initialize (bool audible) { return m_startSilent ? Action::None : play (audible); }

    Action play (bool audible) {
        // A zero gain returns before loading anything but still clears
        // startsilent, stopped and paused.
        if (!audible) {
            m_startSilent = m_stopped = m_paused = false;
            return Action::None;
        }
        if (m_paused) return resumePausedVoice ();
        m_startSilent = m_stopped = false;
        if (m_clipCount == 0) return Action::None;
        switch (m_mode) {
            case Mode::Loop:
            case Mode::Single:
                // Loop and single stop every voice and restart from the beginning.
                m_timer = 0.0f;
                return Action::StartNew;
            case Mode::Random:
                // Random only starts once its schedule timer has run out.
                return m_timer == 0.0f ? Action::StartNew : Action::None;
        }
        return Action::None;
    }

    /**
     * A clip was started from its beginning. Random mode measures the next
     * interval from this start: timer = min + u * (max - min) + duration.
     * An unknown duration defers the interval until the decoder reports EOF.
     */
    void clipStarted (std::optional<float> duration, float randomUnit) {
        m_voice = Voice::Live;
        m_interval = m_minTime + std::clamp (randomUnit, 0.0f, 1.0f) * (m_maxTime - m_minTime);
        m_durationKnown = duration && std::isfinite (*duration) && *duration >= 0.0f;
        m_remaining = m_durationKnown ? *duration : std::numeric_limits<float>::infinity ();
        if (m_mode == Mode::Random) m_timer = m_interval + m_remaining;
        m_loopClip = LoopClip::Playing;
        m_inaudibleLive = 0.0f;
    }

    /** No clip could be started. */
    void startFailed () { stop (); }

    /**
     * The decoder finished a non-repeating clip. The logical voice stays
     * live (native keeps its state), so this only drives the multi-file
     * loop repick and the unknown-duration fallback.
     */
    Action clipEnded (bool audible) {
        if (!m_durationKnown) {
            m_durationKnown = true;
            m_remaining = 0.0f;
            if (m_mode == Mode::Random) {
                m_timer = std::max (0.0f, m_interval);
                if (m_timer == 0.0f && !m_paused && !m_stopped) return play (audible);
            }
        }
        if (m_mode != Mode::Loop || m_paused || m_stopped) return Action::None;
        // A replayed voice that ends again leaves the pending wait running.
        if (m_loopClip != LoopClip::Playing) return Action::None;
        if (audible && !(m_inaudibleLive > 0.0f)) return play (true);
        // The next update arms the wait for the inaudible time.
        m_loopClip = LoopClip::Ended;
        return Action::None;
    }

    /** Scene update; skipped while paused or stopped. */
    Action advance (float sceneDelta, bool audible) {
        if (m_paused || m_stopped || !std::isfinite (sceneDelta) || sceneDelta <= 0.0f) return Action::None;
        Action action = Action::None;
        // The schedule timer only runs while vol^2 * gain > 0.
        if (audible && m_timer > 0.0f) {
            m_timer -= sceneDelta;
            if (m_timer <= 0.0f) {
                m_timer = 0.0f;
                action = play (true);
            }
        }
        if (m_remaining > 0.0f) m_remaining -= sceneDelta;
        // Native multi-file loops repick when a clip-length timer expires
        // It only runs while audible, but a muted voice
        // keeps playing, so the clip reaches EOF early by
        // the inaudible time it played through; the next pick waits that long.
        // The update that sees EOF still belongs to the clip.
        if (m_mode == Mode::Loop && !loopsInDecoder () && m_voice == Voice::Live &&
            m_loopClip != LoopClip::Waiting) {
            if (!audible) m_inaudibleLive += sceneDelta;
            if (m_loopClip == LoopClip::Ended) {
                m_loopClip = LoopClip::Waiting;
                m_timer = std::max (m_inaudibleLive, std::numeric_limits<float>::min ());
            }
        }
        return action;
    }

    /**
     * Volume change callback. Loop and random layers start when
     * no voice is live and the layer is not paused, startsilent or stopped.
     * Single mode never starts here.
     */
    Action volumeChanged (bool audible) {
        if (m_mode == Mode::Single) return Action::None;
        if ((m_mode == Mode::Loop || m_timer > 0.0f) && m_voice != Voice::None) return Action::None;
        if (m_paused || m_startSilent || m_stopped) return Action::None;
        return play (audible);
    }

    /** Returns whether the current clip must be paused. */
    bool pause () {
        if (m_paused || m_stopped) return false;
        m_paused = true;
        if (m_voice == Voice::Live) m_voice = Voice::Paused;
        return true;
    }

    Action resume () { return m_paused ? resumePausedVoice () : Action::None; }

    void stop () {
        m_paused = false;
        m_stopped = true;
        m_timer = 0.0f;
        m_voice = Voice::None;
    }

    [[nodiscard]] bool isPlaying () const {
        if (m_mode == Mode::Single && !(m_remaining > 0.0f)) return false;
        if (m_mode == Mode::Random && !(m_timer > 0.0f)) return false;
        return m_voice != Voice::None && !m_paused;
    }

private:
    /** A multi-file loop clip is playing, has just ended, or waits to repick. */
    enum class LoopClip { Playing, Ended, Waiting };

    Action resumePausedVoice () {
        m_paused = false;
        if (m_voice != Voice::Paused) return Action::None;
        m_voice = Voice::Live;
        return Action::ResumeVoice;
    }

    Mode m_mode;
    size_t m_clipCount;
    bool m_startSilent;
    bool m_paused = false;
    bool m_stopped = false;
    float m_minTime;
    float m_maxTime;
    float m_timer = 0.0f;
    float m_remaining = 0.0f;
    float m_interval = 0.0f;
    bool m_durationKnown = true;
    Voice m_voice = Voice::None;
    LoopClip m_loopClip = LoopClip::Playing;
    float m_inaudibleLive = 0.0f;
};

/** Try each still-usable clip at most once; remember failures across cycles. */
template <typename Rng, typename Starter>
std::optional<size_t> startOneOf (size_t count, std::set<size_t>& failed, Rng& rng, Starter&& tryStart) {
    std::vector<size_t> candidates;
    for (size_t index = 0; index < count; ++index) {
        if (!failed.contains (index)) candidates.push_back (index);
    }
    std::shuffle (candidates.begin (), candidates.end (), rng);
    for (const size_t index : candidates) {
        if (tryStart (index)) return index;
        failed.insert (index);
    }
    return std::nullopt;
}

/** The decoder side of one sound layer. */
class SoundClipPlayer {
public:
    virtual ~SoundClipPlayer () = default;
    /**
     * Native re-applies the layer's attenuation and mindistance
     * every time its scheduler body runs, so a clip started or
     * resumed by a timer, a loop end or a volume change also picks up pending
     * script writes. Called before the voice is started or resumed.
     */
    virtual void latchDistance () { }
    /** Create a new decoder for the clip, positioned at its start, and start mixing it. */
    virtual bool startClip (size_t index, bool repeat, float volume) = 0;
    virtual void releaseClip () = 0;
    [[nodiscard]] virtual bool hasClip () const = 0;
    /** The decoder reached EOF and the mixer consumed its last samples. */
    [[nodiscard]] virtual bool clipFinished () const = 0;
    virtual void setClipPaused (bool paused) = 0;
    virtual void setClipVolume (float volume) = 0;
    [[nodiscard]] virtual std::optional<float> clipDuration () const = 0;
};

/** Native volume gate: vol^2 > 0 with the mixer's non-negative volume. */
inline bool isAudibleSoundVolume (float volume) { return std::isfinite (volume) && volume > 0.0f; }

/** Applies SoundPlaybackPolicy actions to a clip player. */
class SoundLayerController {
public:
    SoundLayerController (SoundPlaybackPolicy policy, size_t clipCount, SoundClipPlayer& player, uint32_t seed) :
        m_policy (policy), m_clipCount (clipCount), m_player (player), m_rng (seed) { }

    void initialize (float volume) {
        m_lastVolume = volume;
        apply (m_policy.initialize (isAudibleSoundVolume (volume)), volume);
    }

    void tick (float sceneDelta, float volume) {
        const bool audible = isAudibleSoundVolume (volume);
        if (m_player.hasClip ()) m_player.setClipVolume (volume);
        if (!(volume == m_lastVolume)) {
            m_lastVolume = volume;
            apply (m_policy.volumeChanged (audible), volume);
        }
        if (m_player.hasClip () && !m_repeating && m_player.clipFinished ()) {
            m_player.releaseClip ();
            apply (m_policy.clipEnded (audible), volume);
        }
        apply (m_policy.advance (sceneDelta, audible), volume);
    }

    void play (float volume) { apply (m_policy.play (isAudibleSoundVolume (volume)), volume); }
    void pause () {
        if (m_policy.pause () && m_player.hasClip ()) m_player.setClipPaused (true);
    }
    void resume (float volume) { apply (m_policy.resume (), volume); }
    void stop () {
        m_player.releaseClip ();
        m_policy.stop ();
    }

    [[nodiscard]] bool isPlaying () const { return m_policy.isPlaying (); }
    [[nodiscard]] const SoundPlaybackPolicy& policy () const { return m_policy; }
    [[nodiscard]] std::optional<size_t> lastClip () const { return m_lastClip; }

private:
    void apply (SoundPlaybackPolicy::Action action, float volume) {
        if (action != SoundPlaybackPolicy::Action::None) m_player.latchDistance ();
        if (action == SoundPlaybackPolicy::Action::StartNew) {
            m_player.releaseClip ();
            m_repeating = m_policy.loopsInDecoder ();
            const auto started = startOneOf (m_clipCount, m_unavailableClips, m_rng, [&] (size_t index) {
                return m_player.startClip (index, m_repeating, volume);
            });
            if (!started) {
                m_policy.startFailed ();
                return;
            }
            m_lastClip = started;
            m_policy.clipStarted (m_player.clipDuration (), std::generate_canonical<float, 24> (m_rng));
        } else if (action == SoundPlaybackPolicy::Action::ResumeVoice) {
            if (m_player.hasClip () && !m_player.clipFinished ()) {
                m_player.setClipPaused (false);
                return;
            }
            // Native resumes the driver voice; a finished stream restarts
            // from its beginning without a new pick or schedule change.
            m_player.releaseClip ();
            if (!m_lastClip || !m_player.startClip (*m_lastClip, m_repeating, volume)) {
                // No voice could be replayed: the layer is not left live without a clip.
                if (m_lastClip) m_unavailableClips.insert (*m_lastClip);
                m_policy.startFailed ();
            }
        }
    }

    SoundPlaybackPolicy m_policy;
    size_t m_clipCount;
    SoundClipPlayer& m_player;
    std::mt19937 m_rng;
    std::set<size_t> m_unavailableClips;
    std::optional<size_t> m_lastClip;
    float m_lastVolume = 0.0f;
    bool m_repeating = false;
};

} // namespace WallpaperEngine::Audio
