#pragma once

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "ExactSourceCache.h"

namespace WallpaperEngine::Render::Shaders {
class GLSLContext {
public:
    /**
     * Types of shaders
     */
    enum UnitType { UnitType_Vertex = 0, UnitType_Fragment = 1 };

    GLSLContext ();
    ~GLSLContext ();

    [[nodiscard]] std::pair<std::string, std::string> toGlsl (const std::string& vertex, const std::string& fragment);
    [[nodiscard]] size_t translationCacheEntries () const { return m_translations.size (); }
    [[nodiscard]] size_t translationCacheBytes () const { return m_translations.bytes (); }

    [[nodiscard]] static GLSLContext& get ();

private:
    ExactSourceCache m_translations {128, 32 * 1024 * 1024};
    static std::unique_ptr<GLSLContext> sInstance;
};
} // namespace WallpaperEngine::Render::Shaders
