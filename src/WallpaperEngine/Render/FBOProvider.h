#pragma once

#include <glm/vec2.hpp>

#include "CFBO.h"
#include "WallpaperEngine/Data/Model/Effect.h"

namespace WallpaperEngine::Render {
using namespace WallpaperEngine::Data::Model;

class FBOProvider {
public:
    explicit FBOProvider (FBOProvider* parent,
                          TextureFormat backbufferFormat = TextureFormat_ARGB8888);

    /** Select scene backing storage before acquiring any scene or effect target. */
    void setBackbufferFormat (TextureFormat format);

    std::shared_ptr<CFBO> create (const FBO& base, uint32_t flags, glm::vec2 size);
    std::shared_ptr<CFBO> create (
	const std::string& name, TextureFormat format, uint32_t flags, float scale, glm::vec2 realSize,
	glm::vec2 textureSize
    );
    std::shared_ptr<CFBO> alias (const std::string& newName, const std::string& original);
    void swap (const std::string& first, const std::string& second);
    [[nodiscard]] std::shared_ptr<CFBO> find (const std::string& name) const;
    /** Remove only this provider's mapping when it still owns the expected target. */
    bool eraseIfMappedTo (const std::string& name, const std::shared_ptr<CFBO>& expected);

private:
    FBOProvider* root ();
    FBOProvider* m_parent;
    TextureFormat m_backbufferFormat;
    std::map<std::string, std::shared_ptr<CFBO>> m_fbos = {};
    // Scene-root cache for authored targets whose descriptor requests a
    // stable name. Local logical mappings may subsequently be swapped.
    std::map<std::string, std::weak_ptr<CFBO>> m_uniqueTargets = {};
    // Effect descriptors have identity independent of their authored name:
    // two descriptors can deliberately acquire the same unique target. The
    // caller must retain each FBO object at a stable address while a provider
    // can be refreshed; CImage's Effect model owns those descriptors.
    std::map<const FBO*, std::shared_ptr<CFBO>> m_authoredTargets = {};
};
}
