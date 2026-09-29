#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"

#include <algorithm>
#include <set>
#include <string>

using WallpaperEngine::Data::Builders::UserSettingBuilder;
using WallpaperEngine::Data::Model::DynamicValue;
using WallpaperEngine::Data::Model::Image;
using WallpaperEngine::Data::Model::ImageData;
using WallpaperEngine::Data::Model::ObjectData;
using WallpaperEngine::Data::Model::Particle;
using WallpaperEngine::Data::Model::ParticleData;
using WallpaperEngine::Data::Model::Text;
using WallpaperEngine::Data::Model::TextData;
using WallpaperEngine::Scripting::scriptPropertyBindings;

namespace {
ObjectData baseObject () {
    ObjectData base {};
    base.id = 42;
    base.name = "typed object";
    base.origin = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    base.groupScale = UserSettingBuilder::fromValue (glm::vec3 (9.0f));
    base.groupAngles = UserSettingBuilder::fromValue (glm::vec3 (8.0f));
    base.groupVisible = UserSettingBuilder::fromValue (false);
    return base;
}

template <size_t N>
DynamicValue* binding (const std::array<WallpaperEngine::Scripting::ScriptPropertyBinding, N>& bindings,
		       const std::string& name) {
    const auto it = std::find_if (bindings.begin (), bindings.end (), [&] (const auto& entry) {
	return name == entry.name;
    });
    return it == bindings.end () ? nullptr : &it->value;
}

template <size_t N>
std::set<std::string> names (const std::array<WallpaperEngine::Scripting::ScriptPropertyBinding, N>& bindings) {
    std::set<std::string> result;
    for (const auto& entry : bindings) result.insert (entry.name);
    return result;
}
} // namespace

TEST_CASE ("Image script bindings update the typed values consumed by rendering", "[script][binding]") {
    ImageData data {};
    data.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    data.angles = UserSettingBuilder::fromValue (glm::vec3 (2.0f));
    data.visible = UserSettingBuilder::fromValue (true);
    data.alpha = UserSettingBuilder::fromValue (0.5f);
    data.color = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    data.brightness = UserSettingBuilder::fromValue (1.0f);
    data.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    data.size = UserSettingBuilder::fromValue (glm::vec2 (32.0f, 24.0f));
    Image image (baseObject (), std::move (data));
    const auto bindings = scriptPropertyBindings (image);
    REQUIRE (names (bindings) == std::set<std::string> {"origin", "scale", "angles", "visible",
						       "alpha", "color", "brightness", "parallaxDepth", "size"});
    REQUIRE (binding (bindings, "scale") == image.scale->value.get ());
    REQUIRE (binding (bindings, "scale") != image.groupScale->value.get ());
    REQUIRE (binding (bindings, "visible") == image.visible->value.get ());
    REQUIRE (binding (bindings, "visible") != image.groupVisible->value.get ());
    REQUIRE (binding (bindings, "size") == image.size->value.get ());
    REQUIRE (binding (bindings, "brightness") == image.brightness->value.get ());
    binding (bindings, "brightness")->update (1.2694342f, DynamicValue::Script);
    REQUIRE (image.brightness->value->getFloat () == 1.2694342f);
    binding (bindings, "scale")->update (glm::vec3 (3.0f), DynamicValue::Script);
    REQUIRE (image.scale->value->getVec3 ().x == 3.0f);
    REQUIRE (image.groupScale->value->getVec3 ().x == 9.0f);
    binding (bindings, "size")->update (glm::vec2 (64.0f, 48.0f), DynamicValue::Script);
    REQUIRE (image.size->value->getVec2 () == glm::vec2 (64.0f, 48.0f));
}

TEST_CASE ("Particle and text bindings retain prior names with typed authority", "[script][binding]") {
    ParticleData particleData {};
    particleData.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    particleData.angles = UserSettingBuilder::fromValue (glm::vec3 (2.0f));
    particleData.visible = UserSettingBuilder::fromValue (true);
    particleData.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    Particle particle (baseObject (), std::move (particleData));
    const auto particleBindings = scriptPropertyBindings (particle);
    REQUIRE (names (particleBindings) == std::set<std::string> {"origin", "scale", "angles", "visible", "parallaxDepth"});
    REQUIRE (binding (particleBindings, "scale") == particle.scale->value.get ());
    REQUIRE (binding (particleBindings, "scale") != particle.groupScale->value.get ());
    binding (particleBindings, "visible")->update (false, DynamicValue::Script);
    REQUIRE_FALSE (particle.visible->value->getBool ());
    REQUIRE_FALSE (particle.groupVisible->value->getBool ());

    TextData textData {};
    textData.text = UserSettingBuilder::fromValue (std::string ("hello"));
    textData.pointSize = UserSettingBuilder::fromValue (24.0f);
    textData.spacing = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    textData.limitRows = UserSettingBuilder::fromValue (false);
    textData.maxRows = UserSettingBuilder::fromValue (0);
    textData.limitWidth = UserSettingBuilder::fromValue (false);
    textData.maxWidth = UserSettingBuilder::fromValue (0.0f);
    textData.limitUseEllipsis = UserSettingBuilder::fromValue (false);
    textData.opaqueBackground = UserSettingBuilder::fromValue (false);
    textData.backgroundColor = UserSettingBuilder::fromValue (glm::vec3 (0.0f));
    textData.padding = UserSettingBuilder::fromValue (glm::vec2 (32.0f));
    textData.scale = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    textData.color = UserSettingBuilder::fromValue (glm::vec3 (1.0f));
    textData.alpha = UserSettingBuilder::fromValue (1.0f);
    textData.visible = UserSettingBuilder::fromValue (true);
    textData.parallaxDepth = UserSettingBuilder::fromValue (glm::vec2 (0.0f));
    Text text (baseObject (), std::move (textData));
    const auto textBindings = scriptPropertyBindings (text);
    REQUIRE (names (textBindings) == std::set<std::string> {"origin", "scale", "angles", "visible",
							   "color", "alpha", "pointSize", "spacing", "limitRows", "maxRows",
							   "limitWidth", "maxWidth",
							   "limitUseEllipsis", "opaqueBackground", "backgroundColor", "padding", "text",
							   "parallaxDepth"});
    REQUIRE (binding (textBindings, "parallaxDepth") == text.parallaxDepth->value.get ());
    REQUIRE (binding (textBindings, "scale") == text.scale->value.get ());
    REQUIRE (binding (textBindings, "scale") != text.groupScale->value.get ());
    REQUIRE (binding (textBindings, "angles") == text.groupAngles->value.get ());
    REQUIRE (binding (textBindings, "spacing") == text.spacing->value.get ());
    binding (textBindings, "spacing")->update (glm::vec2 (2.0f, 4.0f), DynamicValue::Script);
    REQUIRE (text.spacing->value->getVec2 () == glm::vec2 (2.0f, 4.0f));
    REQUIRE (binding (textBindings, "limitRows") == text.limitRows->value.get ());
    binding (textBindings, "limitRows")->update (true, DynamicValue::Script);
    REQUIRE (text.limitRows->value->getBool ());
    REQUIRE (binding (textBindings, "maxRows") == text.maxRows->value.get ());
    binding (textBindings, "maxRows")->update (2, DynamicValue::Script);
    REQUIRE (text.maxRows->value->getInt () == 2);
    REQUIRE (binding (textBindings, "limitWidth") == text.limitWidth->value.get ());
    binding (textBindings, "limitWidth")->update (true, DynamicValue::Script);
    REQUIRE (text.limitWidth->value->getBool ());
    REQUIRE (binding (textBindings, "maxWidth") == text.maxWidth->value.get ());
    binding (textBindings, "maxWidth")->update (120.0f, DynamicValue::Script);
    REQUIRE (text.maxWidth->value->getFloat () == 120.0f);
    REQUIRE (binding (textBindings, "limitUseEllipsis") == text.limitUseEllipsis->value.get ());
    binding (textBindings, "limitUseEllipsis")->update (true, DynamicValue::Script);
    REQUIRE (text.limitUseEllipsis->value->getBool ());
    REQUIRE (binding (textBindings, "opaqueBackground") == text.opaqueBackground->value.get ());
    binding (textBindings, "opaqueBackground")->update (true, DynamicValue::Script);
    REQUIRE (text.opaqueBackground->value->getBool ());
    REQUIRE (binding (textBindings, "backgroundColor") == text.backgroundColor->value.get ());
    binding (textBindings, "backgroundColor")->update (glm::vec3 (1.0f, 0.0f, 0.0f), DynamicValue::Script);
    REQUIRE (text.backgroundColor->value->getVec4 ().r == 1.0f);
    REQUIRE (binding (textBindings, "padding") == text.padding->value.get ());
    binding (textBindings, "padding")->update (glm::vec2 (8.0f, 16.0f), DynamicValue::Script);
    REQUIRE (text.padding->value->getVec2 () == glm::vec2 (8.0f, 16.0f));
    binding (textBindings, "text")->update (std::string ("updated"), DynamicValue::Script);
    REQUIRE (text.text->value->getString () == "updated");
}
