#pragma once

#include "WallpaperEngine/Data/Model/Types.h"

extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Scripting {
// Shader-bound scripts receive their material constants as thisObject while
// thisLayer remains the containing image layer.
JSValue createMaterialThisObject (JSContext* context, JSValueConst layer,
                                  const Data::Model::ShaderConstantMap& constants);
}
