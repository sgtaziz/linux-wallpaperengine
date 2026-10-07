#pragma once

#include <filesystem>
#include <functional>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Data::Model { struct Project; }

namespace WallpaperEngine::Scripting {

// The on-disk value is one JSON object with global and per-display namespaces.
// Each operation reloads under a file lock so independently rendered displays
// can share LOCATION_GLOBAL without losing each other's writes.
// evaluatingTopLevel reports module top-level evaluation, where native 2.8.42
// rejects every localStorage call.
class LocalStorageObject {
public:
    LocalStorageObject (JSContext* context, const Data::Model::Project& project,
                        std::function<bool ()> evaluatingTopLevel);
    LocalStorageObject (JSContext* context, std::filesystem::path stateRoot,
                        std::string wallpaperIdentity, std::string screenKey,
                        std::function<bool ()> evaluatingTopLevel = {});
    ~LocalStorageObject ();
    LocalStorageObject (const LocalStorageObject&) = delete;
    LocalStorageObject& operator= (const LocalStorageObject&) = delete;

    JSValue instance () const;
    const std::filesystem::path& path () const { return m_path; }

private:
    static JSValue dispatch (JSContext* context, JSValueConst thisValue, int argc,
                             JSValueConst* argv, int magic, JSValueConst* data);
    JSValue invoke (JSContext* context, int argc, JSValueConst* argv, int operation);

    JSContext* m_context;
    std::filesystem::path m_path;
    std::string m_screenKey;
    std::function<bool ()> m_evaluatingTopLevel;
    unsigned m_instanceId;
};

} // namespace WallpaperEngine::Scripting
