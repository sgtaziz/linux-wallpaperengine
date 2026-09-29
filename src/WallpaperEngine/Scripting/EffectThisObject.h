#pragma once

#include <cstddef>
#include "quickjs.h"

namespace WallpaperEngine::Scripting {
/** The property owner for a script attached to an image effect. */
JSValue createEffectThisObject (JSContext* context, JSValueConst layer, size_t effectIndex);
}
