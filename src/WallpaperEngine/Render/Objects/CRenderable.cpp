#include "CRenderable.h"

#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Parsers/MaterialParser.h"
#include "WallpaperEngine/Render/UserTextureSelection.h"

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Objects::Effects;
using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Builders;

CRenderable::CRenderable (Wallpapers::CScene& scene, const Object& object, const Material& material) :
    CObject (scene, object), Render::FBOProvider (&scene), m_material (material) { }

void CRenderable::detectTexture () {
    if (m_material.passes.empty () || !m_material.passes.front ()) return;
    const auto& pass = *m_material.passes.front ();

    const auto resolveAuthored = [this] (const std::string& name) {
	return name.starts_with ("_rt_") || name.starts_with ("_alias_")
	    ? this->getScene ().findFBO (name) : this->getContext ().resolveTexture (name);
    };
    std::shared_ptr<const TextureProvider> authored;
    if (const auto base = pass.textures.find (0); base != pass.textures.end ())
	authored = resolveAuthored (base->second);

    // ObjectParser has already merged instance textures with insert_or_assign.
    // Apply CPass's selection helper so geometry and animation metadata use its
    // selected-provider and fallback semantics at initial setup.
    if (const auto selector = pass.usertextures.find (0); selector != pass.usertextures.end ()
	&& selector->second.source == Data::Model::UserTextureSource::Ordinary) {
	const auto& properties = getScene ().getScene ().project.properties;
	const auto property = properties.find (selector->second.name);
	const auto* sceneTexture = property == properties.end () ? nullptr
	    : dynamic_cast<const Data::Model::PropertySceneTexture*> (property->second.get ());
	if (sceneTexture) {
	    std::string selectedValue;
	    std::shared_ptr<const TextureProvider> selectedTexture;
	    m_texture = resolveUserTextureSelection (
		*sceneTexture, selectedValue, selectedTexture,
		[this] (const std::string& selected) -> std::shared_ptr<const TextureProvider> {
		    try {
			return getContext ().resolveTexture (selected);
		    } catch (const std::runtime_error&) {
			return {};
		    }
		},
		[&authored] { return authored; }
	    );
	    return;
	}
    }
    m_texture = std::move (authored);
}

void CRenderable::setup () {
    CObject::setup ();

    // calculate full animation time (if any)
    this->m_animationTime = 0.0f;

    for (const auto& cur : this->getTexture ()->getFrames ()) {
	this->m_animationTime += cur->frametime;
    }
}

std::shared_ptr<const TextureProvider> CRenderable::getTexture () const { return this->m_texture; }

double CRenderable::getAnimationTime () const { return this->m_animationTime; }
