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

    void tick ();
    void cancelTimersForObject (const ScriptableObject& object) { m_timers.cancelOwner (object); }

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
