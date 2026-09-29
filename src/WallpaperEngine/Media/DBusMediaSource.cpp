#include "DBusMediaSource.h"
#include "MediaTimelineUnits.h"
#include "MediaArtwork.h"

#include "WallpaperEngine/Data/Utils/ScopeGuard.h"
#include "WallpaperEngine/Logging/Log.h"

#include <string_view>
#include <utility>

using namespace WallpaperEngine::Media;

DBusHandlerResult dbus_message_filter (DBusConnection* connection, DBusMessage* message, void* user_data) {
    const auto mediaSource = static_cast<DBusMediaSource*> (user_data);

    if (dbus_message_is_signal (message, "org.freedesktop.DBus", "NameOwnerChanged")) {
        mediaSource->noteNameOwnerChanged (message);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (!dbus_message_is_signal (message, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* interface = nullptr;
    DBusMessageIter iter;

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &interface);

    std::string iface = interface ?: "";

    if (iface != "org.mpris.MediaPlayer2.Player") {
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    DBusMessageIter changed;

    dbus_message_iter_next (&iter);
    dbus_message_iter_recurse (&iter, &changed);

    if (!mediaSource->acceptsSignalSender (dbus_message_get_sender (message))) {
        while (dbus_message_iter_get_arg_type (&changed) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse (&changed, &entry);
            const char* key = nullptr;
            if (dbus_message_iter_get_arg_type (&entry) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic (&entry, &key);
            if (key && (std::string_view (key) == "Metadata" ||
                        std::string_view (key) == "PlaybackStatus")) {
                mediaSource->noteCandidateChanged ();
                break;
            }
            dbus_message_iter_next (&changed);
        }
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    while (dbus_message_iter_get_arg_type (&changed) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&changed, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "Metadata") {
	    mediaSource->parseMetadata (value);
	} else if (keyStr == "PlaybackStatus") {
	    mediaSource->parsePlaybackStatus (value, dbus_message_get_sender (message));
	}

	dbus_message_iter_next (&changed);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}

DBusMediaSource::DBusMediaSource (std::chrono::milliseconds updateInterval) : MediaSource (updateInterval) {
    DBusError err;

    dbus_error_init (&err);

    this->m_connection = dbus_bus_get (DBUS_BUS_SESSION, &err);

    if (!this->m_connection) {
	sLog.exception ("Could not connect to DBus: ", err.message);
    }

    dbus_connection_add_filter (this->m_connection, dbus_message_filter, this, nullptr);

    dbus_bus_add_match (
	this->m_connection, "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
	nullptr
    );
    dbus_bus_add_match (this->m_connection,
        "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged',"
        "arg0namespace='org.mpris.MediaPlayer2'", nullptr);

    dbus_connection_flush (this->m_connection);

    this->performUpdate ();
}

DBusMediaSource::~DBusMediaSource () {
    dbus_connection_remove_filter (this->m_connection, dbus_message_filter, this);

    dbus_connection_unref (this->m_connection);
}

bool DBusMediaSource::acceptsSignalSender (const char* sender) const {
    return sender && m_selectedOwner.has_value () && *m_selectedOwner == sender;
}

void DBusMediaSource::noteCandidateChanged () {
    // A second integration for the same tab may acquire artwork after startup.
    // Rediscover only while the selected representation has no decoded cover.
    if (m_currentPlayer && (m_mediaInfo.playbackState != Playing || !m_mediaInfo.artwork))
        m_candidateChanged = true;
}

void DBusMediaSource::noteNameOwnerChanged (DBusMessage* message) {
    const char* name = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    DBusError error;
    dbus_error_init (&error);
    const bool valid = dbus_message_get_args (message, &error,
        DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &oldOwner,
        DBUS_TYPE_STRING, &newOwner, DBUS_TYPE_INVALID);
    if (dbus_error_is_set (&error)) dbus_error_free (&error);
    if (!valid || !name || !oldOwner || !m_currentPlayer || !m_selectedOwner) return;
    if (std::string_view (name).starts_with ("org.mpris.MediaPlayer2.") &&
        !*oldOwner && newOwner && *newOwner &&
        (m_mediaInfo.playbackState != Playing || !m_mediaInfo.artwork))
        m_candidateChanged = true;
    if (*m_currentPlayer == name && *m_selectedOwner == oldOwner
        && (!newOwner || *m_selectedOwner != newOwner))
        m_selectedOwnerChanged = true;
}

void DBusMediaSource::parseMetadata (DBusMessageIter& variant) {
    DBusMessageIter dict;
    dbus_message_iter_recurse (&variant, &dict);

    // MPRIS Metadata is a full replacement map for the current track. Missing
    // fields must clear their old values when a new track exposes fewer keys.
    MediaInfo updated = m_mediaInfo;
    updated.title.clear ();
    updated.artist.clear ();
    updated.album.clear ();
    updated.url.reset ();
    updated.thumbnailAvailable = false;
    updated.artwork.reset ();
    updated.duration = 0;

    while (dbus_message_iter_get_arg_type (&dict) == DBUS_TYPE_DICT_ENTRY) {
	DBusMessageIter entry;
	dbus_message_iter_recurse (&dict, &entry);

	const char* key = nullptr;
	dbus_message_iter_get_basic (&entry, &key);

	std::string keyStr = key ?: "";

	DBusMessageIter value;
	dbus_message_iter_next (&entry);
	dbus_message_iter_recurse (&entry, &value);

	if (keyStr == "xesam:title") {
	    const char* title = nullptr;
	    dbus_message_iter_get_basic (&value, &title);

	    updated.title = title ?: "";
	} else if (keyStr == "xesam:artist") {
	    DBusMessageIter arr;

	    dbus_message_iter_recurse (&value, &arr);

	    if (dbus_message_iter_get_arg_type (&arr) == DBUS_TYPE_STRING) {
		const char* artist = nullptr;
		dbus_message_iter_get_basic (&arr, &artist);

		updated.artist = artist ?: "";
	    }
	} else if (keyStr == "xesam:album") {
	    const char* album = nullptr;
	    dbus_message_iter_get_basic (&value, &album);

	    updated.album = album ?: "";
	} else if (keyStr == "mpris:artUrl") {
	    const char* artUrl = nullptr;
	    dbus_message_iter_get_basic (&value, &artUrl);

	    if (artUrl && *artUrl) updated.url = artUrl;
	} else if (keyStr == "mpris:length") {
	    int64_t length = 0;
	    dbus_message_iter_get_basic (&value, &length);

	    updated.duration = mprisMicrosecondsToSeconds (length);
	}

	dbus_message_iter_next (&dict);
    }

    if (updated.url) updated.artwork = m_artworkCache.load (*updated.url);
    updated.thumbnailAvailable = updated.artwork != nullptr;
    const bool metadataUpdate = updated.title != m_mediaInfo.title || updated.artist != m_mediaInfo.artist
        || updated.album != m_mediaInfo.album || updated.duration != m_mediaInfo.duration;
    const bool albumUpdate = updated.url != m_mediaInfo.url
        || updated.artwork != m_mediaInfo.artwork;
    m_mediaInfo = std::move (updated);

    sLog.debug (
	"Player metadata received: title=", this->m_mediaInfo.title, ",artist=", this->m_mediaInfo.artist,
	",album=", this->m_mediaInfo.album, ",url=", this->m_mediaInfo.url.has_value () ? *this->m_mediaInfo.url : ""
    );

    if (metadataUpdate && !m_discovering) {
	this->fireMetadataListeners ();
    }

    if (albumUpdate && !m_discovering) {
	this->fireAlbumArtListeners ();
    }
}

void DBusMediaSource::parsePlaybackStatus (DBusMessageIter& variant, const char* sender) {
    const char* status = nullptr;
    dbus_message_iter_get_basic (&variant, &status);
    std::string statusStr = status ?: "";
    PlaybackState newState = this->m_mediaInfo.playbackState;

    if (statusStr == "Playing") {
	if (sender != nullptr) {
	    // Discovery passes the well-known MPRIS name; a later signal passes
	    // its unique sender. Keep the well-known name so replacement owners
	    // can be discovered under the same player identity.
	    if (sender[0] != ':') this->m_currentPlayer = sender;
	    this->m_selectedOwner = sender;
	}

	newState = PlaybackState::Playing;
    } else if (statusStr == "Paused") {
	newState = PlaybackState::Paused;
    } else {
	newState = PlaybackState::Stopped;
    }

    if (this->m_mediaInfo.playbackState == Playing && newState != Playing)
        m_candidateChanged = true;
    if (newState != this->m_mediaInfo.playbackState) {
	this->m_mediaInfo.playbackState = newState;
	if (!m_discovering) this->fireMetadataListeners ();
    }
}

void DBusMediaSource::parsePosition (DBusMessageIter& variant) {
    int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    const double seconds = mprisMicrosecondsToSeconds (position);
    if (this->m_mediaInfo.position != seconds) {
	this->m_mediaInfo.position = seconds;
	this->fireMetadataListeners ();
    }
}

void DBusMediaSource::update () {
    // drain any dbus events
    dbus_connection_read_write (this->m_connection, 0);

    while (dbus_connection_dispatch (this->m_connection) == DBUS_DISPATCH_DATA_REMAINS)
	;

    if (m_selectedOwnerChanged ||
        (m_candidateChanged && std::chrono::steady_clock::now () >= m_nextUpdate)) {
        m_selectedOwnerChanged = false;
        m_candidateChanged = false;
        const auto previous = m_mediaInfo;
        m_currentPlayer.reset ();
        m_selectedOwner.reset ();
        m_mediaInfo = {.playbackState = Stopped, .title = "", .artist = "", .album = "",
                       .url = std::nullopt, .thumbnailAvailable = false, .artwork = nullptr,
                       .duration = 0.0, .position = 0.0, .available = false};
        performUpdate ();
        m_nextUpdate = std::chrono::steady_clock::now () + m_updateInterval;
        if (!m_mediaInfo.available && previous.available) fireMetadataListeners ();
        if (m_mediaInfo.artwork != previous.artwork && !m_albumEventEmittedInPoll)
            fireAlbumArtListeners ();
        return;
    }

    this->MediaSource::update ();
}

DBusMessage* DBusMediaSource::dbusMessage (
    const char* bus_name, const char* path, const char* interface, const char* method, const char* iface,
    const char* prop
) {
    DBusError err;
    dbus_error_init (&err);

    DBusMessage* msg = dbus_message_new_method_call (bus_name, path, interface, method);
    Data::Utils::ScopeGuard guard ([msg] { dbus_message_unref (msg); });

    if (iface != nullptr && prop != nullptr) {
	dbus_message_append_args (msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    }

    // This runs on the render thread. A stalled player must not freeze a
    // wallpaper indefinitely; retry on the next polling interval.
    DBusMessage* reply = dbus_connection_send_with_reply_and_block (m_connection, msg, 1000, &err);

    if (reply == nullptr) {
	sLog.error ("DBus error: ", err.message, " (", err.name, ")");
	return nullptr;
    }

    return reply;
}

void DBusMediaSource::detectPlayer () {
    DBusMessage* reply
	= this->dbusMessage ("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames");

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    std::vector<std::string> players;
    DBusMessageIter iter;
    dbus_message_iter_init (reply, &iter);

    if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) {
	return;
    }

    DBusMessageIter array;
    dbus_message_iter_recurse (&iter, &array);

    while (dbus_message_iter_get_arg_type (&array) != DBUS_TYPE_INVALID) {
	char* name;
	dbus_message_iter_get_basic (&array, &name);

	std::string service = name;

	if (service.starts_with ("org.mpris.MediaPlayer2.")) {
	    players.push_back (service);
	}

	dbus_message_iter_next (&array);
    }

    if (players.empty ()) {
	return;
    }

    std::optional<std::pair<std::string, PlaybackState>> fallback;
    std::optional<std::string> selectedPlaying;
    struct TrackIdentity {
        std::string mediaUrl;
        bool hasArtworkUrl = false;
    };
    const auto trackIdentity = [this] (const std::string& player) -> std::optional<TrackIdentity> {
        DBusMessage* metadataReply = this->dbusMessage (
            player.c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
            "org.mpris.MediaPlayer2.Player", "Metadata"
        );
        if (!metadataReply) return std::nullopt;
        Data::Utils::ScopeGuard metadataGuard ([metadataReply] { dbus_message_unref (metadataReply); });
        DBusMessageIter outer;
        if (!dbus_message_iter_init (metadataReply, &outer) ||
            dbus_message_iter_get_arg_type (&outer) != DBUS_TYPE_VARIANT) return std::nullopt;
        DBusMessageIter variant;
        dbus_message_iter_recurse (&outer, &variant);
        if (dbus_message_iter_get_arg_type (&variant) != DBUS_TYPE_ARRAY) return std::nullopt;
        DBusMessageIter entries;
        dbus_message_iter_recurse (&variant, &entries);
        TrackIdentity identity;
        while (dbus_message_iter_get_arg_type (&entries) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse (&entries, &entry);
            const char* key = nullptr;
            if (dbus_message_iter_get_arg_type (&entry) != DBUS_TYPE_STRING) {
                dbus_message_iter_next (&entries);
                continue;
            }
            dbus_message_iter_get_basic (&entry, &key);
            if (!dbus_message_iter_next (&entry) ||
                dbus_message_iter_get_arg_type (&entry) != DBUS_TYPE_VARIANT) {
                dbus_message_iter_next (&entries);
                continue;
            }
            DBusMessageIter value;
            dbus_message_iter_recurse (&entry, &value);
            if (dbus_message_iter_get_arg_type (&value) == DBUS_TYPE_STRING) {
                const char* text = nullptr;
                dbus_message_iter_get_basic (&value, &text);
                if (key && std::string_view (key) == "xesam:url") identity.mediaUrl = text ?: "";
                if (key && std::string_view (key) == "mpris:artUrl")
                    identity.hasArtworkUrl = text && *text;
            }
            dbus_message_iter_next (&entries);
        }
        return identity;
    };
    std::optional<TrackIdentity> selectedIdentity;

    for (const auto& player : players) {
	// also get playback status
	reply = this->dbusMessage (
	    player.c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	    "org.mpris.MediaPlayer2.Player", "PlaybackStatus"
	);

	if (reply == nullptr) continue;

	Data::Utils::ScopeGuard guard2 ([reply] { dbus_message_unref (reply); });

	DBusMessageIter outer;
	if (!dbus_message_iter_init (reply, &outer) ||
	    dbus_message_iter_get_arg_type (&outer) != DBUS_TYPE_VARIANT) continue;
	DBusMessageIter variant;
	dbus_message_iter_recurse (&outer, &variant);
	const char* status = nullptr;
	dbus_message_iter_get_basic (&variant, &status);
	if (!fallback) fallback = std::pair (player,
	    std::string_view (status ?: "") == "Paused" ? Paused : Stopped);

	if (std::string_view (status ?: "") == "Playing") {
	    if (!selectedPlaying) {
	        selectedPlaying = player;
	        selectedIdentity = trackIdentity (player);
	    } else if (selectedIdentity && !selectedIdentity->mediaUrl.empty () &&
	               !selectedIdentity->hasArtworkUrl) {
	        const auto candidate = trackIdentity (player);
	        // Two MPRIS integrations can publish the same browser tab. Prefer its
	        // artwork-bearing representation, but never mix unrelated players.
	        if (candidate && candidate->hasArtworkUrl &&
	            candidate->mediaUrl == selectedIdentity->mediaUrl) {
	            selectedPlaying = player;
	            selectedIdentity = candidate;
	        }
	    }
	}
    }

    if (selectedPlaying) {
	m_currentPlayer = *selectedPlaying;
	m_mediaInfo.playbackState = Playing;
    } else if (fallback) {
	m_currentPlayer = fallback->first;
	m_mediaInfo.playbackState = fallback->second;
    }
    if (m_currentPlayer) {
	if (m_currentPlayer->starts_with (":")) {
	    m_selectedOwner = *m_currentPlayer;
	} else {
	    DBusMessage* request = dbus_message_new_method_call (
	        "org.freedesktop.DBus", "/org/freedesktop/DBus",
	        "org.freedesktop.DBus", "GetNameOwner");
	    const char* name = m_currentPlayer->c_str ();
	    dbus_message_append_args (request, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
	    DBusError ownerError;
	    dbus_error_init (&ownerError);
	    DBusMessage* ownerReply = dbus_connection_send_with_reply_and_block (
	        m_connection, request, 1000, &ownerError);
	    dbus_message_unref (request);
	    if (ownerReply) {
	        const char* owner = nullptr;
	        if (dbus_message_get_args (ownerReply, &ownerError,
	                                   DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner)
	            m_selectedOwner = owner;
	        dbus_message_unref (ownerReply);
	    }
	    if (dbus_error_is_set (&ownerError)) dbus_error_free (&ownerError);
	}
    }
}

void DBusMediaSource::initialStatusFetch () {
    if (this->m_currentPlayer.has_value () == false) {
	return;
    }

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Metadata"
    );

    if (reply == nullptr) {
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    this->parseMetadata (variant);
}

void DBusMediaSource::performUpdate () {
    m_albumEventEmittedInPoll = false;
    const auto previousArtwork = m_mediaInfo.artwork;
    // A player can start after this source was constructed. Retry discovery
    // at the configured interval instead of remaining permanently empty.
    if (!this->m_currentPlayer.has_value ()) {
	m_discovering = true;
	detectPlayer ();
	initialStatusFetch ();
	m_discovering = false;
	if (!m_currentPlayer.has_value ()) return;
    }
    const bool firstSnapshot = !m_mediaInfo.available;

    DBusMessage* reply = this->dbusMessage (
	this->m_currentPlayer.value ().c_str (), "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
	"org.mpris.MediaPlayer2.Player", "Position"
    );

    if (reply == nullptr) {
	// A disappeared or stalled player is rediscovered on a later bounded poll.
	m_currentPlayer.reset ();
	m_selectedOwner.reset ();
	m_mediaInfo.playbackState = Stopped;
	m_mediaInfo.available = false;
	m_mediaInfo.title.clear ();
	m_mediaInfo.artist.clear ();
	m_mediaInfo.album.clear ();
	m_mediaInfo.url.reset ();
	m_mediaInfo.thumbnailAvailable = false;
	m_mediaInfo.artwork.reset ();
	m_mediaInfo.duration = 0;
	m_mediaInfo.position = 0;
	fireMetadataListeners ();
	if (m_mediaInfo.artwork != previousArtwork) {
	    m_albumEventEmittedInPoll = true;
	    fireAlbumArtListeners ();
	}
	return;
    }

    Data::Utils::ScopeGuard guard ([reply] { dbus_message_unref (reply); });
    DBusMessageIter outer;
    dbus_message_iter_init (reply, &outer);

    DBusMessageIter variant;
    dbus_message_iter_recurse (&outer, &variant);

    dbus_int64_t position = 0;
    dbus_message_iter_get_basic (&variant, &position);

    const double seconds = mprisMicrosecondsToSeconds (position);
    if (firstSnapshot || this->m_mediaInfo.position != seconds) {
	this->m_mediaInfo.position = seconds;
	this->m_mediaInfo.available = true;
	this->fireMetadataListeners ();
    }
    if (m_mediaInfo.artwork != previousArtwork) {
        m_albumEventEmittedInPoll = true;
        fireAlbumArtListeners ();
    }
}
