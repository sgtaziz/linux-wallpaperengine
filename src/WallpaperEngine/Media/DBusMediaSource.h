#pragma once

#include "MediaSource.h"
#include "MediaArtwork.h"
#include <dbus/dbus.h>

namespace WallpaperEngine::Media {
class DBusMediaSource : public MediaSource {
public:
    explicit DBusMediaSource (std::chrono::milliseconds updateInterval);
    ~DBusMediaSource () override;

    void parseMetadata (DBusMessageIter& variant);
    void parsePlaybackStatus (DBusMessageIter& variant, const char* sender);
    void parsePosition (DBusMessageIter& variant);
    [[nodiscard]] bool acceptsSignalSender (const char* sender) const;
    void noteNameOwnerChanged (DBusMessage* message);
    void noteCandidateChanged ();

    void update () override;

protected:
    void performUpdate () override;
    void initialStatusFetch ();
    void detectPlayer ();

    DBusMessage* dbusMessage (
	const char* bus_name, const char* path, const char* interface, const char* method, const char* iface = nullptr,
	const char* prop = nullptr
    );

    std::optional<std::string> m_currentPlayer = std::nullopt;
    std::optional<std::string> m_selectedOwner = std::nullopt;
    bool m_discovering = false;
    bool m_selectedOwnerChanged = false;
    bool m_candidateChanged = false;
    bool m_albumEventEmittedInPoll = false;
    MediaArtworkProbeCache m_artworkCache;

    DBusConnection* m_connection;
};
}
