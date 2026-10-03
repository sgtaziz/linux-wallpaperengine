#pragma once

extern "C" {
#include "quickjs.h"
}

#include <string>

namespace WallpaperEngine::Data::Model { class DynamicValue; }
namespace WallpaperEngine::Scripting {
class ScriptableObject;
// Installs the current property's animation accessor on its distinct module owner.
void attachPropertyAnimation (JSContext* context, JSValueConst owner, JSValueConst layer,
                              Data::Model::DynamicValue& value);
/** An IAnimation for a timeline-animated property of a live layer wrapper. */
JSValue createPropertyAnimationObject (JSContext* context, JSValueConst layer, Data::Model::DynamicValue& value);
/** The object's registered property whose timeline animation has this name, or nullptr. */
Data::Model::DynamicValue* findObjectAnimation (const ScriptableObject& object, const std::string& name);
}
