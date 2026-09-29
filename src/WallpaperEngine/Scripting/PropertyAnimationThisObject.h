#pragma once

extern "C" {
#include "quickjs.h"
}

namespace WallpaperEngine::Data::Model { class DynamicValue; }
namespace WallpaperEngine::Scripting {
// Installs the current property's animation accessor on its distinct module owner.
void attachPropertyAnimation (JSContext* context, JSValueConst owner, JSValueConst layer,
                              Data::Model::DynamicValue& value);
}
