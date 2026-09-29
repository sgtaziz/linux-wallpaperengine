#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"

using WallpaperEngine::Assets::AssetLocator;
using WallpaperEngine::FileSystem::Container;
using WallpaperEngine::Render::Shaders::GLSLContext;
using WallpaperEngine::Render::Shaders::ShaderUnit;
using WallpaperEngine::Data::Model::ShaderConstantMap;
using WallpaperEngine::Data::Model::TextureMap;
using WallpaperEngine::Data::Model::ComboMap;

TEST_CASE ("Audio Bars directive repair refuses a changed source with the same path and size",
           "[shader][known-audio-bars]") {
    // A generic unmatched-endif removal could close a real outer branch in a
    // revised Workshop shader. Match the known package bytes before repair.
    std::string changed =
        "void main() {\r\n"
        "#if DEFORMITY == 2\r\n"
        "#endif\r\n"
        "\r\n#endif\r\n\r\n#if TRANSFORM\r\n"
        "#endif\r\n}\r\n";
    REQUIRE (changed.size () < 2565);
    changed.append (2565 - changed.size (), ' ');

    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ShaderConstantMap constants;
    const TextureMap textures;
    const ComboMap combos;
    ShaderUnit unit (GLSLContext::UnitType_Vertex,
                     "workshop/3082978660/effects/Simple_Audio_Bars", changed,
                     assets, constants, textures, textures, combos, combos);
    REQUIRE (unit.compile ().find ("\r\n#endif\r\n\r\n#if TRANSFORM") != std::string::npos);
}

TEST_CASE ("Shader uniforms accept authored JSON comments without a space", "[shader][metadata]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ShaderConstantMap constants;
    const TextureMap textures;
    const ComboMap combos;
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "dot_matrix_mobile_fix",
                     // These are the actual parameter lines in the packaged dot shader.
                     "uniform float alpha;\t//{\"material\":\"Alpha\", \"default\":0.3, \"range\":[0,1]}\n"
                     "uniform float scale; // {\"material\": \"Scale\",\"default\":20,\"range\":[0,50]}\n"
                     "void main() { gl_FragColor = vec4(alpha + scale); }\n",
                     assets, constants, textures, textures, combos, combos);
    REQUIRE (unit.getParameters ().size () == 2);
    REQUIRE (unit.getParameters ()[0]->getIdentifierName () == "Alpha");
    REQUIRE (unit.getParameters ()[0]->getFloat () == 0.3f);
    REQUIRE (unit.getParameters ()[1]->getIdentifierName () == "Scale");
    REQUIRE (unit.getParameters ()[1]->getFloat () == 20.0f);
}
