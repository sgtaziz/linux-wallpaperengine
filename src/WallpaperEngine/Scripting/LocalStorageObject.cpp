#include "LocalStorageObject.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Model::Project;
// Ordered so stored objects keep their key insertion order, as native's raw
// JSON text does.
using json = nlohmann::ordered_json;

namespace {
// Native 2.8.42 accounts each namespace entry as an 8-byte header plus its
// JSON text and refuses a set that would exceed this total.
constexpr size_t kMaxNamespaceBytes = 100000;
constexpr size_t kEntryHeaderBytes = 8;
// Native keeps each value's text verbatim, including two texts that are not
// JSON: "undefined" when JSON.stringify yields undefined and "" when it throws.
// A later get cannot parse either and returns null. Linux stores null and
// records the native text length under this member, mirroring the namespace
// path, so the cap still counts it.
constexpr const char* kUnparsableTextRecord = "unparsableText";
unsigned nextInstanceId = 0;
std::map<unsigned, LocalStorageObject*> instances;

std::string stableHash (const std::string& value) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw (16) << std::setfill ('0') << hash;
    return output.str ();
}

std::filesystem::path defaultStateRoot () {
    if (const char* state = std::getenv ("XDG_STATE_HOME"); state && *state)
        return std::filesystem::path (state) / "linux-wallpaperengine" / "scenescript-storage";
    if (const char* home = std::getenv ("HOME"); home && *home)
        return std::filesystem::path (home) / ".local" / "state" / "linux-wallpaperengine" / "scenescript-storage";
    throw std::runtime_error ("localStorage requires XDG_STATE_HOME or HOME");
}

std::string wallpaperIdentity (const Project& project) {
    if (!project.workshopId.empty () && project.workshopId[0] != '-')
        return "workshop:" + project.workshopId;
    if (!project.storageSourcePath.empty ()) return "source:" + project.storageSourcePath;
    try {
        return "path:" + std::filesystem::weakly_canonical (
            project.assetLocator->physicalPath ("project.json")).string ();
    } catch (const std::exception&) {
        // Package-backed projects may not expose a host path. Their content
        // identity survives process restarts even when the parser's negative
        // temporary workshop ID changes.
        return "package:" + stableHash (project.assetLocator->readString ("project.json"));
    }
}

struct LockedFile {
    explicit LockedFile (const std::filesystem::path& path) {
        fd = open (path.c_str (), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0 || flock (fd, LOCK_EX) != 0) {
            if (fd >= 0) close (fd);
            throw std::runtime_error ("cannot lock SceneScript storage");
        }
    }
    ~LockedFile () { flock (fd, LOCK_UN); close (fd); }
    int fd;
};

json readData (const std::filesystem::path& path) {
    std::error_code statusError;
    const bool exists = std::filesystem::exists (path, statusError);
    if (statusError) throw std::filesystem::filesystem_error ("cannot inspect SceneScript storage", path, statusError);
    if (!exists) return json::object ();
    std::ifstream input (path, std::ios::binary);
    if (!input) throw std::runtime_error ("cannot read SceneScript storage");
    json data = json::parse (input, nullptr, false);
    if (!data.is_object ()) throw std::runtime_error ("SceneScript storage file is invalid");
    return data;
}

void writeData (const std::filesystem::path& path, const json& data) {
    const std::string serialized = data.dump ();
    static unsigned serial = 0;
    const auto temporary = path.string () + "." + std::to_string (getpid ()) + "." + std::to_string (++serial) + ".tmp";
    int fd = open (temporary.c_str (), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error ("cannot open SceneScript storage temporary file");
    try {
        size_t offset = 0;
        while (offset < serialized.size ()) {
            const ssize_t count = write (fd, serialized.data () + offset, serialized.size () - offset);
            if (count <= 0) throw std::runtime_error ("cannot write SceneScript storage");
            offset += static_cast<size_t> (count);
        }
        if (fsync (fd) != 0) throw std::runtime_error ("cannot sync SceneScript storage");
        close (fd);
        fd = -1;
        std::filesystem::rename (temporary, path);
    } catch (...) {
        if (fd >= 0) close (fd);
        std::filesystem::remove (temporary);
        throw;
    }
}

std::string toString (JSContext* context, JSValueConst value) {
    size_t length = 0;
    const char* chars = JS_ToCStringLen (context, &length, value);
    if (!chars) throw std::runtime_error ("invalid SceneScript storage string");
    std::string result (chars, length);
    JS_FreeCString (context, chars);
    return result;
}

// Native reads keys and locations as C strings, so text after a NUL is ignored.
std::string toCString (JSContext* context, JSValueConst value) {
    std::string result = toString (context, value);
    result.resize (std::strlen (result.c_str ()));
    return result;
}

// Length of the JSON.stringify text native would hold for a stored value.
size_t storedTextSize (JSContext* context, const json& value) {
    const std::string dumped = value.dump ();
    JSValue parsed = JS_ParseJSON (context, dumped.c_str (), dumped.size (), "localStorage");
    if (JS_IsException (parsed)) throw std::runtime_error ("SceneScript storage value is invalid");
    JSValue encoded = JS_JSONStringify (context, parsed, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue (context, parsed);
    if (JS_IsException (encoded)) throw std::runtime_error ("SceneScript storage value is invalid");
    WallpaperEngine::Data::Utils::ScopeGuard releaseEncoded ([context, encoded] {
        JS_FreeValue (context, encoded);
    });
    return toString (context, encoded).size ();
}

// Native raises every localStorage failure through one error constructor, a
// SyntaxError.
JSValue throwStorageError (JSContext* context, const char* message) {
    return JS_ThrowSyntaxError (context, "%s", message);
}

// The recorded unparsable-text lengths of one namespace, or nullptr.
json* unparsableTextLengths (json& data, bool global, const std::string& screenKey) {
    const auto record = data.find (kUnparsableTextRecord);
    if (record == data.end () || !record->is_object ()) return nullptr;
    json* parent = &*record;
    if (!global) {
        const auto screens = parent->find ("screens");
        if (screens == parent->end () || !screens->is_object ()) return nullptr;
        parent = &*screens;
    }
    const auto lengths = parent->find (global ? "global" : screenKey);
    return lengths != parent->end () && lengths->is_object () ? &*lengths : nullptr;
}

std::optional<size_t> recordedLength (const json* lengths, const std::string& key) {
    if (!lengths) return std::nullopt;
    const auto found = lengths->find (key);
    if (found == lengths->end () || !found->is_number_unsigned ()) return std::nullopt;
    return found->get<size_t> ();
}

// Records key's native text length, or forgets it when length is empty; a null
// key forgets the whole namespace. Empty records are removed.
void recordUnparsableText (json& data, bool global, const std::string& screenKey, const std::string* key,
                           std::optional<size_t> length) {
    if (length) {
        json& record = data[kUnparsableTextRecord];
        (global ? record["global"] : record["screens"][screenKey])[*key] = *length;
        return;
    }
    json* lengths = unparsableTextLengths (data, global, screenKey);
    if (!lengths) return;
    if (key) lengths->erase (*key); else lengths->clear ();
    if (!lengths->empty ()) return;
    json& record = data[kUnparsableTextRecord];
    if (global) {
        record.erase ("global");
    } else {
        record["screens"].erase (screenKey);
        if (record["screens"].empty ()) record.erase ("screens");
    }
    if (record.empty ()) data.erase (kUnparsableTextRecord);
}
} // namespace

LocalStorageObject::LocalStorageObject (JSContext* context, const Project& project,
                                        std::function<bool ()> evaluatingTopLevel) :
    LocalStorageObject (context, defaultStateRoot (), wallpaperIdentity (project), project.storageScreenKey,
                        std::move (evaluatingTopLevel)) { }

LocalStorageObject::LocalStorageObject (JSContext* context, std::filesystem::path stateRoot,
                                        std::string identity, std::string screenKey,
                                        std::function<bool ()> evaluatingTopLevel) :
    m_context (context), m_path (std::move (stateRoot) / (stableHash (identity) + ".json")),
    m_screenKey (std::move (screenKey)), m_evaluatingTopLevel (std::move (evaluatingTopLevel)),
    m_instanceId (++nextInstanceId) {
    if (m_instanceId == 0) throw std::overflow_error ("too many SceneScript storage objects");
    instances.emplace (m_instanceId, this);
}

LocalStorageObject::~LocalStorageObject () { instances.erase (m_instanceId); }

JSValue LocalStorageObject::instance () const {
    JSValue object = JS_NewObject (m_context);
    JS_SetPropertyStr (m_context, object, "LOCATION_GLOBAL", JS_NewString (m_context, "global"));
    JS_SetPropertyStr (m_context, object, "LOCATION_SCREEN", JS_NewString (m_context, "screen"));
    JSValue owner[] = {JS_NewUint32 (m_context, m_instanceId)};
    constexpr std::array<const char*, 4> names = {"get", "set", "clear", "delete"};
    for (int i = 0; i < 4; ++i)
        JS_SetPropertyStr (m_context, object, names[i],
            JS_NewCFunctionData (m_context, dispatch, i == 1 ? 3 : i == 2 ? 1 : 2,
                                 i, 1, owner));
    JS_FreeValue (m_context, owner[0]);
    return object;
}

JSValue LocalStorageObject::dispatch (JSContext* context, JSValueConst, int argc,
                                      JSValueConst* argv, int operation, JSValueConst* data) {
    uint32_t id = 0;
    if (JS_ToUint32 (context, &id, data[0]) < 0) return JS_EXCEPTION;
    const auto found = instances.find (id);
    if (found == instances.end ()) return JS_ThrowTypeError (context, "localStorage owner no longer exists");
    return found->second->invoke (context, argc, argv, operation);
}

JSValue LocalStorageObject::invoke (JSContext* context, int argc, JSValueConst* argv, int operation) {
    enum Operation { Get = 0, Set = 1, Clear = 2, Delete = 3 };
    // Messages are verbatim native 2.8.42 text, including the shared key error.
    constexpr std::array<const char*, 4> topLevelErrors = {
        "LocalStorageGet cannot be cleared from global scope.",
        "LocalStorageSet cannot be cleared from global scope.",
        "LocalStorageClear cannot be cleared from global scope.",
        "LocalStorageDelete cannot be cleared from global scope.",
    };
    const auto argument = [argc, argv] (int index) -> JSValueConst {
        return index < argc ? argv[index] : JS_UNDEFINED;
    };
    try {
        if (m_evaluatingTopLevel && m_evaluatingTopLevel ())
            return throwStorageError (context, topLevelErrors[operation]);
        if (operation != Clear && !JS_IsString (argument (0)))
            return throwStorageError (context, "LocalStorageSet key not a string.");
        int locationArg = operation == Set ? 2 : operation == Clear ? 0 : 1;
        std::string serialized;
        std::optional<size_t> unparsableLength;
        if (operation == Set) {
            if (JS_IsUndefined (argument (1))) {
                // Native deletes instead, passing its own arguments so the
                // value slot is read as the location (always the screen
                // namespace), and returns the delete's result.
                operation = Delete;
                locationArg = 1;
            } else {
                JSValue encoded = JS_JSONStringify (context, argument (1), JS_UNDEFINED, JS_UNDEFINED);
                if (JS_IsException (encoded)) {
                    // Native catches the exception and stores empty text.
                    JSValue exception = JS_GetException (context);
                    if (JS_IsUncatchableError (exception)) return JS_Throw (context, exception);
                    JS_FreeValue (context, exception);
                    unparsableLength = 0;
                } else if (JS_IsUndefined (encoded)) {
                    unparsableLength = std::char_traits<char>::length ("undefined");
                } else {
                    WallpaperEngine::Data::Utils::ScopeGuard releaseEncoded ([context, encoded] {
                        JS_FreeValue (context, encoded);
                    });
                    serialized = toString (context, encoded);
                }
            }
        }
        // A window has no native screen instance, so its screen namespace is
        // the global one (an empty screen key).
        const bool global = m_screenKey.empty () || (JS_IsString (argument (locationArg))
            && toCString (context, argument (locationArg)) == "global");
        const std::string key = operation == Clear ? "" : toCString (context, argument (0));
        std::filesystem::create_directories (m_path.parent_path ());
        LockedFile lock (m_path.string () + ".lock");
        json data = readData (m_path);
        json& namespaceData = global ? data["global"] : data["screens"][m_screenKey];
        if (!namespaceData.is_object ()) namespaceData = json::object ();
        if (operation == Get) {
            const auto found = namespaceData.find (key);
            if (found == namespaceData.end ()) return JS_UNDEFINED;
            const std::string stored = found->dump ();
            return JS_ParseJSON (context, stored.c_str (), stored.size (), "localStorage");
        }
        if (operation == Set) {
            const json* lengths = unparsableTextLengths (data, global, m_screenKey);
            size_t total = kEntryHeaderBytes + unparsableLength.value_or (serialized.size ());
            for (const auto& entry : namespaceData.items ()) {
                if (entry.key () == key) continue;
                const auto recorded = recordedLength (lengths, entry.key ());
                total += kEntryHeaderBytes + (recorded ? *recorded : storedTextSize (context, entry.value ()));
            }
            if (total > kMaxNamespaceBytes)
                return throwStorageError (context, "LocalStorageSet failed, possibly out of memory.");
            namespaceData[key] = unparsableLength ? json (nullptr) : json::parse (serialized);
        }
        if (operation == Clear) namespaceData.clear ();
        bool deleted = false;
        if (operation == Delete) deleted = namespaceData.erase (key) != 0;
        if (operation != Delete || deleted) {
            // Last, as it may add top-level members and invalidate namespaceData.
            recordUnparsableText (data, global, m_screenKey, operation == Clear ? nullptr : &key,
                                  operation == Set ? unparsableLength : std::nullopt);
            writeData (m_path, data);
        }
        if (operation == Set) return JS_UNDEFINED;
        return JS_NewBool (context, operation == Clear || deleted);
    } catch (const std::exception& error) {
        return JS_ThrowInternalError (context, "%s", error.what ());
    }
}
