#pragma once

#include "quickjs.h"

#include <stdexcept>

namespace WallpaperEngine::Scripting {
// QuickJS allocates class IDs per runtime. Always start with a fresh zero ID;
// reusing an ID from an earlier runtime can collide with its own classes.
inline JSClassID registerSceneScriptClass (JSRuntime* runtime, const JSClassDef& definition) {
    JSClassID id = 0;
    JS_NewClassID (runtime, &id);
    if (!id || JS_NewClass (runtime, id, &definition) < 0)
        throw std::runtime_error ("Cannot register SceneScript JavaScript class");
    return id;
}
}
