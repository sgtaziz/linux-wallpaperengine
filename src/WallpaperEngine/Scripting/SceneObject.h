#pragma once
#include "quickjs.h"

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}
namespace WallpaperEngine::Scripting {
class ScriptEngine;
class SceneObject {
public:
    SceneObject (ScriptEngine& engine, Render::Wallpapers::CScene& scene);
    ~SceneObject ();

    const Render::Wallpapers::CScene& getScene () const { return m_scene; }
    Render::Wallpapers::CScene& getMutableScene () { return m_scene; }
    JSValue getInstance () const { return m_instance; }
    JSClassID getClassId () const { return m_classId; }
    JSClassID getModelDataClassId () const { return m_modelDataClassId; }
    ScriptEngine& getEngine () const { return m_engine; }

private:
    Render::Wallpapers::CScene& m_scene;
    ScriptEngine& m_engine;

    JSClassID m_classId;
    JSClassID m_modelDataClassId;
    JSClassDef m_definition;
    JSValue m_instance;
};
}
