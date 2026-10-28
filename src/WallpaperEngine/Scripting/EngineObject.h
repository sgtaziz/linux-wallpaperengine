#pragma once
#include "EngineTimers.h"
#include "quickjs.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class ScriptableObject;
/**
 * Marks a cursorClick, cursorDown or cursorUp callback on one context, the
 * only callbacks in which native accepts openUserShortcut
 * (2.7.3 accepts callback phases 0xb..0xd; dispatches
 * those as the cursor event payload callbacks after cursorMove).
 */
class UserCommandScope {
public:
    explicit UserCommandScope (JSContext* context);
    ~UserCommandScope ();
    UserCommandScope (const UserCommandScope&) = delete;
    UserCommandScope& operator= (const UserCommandScope&) = delete;

    static bool active (JSContext* context);
    /** True for the authored callbacks that open a user command scope. */
    static bool allows (const char* callback);

private:
    JSContext* m_previous;
};

/**
 * engine.runtime: native asks the host and answers with the scene root clock
 * that zeroes with the scene and advances by each frame's delta
 * before the scripts update. Load time and paused time are never included.
 */
template <typename Scene> double engineRuntime (const Scene& scene) { return scene.getSceneTime (); }

class EngineObject {
public:
    EngineObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~EngineObject ();

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    JSValue getInstance () const { return m_instance; }
    ScriptEngine& getEngine () const { return m_engine; }
    uint32_t getInstanceId () const { return m_timers.instanceId (); }
    [[nodiscard]] std::optional<std::string> registeredAssetPath (JSValueConst value) const;
    [[nodiscard]] bool registeredAssetPrecached (JSValueConst value) const;
    [[nodiscard]] JSClassID getClassId () const { return m_classId; }
    [[nodiscard]] JSClassID getAssetHandleClassId () const { return m_assetHandleClassId; }
    JSValue registerAudioBuffers (uint32_t resolution);

    /** Refreshes registered audio buffers and advances timers by the scene delta (seconds). */
    void tick (float delta);
    void cancelTimersForObject (const ScriptableObject& object) { m_timers.cancelOwner (object); }
    const EngineTimers& getTimers () const { return m_timers; }

protected:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;
    EngineTimers m_timers;
    JSClassID m_classId;
    JSClassDef m_definition;
    JSValue m_instance;
    JSClassID m_assetHandleClassId;
    JSClassDef m_assetHandleDefinition;
    struct AudioBuffers {
        uint32_t resolution;
        std::array<JSValue, 3> arrays; // left, right, average; retained until context teardown.
    };
    std::vector<AudioBuffers> m_audioBuffers;
};
}
