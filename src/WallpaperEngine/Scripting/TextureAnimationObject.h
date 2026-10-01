#pragma once

#include "Adapters/ObjectInstanceCache.h"

#include <memory>

namespace WallpaperEngine::Render {
class ImageTextureAnimation;
}

namespace WallpaperEngine::Scripting {
/** SceneScript handle identity belongs to the image, rather than its shared texture. */
class TextureAnimationObject {
public:
    explicit TextureAnimationObject (JSContext* context);
    JSValue instantiate (const void* layer, const std::shared_ptr<Render::ImageTextureAnimation>& animation);
    void forgetInstance (const void* layer) { m_instances.forget (layer); }
    void releaseInstances () { m_instances.clear (); }

private:
    JSContext* m_context;
    JSClassID m_classId;
    JSClassExoticMethods m_exoticMethods {};
    Adapters::ObjectInstanceCache m_instances;
};
}
