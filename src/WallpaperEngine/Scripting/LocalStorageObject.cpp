#include "LocalStorageObject.h"

#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Utils/ScopeGuard.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

using namespace WallpaperEngine::Scripting;
using WallpaperEngine::Data::Model::Project;
using json = nlohmann::json;

namespace {
constexpr size_t kMaxWallpaperBytes = 100 * 1024;
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
    if (serialized.size () > kMaxWallpaperBytes)
        throw std::length_error ("SceneScript storage exceeds 100 KB");
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
} // namespace

LocalStorageObject::LocalStorageObject (JSContext* context, const Project& project) :
    LocalStorageObject (context, defaultStateRoot (), wallpaperIdentity (project), project.storageScreenKey) { }

LocalStorageObject::LocalStorageObject (JSContext* context, std::filesystem::path stateRoot,
                                        std::string identity, std::string screenKey) :
    m_context (context), m_path (std::move (stateRoot) / (stableHash (identity) + ".json")),
    m_screenKey (std::move (screenKey)), m_instanceId (++nextInstanceId) {
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
    try {
        if (operation != 2 && argc < 1) return JS_ThrowTypeError (context, "localStorage key is required");
        if (operation == 1 && argc < 2) return JS_ThrowTypeError (context, "localStorage value is required");
        const int locationArg = operation == 1 ? 2 : operation == 2 ? 0 : 1;
        std::string location = "screen";
        if (argc > locationArg && !JS_IsUndefined (argv[locationArg]))
            location = toString (context, argv[locationArg]);
        if (location != "screen" && location != "global")
            return JS_ThrowRangeError (context, "invalid localStorage location");
        const std::string key = operation == 2 ? "" : toString (context, argv[0]);
        json value;
        if (operation == 1) {
            JSValue encoded = JS_JSONStringify (context, argv[1], JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException (encoded)) return encoded;
            if (JS_IsUndefined (encoded)) return JS_ThrowTypeError (context, "localStorage value is not serializable");
            WallpaperEngine::Data::Utils::ScopeGuard releaseEncoded ([context, encoded] {
                JS_FreeValue (context, encoded);
            });
            const std::string serialized = toString (context, encoded);
            value = json::parse (serialized);
        }
        std::filesystem::create_directories (m_path.parent_path ());
        LockedFile lock (m_path.string () + ".lock");
        json data = readData (m_path);
        json& namespaceData = location == "global" ? data["global"] : data["screens"][m_screenKey];
        if (!namespaceData.is_object ()) namespaceData = json::object ();
        if (operation == 0) {
            const auto found = namespaceData.find (key);
            if (found == namespaceData.end ()) return JS_NULL;
            const std::string serialized = found->dump ();
            return JS_ParseJSON (context, serialized.c_str (), serialized.size (), "localStorage");
        }
        if (operation == 1) namespaceData[key] = std::move (value);
        if (operation == 2) namespaceData.clear ();
        bool deleted = false;
        if (operation == 3) deleted = namespaceData.erase (key) != 0;
        if (operation != 3 || deleted) writeData (m_path, data);
        return operation == 3 ? JS_NewBool (context, deleted) : JS_UNDEFINED;
    } catch (const std::exception& error) {
        return JS_ThrowInternalError (context, "%s", error.what ());
    }
}
