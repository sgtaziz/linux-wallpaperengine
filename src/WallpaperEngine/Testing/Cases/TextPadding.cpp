#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"

#include <stdexcept>
#include <string>

TEST_CASE ("Text padding retains scalar and authored horizontal/vertical forms", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto parse = [&] (const char* padding) {
	const std::string source = std::string (R"({"id":79,"name":"text","text":"",)")
				   + padding + "}";
	auto object = ObjectParser::parse (JSON::parse (source), project);
	return dynamic_cast<Text&> (*object).padding->value->getVec2 ();
    };

    const auto scalar = parse (R"("padding":24)");
    REQUIRE (scalar.x == 24.0f);
    REQUIRE (scalar.y == 24.0f);
    const auto vector = parse (R"("padding":"32.00000 24.00000")");
    REQUIRE (vector.x == 32.0f);
    REQUIRE (vector.y == 24.0f);
    const auto zero = parse (R"("padding":"0.00000 0.00000")");
    REQUIRE (zero.x == 0.0f);
    REQUIRE (zero.y == 0.0f);
    const auto absent = parse (R"("other":true)");
    REQUIRE (absent.x == 32.0f);
    REQUIRE (absent.y == 32.0f);
}

TEST_CASE ("Malformed text padding reports its field without aborting", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto parse = [&] (const char* value) {
	const std::string source = std::string (R"({"id":79,"name":"text","text":"","padding":)")
				   + value + "}";
	return ObjectParser::parse (JSON::parse (source), project);
    };
    for (const char* malformed : {R"("32")", R"("32 24 8")", R"("left right")", R"([32,24])"}) {
	try {
	    parse (malformed);
	    FAIL ("Malformed text padding should fail");
	} catch (const std::invalid_argument& error) {
	    REQUIRE (std::string (error.what ()).find ("Text padding") != std::string::npos);
	}
    }
}

TEST_CASE ("Text spacing retains the authored two-axis value", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto object = ObjectParser::parse (JSON::parse (
        R"({"id":79,"name":"text","text":"dbp\nq","spacing":"8 16"})"), project);
    const auto& text = dynamic_cast<const Text&> (*object);
    REQUIRE (text.spacing->value->getVec2 () == glm::vec2 (8.0f, 16.0f));
    auto defaultObject = ObjectParser::parse (JSON::parse (
        R"({"id":80,"name":"default text","text":"dbp"})"), project);
    REQUIRE (dynamic_cast<const Text&> (*defaultObject).spacing->value->getVec2 () == glm::vec2 (0.0f));
}

TEST_CASE ("Text maxrows retains native numeric default behind its disabled gate", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto capped = ObjectParser::parse (JSON::parse (
        R"({"id":81,"name":"capped","text":"A\nB","limitrows":true,"maxrows":1})"), project);
    REQUIRE (dynamic_cast<const Text&> (*capped).limitRows->value->getBool ());
    REQUIRE (dynamic_cast<const Text&> (*capped).maxRows->value->getInt () == 1);
    auto unlimited = ObjectParser::parse (JSON::parse (
        R"({"id":82,"name":"unlimited","text":"A\nB"})"), project);
    REQUIRE (dynamic_cast<const Text&> (*unlimited).maxRows->value->getInt () == 1);
    REQUIRE_FALSE (dynamic_cast<const Text&> (*unlimited).limitRows->value->getBool ());
    auto gateOnly = ObjectParser::parse (JSON::parse (
        R"({"id":87,"name":"gate only","text":"A\nB","limitrows":true})"), project);
    REQUIRE (dynamic_cast<const Text&> (*gateOnly).limitRows->value->getBool ());
    REQUIRE (dynamic_cast<const Text&> (*gateOnly).maxRows->value->getInt () == 1);
}

TEST_CASE ("Text maxwidth retains native numeric default behind its disabled gate", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto wrapped = ObjectParser::parse (JSON::parse (
        R"({"id":83,"name":"wrapped","text":"small words","limitwidth":true,"maxwidth":72.5})"), project);
    REQUIRE (dynamic_cast<const Text&> (*wrapped).limitWidth->value->getBool ());
    REQUIRE (dynamic_cast<const Text&> (*wrapped).maxWidth->value->getFloat () == 72.5f);
    auto unlimited = ObjectParser::parse (JSON::parse (
        R"({"id":84,"name":"unlimited","text":"small words"})"), project);
    REQUIRE (dynamic_cast<const Text&> (*unlimited).maxWidth->value->getFloat () == 500.0f);
    REQUIRE_FALSE (dynamic_cast<const Text&> (*unlimited).limitWidth->value->getBool ());
    auto gateOnly = ObjectParser::parse (JSON::parse (
        R"({"id":88,"name":"gate only","text":"small words","limitwidth":true})"), project);
    REQUIRE (dynamic_cast<const Text&> (*gateOnly).limitWidth->value->getBool ());
    REQUIRE (dynamic_cast<const Text&> (*gateOnly).maxWidth->value->getFloat () == 500.0f);
}

TEST_CASE ("Text row-limit ellipsis is an authored opt-in flag", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto enabled = ObjectParser::parse (JSON::parse (
        R"({"id":85,"name":"ellipsis","text":"A\nB","limituseellipsis":true})"), project);
    REQUIRE (dynamic_cast<const Text&> (*enabled).limitUseEllipsis->value->getBool ());
    auto disabled = ObjectParser::parse (JSON::parse (
        R"({"id":86,"name":"plain","text":"A\nB"})"), project);
    REQUIRE_FALSE (dynamic_cast<const Text&> (*disabled).limitUseEllipsis->value->getBool ());
}

TEST_CASE ("Text opaque background retains its gate and RGB independently of padding", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto opaque = ObjectParser::parse (JSON::parse (
        R"({"id":89,"name":"background","text":"A","opaquebackground":true,"backgroundcolor":"255 0 0","padding":12})"), project);
    const auto& text = dynamic_cast<const Text&> (*opaque);
    REQUIRE (text.opaqueBackground->value->getBool ());
    REQUIRE (text.padding->value->getVec2 () == glm::vec2 (12.0f));
    REQUIRE (text.backgroundColor->value->getVec4 ().r == 1.0f);
    REQUIRE (text.backgroundColor->value->getVec4 ().g == 0.0f);
    auto plain = ObjectParser::parse (JSON::parse (
        R"({"id":90,"name":"plain","text":"A"})"), project);
    REQUIRE_FALSE (dynamic_cast<const Text&> (*plain).opaqueBackground->value->getBool ());
    REQUIRE (dynamic_cast<const Text&> (*plain).padding->value->getVec2 () == glm::vec2 (32.0f));
}

TEST_CASE ("Text padding setting preserves a scripted two-axis value", "[scene][parser]") {
    using WallpaperEngine::Data::JSON::JSON;
    using WallpaperEngine::Data::Model::Project;
    using WallpaperEngine::Data::Model::Text;
    using WallpaperEngine::Data::Parsers::ObjectParser;
    Project project {};
    auto object = ObjectParser::parse (JSON::parse (
        R"({"id":91,"name":"padding script","text":"A","padding":{"value":12,"script":"export function update(v) { return v; }"}})"), project);
    const auto& text = dynamic_cast<const Text&> (*object);
    REQUIRE (text.padding->value->getVec2 () == glm::vec2 (12.0f));
    REQUIRE (text.padding->value->getScriptSource ().has_value ());
}
