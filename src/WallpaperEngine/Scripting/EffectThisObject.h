#pragma once

#include <cstddef>
#include <vector>
#include "quickjs.h"
#include "WallpaperEngine/Data/Model/Object.h"

namespace WallpaperEngine::Scripting {
class ScriptableObject;
/** The effect list of an image or text layer, or nullptr for other layers. */
const std::vector<Data::Model::ImageEffectUniquePtr>* layerEffects (const ScriptableObject* layer);
/** The property owner for a script attached to an image effect. */
JSValue createEffectThisObject (JSContext* context, JSValueConst layer, size_t effectIndex);
}
