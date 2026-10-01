#include <catch2/catch_test_macros.hpp>

#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/Data/Builders/UserSettingBuilder.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"
#include "WallpaperEngine/Render/Shaders/ShaderMetadata.h"
#include "WallpaperEngine/Render/Shaders/Shader.h"
#include "WallpaperEngine/Render/Shaders/ExactSourceCache.h"

using WallpaperEngine::Assets::AssetLocator;
using WallpaperEngine::FileSystem::Container;
using WallpaperEngine::Render::Shaders::GLSLContext;
using WallpaperEngine::Render::Shaders::ShaderUnit;
using WallpaperEngine::Render::Shaders::Shader;
using WallpaperEngine::Data::Model::ShaderConstantMap;
using WallpaperEngine::Data::Model::TextureMap;
using WallpaperEngine::Data::Model::ComboMap;
using WallpaperEngine::Data::Builders::UserSettingBuilder;

static const ShaderConstantMap emptyConstants;
static const TextureMap emptyTextures;
static const ComboMap emptyCombos;

TEST_CASE ("Sprite-trail shader restores native texture corner handedness after Y reflection",
           "[shader][particle][trail]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source =
        "void ComputeParticleTrailTangents(vec3 eyeDirection, vec3 localVelocity, out vec3 right) {\n"
        "  right = cross(eyeDirection, localVelocity);\n"
        "}\n"
        "void main() { vec3 right; ComputeParticleTrailTangents(vec3(0,0,1), vec3(0,1,0), right); gl_Position = vec4(right,1); }\n";
    ComboMap trailCombos;
    trailCombos["TRAILRENDERER"] = 1;
    ShaderUnit trail (GLSLContext::UnitType_Vertex, "genericparticle", source,
                      assets, emptyConstants, emptyTextures, emptyTextures,
                      emptyCombos, trailCombos);
    ShaderUnit ordinary (GLSLContext::UnitType_Vertex, "genericparticle", source,
                         assets, emptyConstants, emptyTextures, emptyTextures,
                         emptyCombos, emptyCombos);
    ShaderUnit unrelated (GLSLContext::UnitType_Vertex, "anotherparticle", source,
                          assets, emptyConstants, emptyTextures, emptyTextures,
                          emptyCombos, trailCombos);
    REQUIRE (trail.compile ().find ("right = -cross(eyeDirection, localVelocity);")
             != std::string::npos);
    REQUIRE (ordinary.compile ().find ("right = -cross(eyeDirection, localVelocity);")
             == std::string::npos);
    REQUIRE (unrelated.compile ().find ("right = -cross(eyeDirection, localVelocity);")
             == std::string::npos);

    // For F=diag(1,-1,1), the unchanged cross gives -F(nativeRight).
    // The rewritten trail branch restores F(nativeRight) at the same UV U.
    const glm::mat3 reflection (glm::vec3 (1, 0, 0), glm::vec3 (0, -1, 0),
                                glm::vec3 (0, 0, 1));
    const glm::vec3 nativeEye (0.3f, 1.0f, -2000.0f);
    const glm::vec3 nativeVelocity (10.0f, 20.0f, 0.0f);
    const glm::vec3 nativeRight = glm::normalize (glm::cross (nativeEye, nativeVelocity));
    const glm::vec3 corrected = -glm::normalize (glm::cross (
        reflection * nativeEye, reflection * nativeVelocity));
    REQUIRE (glm::length (corrected - reflection * nativeRight) < 1e-6f);
}

TEST_CASE ("Position metadata retains authored coordinates as an editor annotation",
           "[shader][position]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source =
        "uniform vec2 g_Center; // {\"material\":\"center\",\"position\":true}\n"
        "uniform vec2 g_Offset; // {\"material\":\"offset\",\"default\":\"0.6 0.0\"}\n"
        "void main() { gl_FragColor = vec4(g_Center + g_Offset, 0.0, 1.0); }\n";
    ShaderConstantMap passConstants;
    passConstants.emplace ("center", UserSettingBuilder::fromValue (glm::vec2 (0.25f, 0.2f)));
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "position-role.frag", source,
                     assets, passConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (unit.getParameters ().size () == 2);
    const auto* center = unit.getParameters ()[0];
    const auto* offset = unit.getParameters ()[1];
    REQUIRE (center->isPosition ());
    REQUIRE_FALSE (offset->isPosition ());
    REQUIRE (center->getVec2 () == glm::vec2 (0.25f, 0.2f));
    // Native position/plain intermediate probes both upload the raw center.
    // UV orientation belongs to image geometry, not to metadata-driven values.
    REQUIRE (center->getVec2 () == glm::vec2 (0.25f, 0.2f));

    ShaderConstantMap overrideConstants;
    overrideConstants.emplace ("center", UserSettingBuilder::fromValue (glm::vec2 (0.1f, 0.9f)));
    ShaderUnit overridden (GLSLContext::UnitType_Fragment, "position-role-override.frag", source,
                           assets, overrideConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (overridden.getParameters ()[0]->isPosition ());
    REQUIRE (overridden.getParameters ()[0]->getVec2 () == glm::vec2 (0.1f, 0.9f));

    ShaderUnit defaulted (GLSLContext::UnitType_Fragment, "position-role-default.frag",
                          "uniform vec2 g_Center; // {\"material\":\"center\",\"default\":\"0.6 0.0\",\"position\":true}\n"
                          "void main() { gl_FragColor = vec4(g_Center, 0.0, 1.0); }\n",
                          assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (defaulted.getParameters ()[0]->isPosition ());
    REQUIRE (defaulted.getParameters ()[0]->getVec2 () == glm::vec2 (0.6f, 0.0f));
}

TEST_CASE ("Shader combo require metadata does not gate runtime authored values", "[shader][combo-require]") {
    // Native 14016ce60's COMBO branch reads combo/default into the runtime
    // table; `require` is editor visibility metadata. The actual authored
    // LIGHTING/RIMLIGHTING pair must survive even when LIGHTING is zero.
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source =
        "// [COMBO] {\"combo\":\"LIGHTING\",\"default\":0}\n"
        "// [COMBO] {\"combo\":\"RIMLIGHTING\",\"default\":1,\"require\":{\"LIGHTING\":1}}\n"
        "void main() {\n#if RIMLIGHTING\ngl_FragColor = vec4(1.0,0.0,0.0,1.0);\n"
        "#else\ngl_FragColor = vec4(0.0,0.0,1.0,1.0);\n#endif\n}\n";
    ShaderUnit defaultUnit (GLSLContext::UnitType_Fragment, "combo-require.frag", source,
                            assets, emptyConstants, emptyTextures, emptyTextures,
                            emptyCombos, emptyCombos);
    const auto& defaults = defaultUnit.compile ();
    REQUIRE (defaultUnit.getDiscoveredCombos ().at ("RIMLIGHTING") == 1);
    REQUIRE (defaults.find ("#define RIMLIGHTING 1") != std::string::npos);

    const ComboMap authored {{"LIGHTING", 0}, {"RIMLIGHTING", 1}};
    ShaderUnit authoredUnit (GLSLContext::UnitType_Fragment, "combo-require-authored.frag", source,
                             assets, emptyConstants, emptyTextures, emptyTextures,
                             authored, emptyCombos);
    const auto& compiled = authoredUnit.compile ();
    REQUIRE (compiled.find ("#define LIGHTING 0") != std::string::npos);
    REQUIRE (compiled.find ("#define RIMLIGHTING 1") != std::string::npos);
}

TEST_CASE ("Shipped sampler dependencies preserve authored permutations", "[shader][sampler-require]") {
    // The installed sampler declarations with both combo and require metadata
    // have no default texture. Native 14016ce60 reads their inline combo but
    // does not inspect require/requireany on the runtime parse path.
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source =
        "uniform sampler2D g_Texture1; // {\"combo\":\"NORMALMAP\",\"requireany\":true,"
        "\"require\":{\"LIGHTING\":1,\"REFLECTION\":1}}\n"
        "void main() {\n#if NORMALMAP\ngl_FragColor = vec4(1.0);\n"
        "#else\ngl_FragColor = vec4(0.0);\n#endif\n}\n";
    for (const int lighting : {0, 1}) {
        for (const int normalmap : {0, 1}) {
            for (const bool texturePresent : {false, true}) {
                const ComboMap authored {{"LIGHTING", lighting}, {"REFLECTION", 0},
                                         {"NORMALMAP", normalmap}};
                const TextureMap textures = texturePresent ? TextureMap {{1, "util/flatnormal"}}
                                                            : TextureMap {};
                ShaderUnit unit (GLSLContext::UnitType_Fragment, "sampler-require.frag", source,
                                 assets, emptyConstants, textures, emptyTextures,
                                 authored, emptyCombos);
                const auto& compiled = unit.compile ();
                REQUIRE (compiled.find ("#define NORMALMAP " + std::to_string (normalmap))
                         != std::string::npos);
                REQUIRE (unit.getDiscoveredCombos ().contains ("NORMALMAP") == texturePresent);
            }
        }
    }
}

TEST_CASE ("Shader source cache keys both full units and evicts least recently used entries", "[shader][cache]") {
    using WallpaperEngine::Render::Shaders::ExactSourceCache;
    ExactSourceCache cache (2, 40);
    cache.insert ({"ab", "c"}, {"v1", "f1"});
    cache.insert ({"a", "bc"}, {"v2", "f2"});
    REQUIRE (cache.size () == 2);
    REQUIRE (cache.find ({"ab", "c"})->first == "v1");
    REQUIRE (cache.find ({"a", "bc"})->second == "f2");
    cache.find ({"ab", "c"});
    cache.insert ({"x", "y"}, {"v3", "f3"});
    REQUIRE (cache.find ({"a", "bc"}) == nullptr);
    REQUIRE (cache.find ({"ab", "c"})->first == "v1");
    REQUIRE (cache.find ({"x", "y"})->second == "f3");
    cache.insert ({"x", "y"}, {"new", "value"});
    REQUIRE (cache.size () == 2);
    REQUIRE (cache.find ({"x", "y"})->second == "value");
    const auto before = cache.bytes ();
    cache.insert ({std::string (41, 'a'), ""}, {"", ""});
    REQUIRE (cache.bytes () == before);
    REQUIRE (cache.size () == 2);

    ExactSourceCache byteLimited (4, 12);
    byteLimited.insert ({"aa", "bb"}, {"cc", "dd"});
    byteLimited.insert ({"ee", "ff"}, {"gg", "hh"});
    REQUIRE (byteLimited.size () == 1);
    REQUIRE (byteLimited.find ({"aa", "bb"}) == nullptr);
    REQUIRE (byteLimited.find ({"ee", "ff"}) != nullptr);
}

TEST_CASE ("GLSL translation cache reuses successful exact source pairs", "[shader][cache]") {
    auto& context = GLSLContext::get ();
    const std::string vertex = "#version 330\n// translation-cache-test-v\nvoid main() { gl_Position = vec4(1.0); }\n";
    const std::string fragment = "#version 330\n// translation-cache-test-f\nout vec4 color; void main() { color = vec4(1.0); }\n";
    const auto before = context.translationCacheEntries ();
    const auto first = context.toGlsl (vertex, fragment);
    REQUIRE_FALSE (first.first.empty ());
    REQUIRE_FALSE (first.second.empty ());
    REQUIRE (context.translationCacheEntries () == before + 1);
    REQUIRE (context.toGlsl (vertex, fragment) == first);
    REQUIRE (context.translationCacheEntries () == before + 1);
    const auto second = context.toGlsl (vertex, fragment + "// distinct fragment\n");
    REQUIRE_FALSE (second.second.empty ());
    REQUIRE (context.translationCacheEntries () == before + 2);
    const auto third = context.toGlsl (vertex + "// distinct vertex\n", fragment);
    REQUIRE_FALSE (third.first.empty ());
    REQUIRE (context.translationCacheEntries () == before + 3);
    const auto failed = context.toGlsl (vertex, "#version 330\nthis is invalid shader syntax\n");
    REQUIRE (failed.first.empty ());
    REQUIRE (context.translationCacheEntries () == before + 3);
}

TEST_CASE ("Shader includes retain directive order and conditional scope", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/first.h", "#include \"second.h\"\n#define FIRST 1\n");
    files->getVFS ().add ("shaders/second.h", "#define SECOND 2\n");
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Vertex, "conditional.vert",
                     "#if ENABLED\n#include \"first.h\"\n#endif\n"
                     "attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1.0); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& result = unit.compile ();
    const auto condition = result.find ("#if ENABLED");
    const auto nested = result.find ("#define SECOND 2");
    const auto first = result.find ("#define FIRST 1");
    const auto end = result.find ("#endif", condition);
    REQUIRE (condition != std::string::npos);
    REQUIRE (condition < nested);
    REQUIRE (nested < first);
    REQUIRE (first < end);
    REQUIRE (result.find ("#include") == std::string::npos);
}

TEST_CASE ("Shader include parser ignores comments and supports include-only units", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/leaf.h", "#define LEAF 1\n");
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "include-only.frag",
                     "/*\n#include \"missing.h\"\n*/\n// #include \"also-missing.h\"\n"
                     "#include \"leaf.h\"\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& result = unit.compile ();
    REQUIRE (result.find ("#define LEAF 1") != std::string::npos);
    REQUIRE (result.find ("tried including file missing.h") == std::string::npos);
    REQUIRE (result.find ("tried including file also-missing.h") == std::string::npos);
}

TEST_CASE ("Shader includes preserve adjacent multiline comment delimiters", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/leaf.h", "#define LEAF 1\n");
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Vertex, "comments.vert",
                     "/* before\n*/ #include \"leaf.h\" /* after\nstill a comment */\n"
                     "void main() { gl_Position = vec4(1.0); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& result = unit.compile ();
    REQUIRE (result.find ("*/  /* after") != std::string::npos);
    REQUIRE (result.find ("#define LEAF 1") != std::string::npos);
    REQUIRE (result.find ("/* after\nstill a comment */") != std::string::npos);
}

TEST_CASE ("Unconditional shader helpers follow later global uniforms", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/helper.h", "vec4 sampleBase() { return texture(g_Texture0, vec2(0.0)); }\n");
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "globals.frag",
                     "// void main() is a comment\n/*\n#if HIDDEN\n*/\n"
                     "#include \"helper.h\"\nuniform sampler2D g_Texture0;\n"
                     "vec4 authoredHelper() { return sampleBase(); }\n"
                     "void main\n() { gl_FragColor = authoredHelper(); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& result = unit.compile ();
    REQUIRE (result.find ("uniform sampler2D g_Texture0") < result.find ("vec4 sampleBase"));
    REQUIRE (result.find ("vec4 sampleBase") < result.find ("vec4 authoredHelper"));
    const auto compiled = GLSLContext::get ().toGlsl (
        "#version 330\nvoid main() { gl_Position = vec4(1.0); }\n", result);
    REQUIRE_FALSE (compiled.second.empty ());
}

TEST_CASE ("Unresolved active shader include remains diagnosable", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    files->getVFS ().add ("shaders/other.h", "#define OTHER 1\n");
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Vertex, "missing.vert",
                     "#include \"missing.h\"\nvoid main() { gl_Position = vec4(1.0); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (unit.compile ().find ("#include \"missing.h\"") != std::string::npos);
}

TEST_CASE ("Only equivalent preprocessor branches collapse", "[shader][include]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "equivalent.frag",
                     "// #if COMMENTED\n/*\n#if ALSO_COMMENTED\n*/\n"
                     "#if runtimeUniform.x > 0\n#define RATIO 1\n#else\n#define RATIO 1\n#endif\n"
                     "#if DIFFERENT\n#define VALUE 1\n#else\n#define VALUE 2\n#endif\n"
                     "#if NESTED\n#if INNER\n#define N 1\n#endif\n#else\n#define N 1\n#endif\n"
                     "/* prefix\n*/ #if COMMENT_BOUNDARY\n#define C 1\n#else\n#define C 1\n#endif\n"
                     "#if TRAILING_COMMENT\n#define T 1\n#else\n#define T 1\n#endif /* suffix\ncontinued */\n"
                     "void main() { gl_FragColor = vec4(RATIO); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& result = unit.compile ();
    REQUIRE (result.find ("#if runtimeUniform.x") == std::string::npos);
    REQUIRE (result.find ("#define RATIO 1") != std::string::npos);
    REQUIRE (result.find ("#if DIFFERENT") != std::string::npos);
    REQUIRE (result.find ("#if NESTED") != std::string::npos);
    REQUIRE (result.find ("#if ALSO_COMMENTED") != std::string::npos);
    REQUIRE (result.find ("*/ #if COMMENT_BOUNDARY") != std::string::npos);
    REQUIRE (result.find ("#endif /* suffix\ncontinued */") != std::string::npos);

    ShaderUnit lens (GLSLContext::UnitType_Fragment, "lens_distorsion.frag",
                     "uniform vec4 g_Texture0Resolution;\nuniform float g_ratio;\n"
                     "#if g_Texture0Resolution.x < g_Texture0Resolution.y\n"
                     "#define ratioDiff (vec2(g_ratio, 1.0))\n#else\n"
                     "#define ratioDiff (vec2(g_ratio, 1.0))\n#endif\n"
                     "void main() { gl_FragColor = vec4(ratioDiff, 0.0, 1.0); }\n",
                     assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto compiled = GLSLContext::get ().toGlsl (
        "#version 330\nvoid main() { gl_Position = vec4(1.0); }\n", lens.compile ());
    REQUIRE_FALSE (compiled.second.empty ());
}

TEST_CASE ("Writable fragment texture coordinates preserve earlier reads", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "sine_wave.frag",
                         "varying vec2 v_TexCoord;\n"
                         "void main() { vec2 before = v_TexCoord; v_TexCoord.x += 0.25; "
                         "gl_FragColor = vec4(before, v_TexCoord); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& source = fragment.compile ();
    REQUIRE (source.find ("vec2 _weMutableTexCoord = v_TexCoord.xy;") != std::string::npos);
    REQUIRE (source.find ("_weMutableTexCoord.x += 0.25") != std::string::npos);
    REQUIRE (source.find ("vec2 before = _weMutableTexCoord") != std::string::npos);
    const auto compiled = GLSLContext::get ().toGlsl (
        "#version 330\nout vec2 v_TexCoord;\nvoid main() { v_TexCoord = vec2(0.5); gl_Position = vec4(1.0); }\n",
        source);
    REQUIRE_FALSE (compiled.second.empty ());
    const auto linkedWide = GLSLContext::get ().toGlsl (
        "#version 330\nout vec4 v_TexCoord;\nvoid main() { v_TexCoord = vec4(0.5); gl_Position = vec4(1.0); }\n",
        source);
    REQUIRE_FALSE (linkedWide.second.empty ());

    ShaderUnit shadowed (GLSLContext::UnitType_Fragment, "already-local.frag",
                         "varying vec2 v_TexCoord;\n"
                         "void main() { vec2 v_TexCoord = vec2(0.0); v_TexCoord.x += 1.0; "
                         "gl_FragColor = vec4(v_TexCoord, 0.0, 1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (shadowed.compile ().find ("_weMutableTexCoord") == std::string::npos);

    ShaderUnit member (GLSLContext::UnitType_Fragment, "member.frag",
                       "varying vec2 v_TexCoord;\nstruct Coordinates { vec2 v_TexCoord; };\n"
                       "void main() { Coordinates c; c.v_TexCoord = vec2(0.0); "
                       "v_TexCoord.x += 1.0; gl_FragColor = vec4(c.v_TexCoord, 0.0, 1.0); }\n",
                       assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (member.compile ().find ("_weMutableTexCoord") == std::string::npos);
}

TEST_CASE ("Fragment inherits authored conditional bounds varying", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ComboMap mode = {{"MODE", 0}};
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "effects/foliagesway.vert",
                       "#if MODE == 0\nvarying vec2 v_Bounds;\n#endif\n"
                       "void main() { gl_Position = vec4(1.0);\n"
                       "#if MODE == 0\nv_Bounds = vec2(0.0, 1.0);\n#endif\n}\n",
                       assets, emptyConstants, emptyTextures, emptyTextures, mode, emptyCombos);
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "effects/foliagesway.frag",
                         "void main() {\n#if MODE == 0\n"
                         "gl_FragColor = vec4(v_Bounds.x);\n#else\ngl_FragColor = vec4(1.0);\n#endif\n}\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, mode, emptyCombos);
    vertex.linkToUnit (&fragment);
    fragment.linkToUnit (&vertex);
    REQUIRE (fragment.compile ().find ("varying vec2 v_Bounds;") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (vertex.compile (), fragment.compile ()).second.empty ());
}

TEST_CASE ("Vertex vec4 output narrows to authored vec2 input only when upper lanes are unused", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "workshop/test_shader.vert",
                       "attribute vec2 a_TexCoord; varying vec4 v_TexCoord;\n"
                       "void main() { v_TexCoord.xy = a_TexCoord; gl_Position = vec4(1.0); }\n",
                       assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "workshop/test_shader.frag",
                         "varying vec2 v_TexCoord;\n"
                         "void main() { vec2 fragPos = v_TexCoord; gl_FragColor = vec4(fragPos, 0.0, 1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    vertex.linkToUnit (&fragment);
    fragment.linkToUnit (&vertex);
    const auto& narrowed = vertex.compile ();
    REQUIRE (narrowed.find ("varying vec2 v_TexCoord") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (narrowed, fragment.compile ()).second.empty ());

    ShaderUnit repeated (GLSLContext::UnitType_Vertex, "workshop/test_shader.vert",
                         "attribute vec2 a_TexCoord; varying vec4 v_TexCoord;\n"
                         "void main() { v_TexCoord.xy = a_TexCoord; gl_Position = vec4(1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    repeated.linkToUnit (&fragment);
    REQUIRE (repeated.compile () == narrowed);
    ShaderUnit wideFragment (GLSLContext::UnitType_Fragment, "workshop/test_shader.frag",
                             "varying vec4 v_TexCoord;\n"
                             "void main() { gl_FragColor = v_TexCoord; }\n",
                             assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    ShaderUnit wideLinked (GLSLContext::UnitType_Vertex, "workshop/test_shader.vert",
                           "attribute vec2 a_TexCoord; varying vec4 v_TexCoord;\n"
                           "void main() { v_TexCoord.xy = a_TexCoord; gl_Position = vec4(1.0); }\n",
                           assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    wideLinked.linkToUnit (&wideFragment);
    REQUIRE (wideLinked.compile ().find ("varying vec4 v_TexCoord") != std::string::npos);

    ShaderUnit upper (GLSLContext::UnitType_Vertex, "upper.vert",
                      "varying vec4 v_TexCoord;\n"
                      "void main() { v_TexCoord.zw = vec2(0.0); gl_Position = vec4(1.0); }\n",
                      assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    upper.linkToUnit (&fragment);
    REQUIRE (upper.compile ().find ("varying vec4 v_TexCoord") != std::string::npos);

    ShaderUnit indexed (GLSLContext::UnitType_Vertex, "indexed.vert",
                        "varying vec4 v_TexCoord;\n"
                        "void main() { v_TexCoord[0] = 1.0; gl_Position = vec4(1.0); }\n",
                        assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    indexed.linkToUnit (&fragment);
    REQUIRE (indexed.compile ().find ("varying vec4 v_TexCoord") != std::string::npos);

    ShaderUnit whole (GLSLContext::UnitType_Vertex, "whole.vert",
                      "varying vec4 v_TexCoord; vec4 copyCoord(vec4 value) { return value; }\n"
                      "void main() { v_TexCoord = copyCoord(vec4(1.0)); gl_Position = vec4(1.0); }\n",
                      assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    whole.linkToUnit (&fragment);
    REQUIRE (whole.compile ().find ("varying vec4 v_TexCoord") != std::string::npos);
}

TEST_CASE ("Sampled local const becomes runtime float without changing literal constants", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "tone_mapping.frag",
                         "varying vec2 v_TexCoord; uniform sampler2D g_Texture1;\n"
                         "const float globalGain = 0.5;\n"
                         "void main() { const float literal = 1.0;\n"
                         "#if MASK\nconst float mask = texSample2D(g_Texture1, v_TexCoord).r;\n"
                         "#else\nconst float mask = 1.0;\n#endif\n"
                         "gl_FragColor = vec4(mask * literal * globalGain); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& source = fragment.compile ();
    REQUIRE (source.find ("float mask = texSample2D") != std::string::npos);
    REQUIRE (source.find ("const float mask = texSample2D") == std::string::npos);
    REQUIRE (source.find ("const float literal = 1.0") != std::string::npos);
    REQUIRE (source.find ("const float globalGain = 0.5") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nout vec2 v_TexCoord;\nvoid main() { v_TexCoord = vec2(0.5); gl_Position = vec4(1.0); }\n",
        source).second.empty ());

    ShaderUnit controls (GLSLContext::UnitType_Fragment, "controls.frag",
                         "uniform sampler2D g_Texture1;\n"
                         "float sampleWithParameter(const float gain) { return gain; }\n"
                         "void main() { // const float commented = texture(g_Texture1, vec2(0.0)).r;\n"
                         "const float literal = 2.0;\n"
                         "const float first = 1.0, second = texture(g_Texture1, vec2(0.0)).r;\n"
                         "gl_FragColor = vec4(first + second + literal); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& unchanged = controls.compile ();
    REQUIRE (unchanged.find ("const float gain") != std::string::npos);
    REQUIRE (unchanged.find ("const float commented") != std::string::npos);
    REQUIRE (unchanged.find ("const float literal = 2.0") != std::string::npos);
    REQUIRE (unchanged.find ("const float first = 1.0, second = texture") != std::string::npos);
}

TEST_CASE ("Wide pointer UV selects xy only for authored vec2 iris offset", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit vertex (GLSLContext::UnitType_Vertex, "iris_follow_mouse.vert",
                       "varying vec4 v_PointerUV; varying vec2 v_TexCoordIris;\n"
                       "uniform vec2 g_Scale; uniform vec2 g_Scale_Multiplier;\n"
                       "void main() { v_PointerUV = vec4(0.5);\n"
                       "vec2 da = v_PointerUV * (g_Scale * g_Scale_Multiplier) * 0.001;\n"
                       "v_TexCoordIris = da.xy; gl_Position = vec4(1.0); }\n",
                       assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "iris_follow_mouse.frag",
                         "varying vec2 v_TexCoordIris;\n"
                         "void main() { gl_FragColor = vec4(v_TexCoordIris, 0.0, 1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    vertex.linkToUnit (&fragment);
    fragment.linkToUnit (&vertex);
    REQUIRE (vertex.compile ().find ("vec2 da = v_PointerUV.xy *") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (vertex.compile (), fragment.compile ()).second.empty ());

    ShaderUnit different (GLSLContext::UnitType_Vertex, "different.vert",
                          "varying vec4 v_PointerUV;\nvoid main() { vec4 da = v_PointerUV * vec4(1.0); "
                          "v_PointerUV = da; gl_Position = vec4(1.0); }\n",
                          assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (different.compile ().find ("v_PointerUV.xy") == std::string::npos);

    ShaderUnit matrixProduct (GLSLContext::UnitType_Vertex, "matrix.vert",
                              "varying vec4 v_PointerUV; uniform vec2 g_Scale; "
                              "uniform vec2 g_Scale_Multiplier; uniform mat4 transform;\n"
                              "void main() { vec2 da = v_PointerUV * (transform) * 0.001; "
                              "gl_Position = vec4(da, 0.0, 1.0); }\n",
                              assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (matrixProduct.compile ().find ("v_PointerUV.xy") == std::string::npos);
}

TEST_CASE ("Wide fragment coordinate casts use bounded lexical matching", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    std::string longSource = "varying vec4 v_TexCoord;\n";
    for (int i = 0; i < 4000; ++i)
        longSource += "// CAST2(1.0) * v_TexCoord is commented out.\n";
    longSource += "void main() {\n"
                  "vec2 left = v_TexCoord * CAST2(1.0);\n"
                  "vec2 right = CAST2((1.0 + 2.0)) * v_TexCoord;\n"
                  "gl_FragColor = vec4(left + right, 0.0, 1.0); }\n";
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "long.frag", longSource,
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& rewritten = fragment.compile ();
    REQUIRE (rewritten.find ("vec2 left = v_TexCoord.xy * CAST2") != std::string::npos);
    REQUIRE (rewritten.find ("vec2 right = CAST2((1.0 + 2.0)) * v_TexCoord.xy") != std::string::npos);
    REQUIRE (rewritten.find ("// CAST2(1.0) * v_TexCoord is commented out") != std::string::npos);
    REQUIRE (rewritten.find ("// CAST2(1.0) * v_TexCoord.xy") == std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nout vec4 v_TexCoord;\nvoid main() { v_TexCoord = vec4(0.5); gl_Position = vec4(1.0); }\n",
        rewritten).second.empty ());

    ShaderUnit swizzled (GLSLContext::UnitType_Fragment, "swizzled.frag",
                         "varying vec4 v_TexCoord;\n"
                         "void main() { vec2 a = CAST2(1.0) * v_TexCoord.zw; "
                         "vec2 b = CAST2(1.0) * v_TexCoord[0]; "
                         "gl_FragColor = vec4(a, b, 1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (swizzled.compile ().find ("v_TexCoord.xy.zw") == std::string::npos);
    REQUIRE (swizzled.compile ().find ("v_TexCoord.xy[0]") == std::string::npos);
}

TEST_CASE ("Wide fragment arithmetic truncates in a vec2 initializer", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source =
        "varying vec4 v_TexCoord; uniform vec2 u_textureScale; uniform vec2 u_textureOffset;\n"
        "uniform vec2 u_texScaleCenter; uniform vec2 u_maskScale;\n"
        "#define ratioDiff vec2(1.0, 1.0)\n"
        "#define texScaleCenter (u_texScaleCenter * 2.0 - 1.0)\n"
        "void main() {\n"
        "vec2 uvTex = ((v_TexCoord * 2.0 - 1.0 - texScaleCenter) / ratioDiff / "
        "u_textureScale + 1.0 + texScaleCenter) / 2.0 - u_textureOffset;\n"
        "vec2 uvMask = ((v_TexCoord * 2.0 - 1.0 - texScaleCenter) / u_maskScale + "
        "1.0 + texScaleCenter) / 2.0 - u_textureOffset;\n"
        "vec2 upper = (v_TexCoord * vec4(2.0)).zw;\n"
        "gl_FragColor = vec4(uvTex + uvMask + upper, 0.0, 1.0); }\n";
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "mixed-width.frag", source,
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& rewritten = fragment.compile ();
    REQUIRE (rewritten.find ("vec2 uvTex = ((v_TexCoord.xy * 2.0") != std::string::npos);
    REQUIRE (rewritten.find ("vec2 uvMask = ((v_TexCoord.xy * 2.0") != std::string::npos);
    REQUIRE (rewritten.find ("vec2 upper = (v_TexCoord * vec4(2.0)).zw") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nout vec4 v_TexCoord;\nvoid main() { v_TexCoord = vec4(0.5); gl_Position = vec4(1.0); }\n",
        rewritten).second.empty ());
}

TEST_CASE ("Shader float metadata preserves fractional defaults and rejects partial numbers", "[shader][metadata]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit fractional (GLSLContext::UnitType_Fragment, "fractional.frag",
                           "uniform float g_Exposure; // {\"material\":\"exposure\",\"default\":\" 0.5 \"}\n"
                           "uniform float g_Offset; // {\"material\":\"offset\",\"default\":-0.25}\n"
                           "void main() { gl_FragColor = vec4(g_Exposure + g_Offset); }\n",
                           assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (fractional.getParameters ().size () == 2);
    REQUIRE (fractional.getParameters ()[0]->getIdentifierName () == "exposure");
    REQUIRE (fractional.getParameters ()[0]->getFloat () == 0.5f);
    REQUIRE (fractional.getParameters ()[1]->getFloat () == -0.25f);

    const auto invalid = [&] (const char* value) {
        ShaderUnit unit (GLSLContext::UnitType_Fragment, "invalid-default.frag",
                         std::string ("uniform float g_Exposure; // {\"material\":\"exposure\",\"default\":\"")
                             + value + "\"}\nvoid main() { gl_FragColor = vec4(1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
        (void) unit;
    };
    REQUIRE_THROWS_AS (invalid ("0.5garbage"), std::invalid_argument);
    REQUIRE_THROWS_AS (invalid ("1e999"), std::invalid_argument);
    REQUIRE_THROWS_AS (invalid (" "), std::invalid_argument);
}

TEST_CASE ("Shader metadata accepts typed material constants without authored defaults", "[shader][metadata]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderConstantMap constants;
    constants.emplace ("exposure", UserSettingBuilder::fromValue (0.625f));
    constants.emplace ("offset", UserSettingBuilder::fromValue (glm::vec2 (0.25f, -0.5f)));
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "constant-only.frag",
                     "uniform float g_Exposure; // {\"material\":\"exposure\"}\n"
                     "uniform vec2 g_Offset; // {\"material\":\"offset\"}\n"
                     "void main() { gl_FragColor = vec4(g_Exposure + g_Offset.x); }\n",
                     assets, constants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (unit.getParameters ().size () == 2);
    REQUIRE (unit.getParameters ()[0]->getFloat () == 0.625f);
    REQUIRE (unit.getParameters ()[1]->getVec2 ().x == 0.25f);
    REQUIRE (unit.getParameters ()[1]->getVec2 ().y == -0.5f);

    ShaderConstantMap mismatched;
    mismatched.emplace ("exposure", UserSettingBuilder::fromValue (7));
    REQUIRE_THROWS_AS (
        ShaderUnit (GLSLContext::UnitType_Fragment, "mismatched.frag",
                    "uniform float g_Exposure; // {\"material\":\"exposure\"}\n",
                    assets, mismatched, emptyTextures, emptyTextures, emptyCombos, emptyCombos),
        std::invalid_argument
    );

    auto shaderFiles = std::make_unique<Container> ();
    shaderFiles->getVFS ().add ("shaders/material-only.vert",
                                "void main() { gl_Position = vec4(1.0); }\n");
    shaderFiles->getVFS ().add ("shaders/material-only.frag",
                                "uniform float g_Exposure; // {\"material\":\"exposure\"}\n"
                                "void main() { gl_FragColor = vec4(g_Exposure); }\n");
    AssetLocator shaderAssets (std::move (shaderFiles));
    ShaderConstantMap passConstants;
    passConstants.emplace ("exposure", UserSettingBuilder::fromValue (0.375f));
    ShaderConstantMap overrideConstants;
    Shader shader (shaderAssets, "material-only", emptyCombos, emptyCombos,
                   emptyTextures, emptyTextures, overrideConstants, &passConstants);
    REQUIRE (shader.findParameter ("exposure").fragment != nullptr);
    REQUIRE (shader.findParameter ("exposure").fragment->getFloat () == 0.375f);
    overrideConstants.emplace ("exposure", UserSettingBuilder::fromValue (0.875f));
    Shader overridden (shaderAssets, "material-only", emptyCombos, emptyCombos,
                       emptyTextures, emptyTextures, overrideConstants, &passConstants);
    REQUIRE (overridden.findParameter ("exposure").fragment->getFloat () == 0.875f);
}

TEST_CASE ("Model scalar material names retain defaults and authored override mapping",
           "[shader][model][material]") {
    // The native rendered contract exercises both the custom Bright spelling
    // and generic2's literal Brigtness spelling, with reserved GLSL names.
    for (const std::string brightness : {"Bright", "Brigtness"}) {
        auto files = std::make_unique<Container> ();
        files->getVFS ().add ("shaders/model-scalar.vert",
                             "void main() { gl_Position = vec4(1.0); }\n");
        const auto source = [&] (bool defaults) {
            const std::string brightnessDefault = defaults ? ",\"default\":0.35" : "";
            const std::string alphaDefault = defaults ? ",\"default\":0.45" : "";
            const std::string powerDefault = defaults ? ",\"default\":0.55" : "";
            return "uniform float g_Brightness; // {\"material\":\"" + brightness
                + "\"" + brightnessDefault + "}\n"
                  "uniform float g_UserAlpha; // {\"material\":\"Alpha\"" + alphaDefault + "}\n"
                  "uniform float g_Power; // {\"material\":\"Power\"" + powerDefault + "}\n"
                  "void main() { gl_FragColor = vec4(g_Brightness,g_UserAlpha,g_Power,1.0); }\n";
        };
        files->getVFS ().add ("shaders/model-scalar.frag", source (false));
        files->getVFS ().add ("shaders/model-default.vert",
                             "void main() { gl_Position = vec4(1.0); }\n");
        files->getVFS ().add ("shaders/model-default.frag", source (true));
        AssetLocator assets (std::move (files));
        ShaderConstantMap authored;
        authored.emplace (brightness, UserSettingBuilder::fromValue (0.2f));
        authored.emplace ("Alpha", UserSettingBuilder::fromValue (0.4f));
        authored.emplace ("Power", UserSettingBuilder::fromValue (0.6f));
        Shader base (assets, "model-scalar", emptyCombos, emptyCombos,
                     emptyTextures, emptyTextures, emptyConstants, &authored);
        REQUIRE (base.findParameter (brightness).fragment->getName () == "g_Brightness");
        REQUIRE (base.findParameter ("Alpha").fragment->getName () == "g_UserAlpha");
        REQUIRE (base.findParameter (brightness).fragment->getFloat () == 0.2f);
        REQUIRE (base.findParameter ("Alpha").fragment->getFloat () == 0.4f);
        REQUIRE (base.findParameter ("Power").fragment->getFloat () == 0.6f);
        ShaderConstantMap overrides;
        overrides.emplace (brightness, UserSettingBuilder::fromValue (0.7f));
        overrides.emplace ("Alpha", UserSettingBuilder::fromValue (0.8f));
        Shader overridden (assets, "model-scalar", emptyCombos, emptyCombos,
                           emptyTextures, emptyTextures, overrides, &authored);
        REQUIRE (overridden.findParameter (brightness).fragment->getFloat () == 0.7f);
        REQUIRE (overridden.findParameter ("Alpha").fragment->getFloat () == 0.8f);
        REQUIRE (overridden.findParameter ("Power").fragment->getFloat () == 0.6f);
        Shader defaulted (assets, "model-default", emptyCombos, emptyCombos,
                          emptyTextures, emptyTextures, emptyConstants);
        REQUIRE (defaulted.findParameter (brightness).fragment->getFloat () == 0.35f);
        REQUIRE (defaulted.findParameter ("Alpha").fragment->getFloat () == 0.45f);
        REQUIRE (defaulted.findParameter ("Power").fragment->getFloat () == 0.55f);
    }
}

TEST_CASE ("Sampler requirements read override-only combos without invalid iterators", "[shader][metadata]") {
    // This checks safe override lookup and preserves existing gating decisions;
    // the native require/requireany meaning is still under investigation.
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const auto discover = [&] (bool any, int mode) {
        const ComboMap overrideCombos = {{"MODE", mode}};
        const std::string metadata = any
            ? "{\"combo\":\"USE_TEX\",\"default\":\"1\",\"requireany\":true,\"require\":{\"MODE\":1}}"
            : "{\"combo\":\"USE_TEX\",\"default\":\"1\",\"require\":{\"MODE\":1}}";
        ShaderUnit unit (GLSLContext::UnitType_Fragment, "override-only.frag",
                         "uniform sampler2D g_Texture1; // " + metadata + "\n"
                         "void main() { gl_FragColor = vec4(1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, overrideCombos);
        return unit.getDiscoveredCombos ();
    };
    REQUIRE_FALSE (discover (false, 1).contains ("USE_TEX"));
    REQUIRE (discover (false, 2).at ("USE_TEX") == 1);
    REQUIRE_FALSE (discover (true, 1).contains ("USE_TEX"));
    REQUIRE (discover (true, 2).at ("USE_TEX") == 1);
}

TEST_CASE ("Particle atlas renderer combo overrides authored blend values", "[shader][particle][texture]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string source = "void main() { gl_FragColor = vec4(1.0); }\n";

    const ComboMap authoredOff {{"SPRITESHEETBLEND", 0}};
    const ComboMap sequenceBlend {{"SPRITESHEETBLEND", 1}};
    ShaderUnit sequence (GLSLContext::UnitType_Fragment, "particle-sequence.frag", source,
                         assets, emptyConstants, emptyTextures, emptyTextures,
                         authoredOff, sequenceBlend);
    REQUIRE (sequence.compile ().find ("#define SPRITESHEETBLEND 1") != std::string::npos);
    REQUIRE (sequence.compile ().find ("#define SPRITESHEETBLEND 0") == std::string::npos);

    const ComboMap authoredOn {{"SPRITESHEETBLEND", 1}};
    const ComboMap discreteAtlas {{"SPRITESHEETBLEND", 0}};
    ShaderUnit randomFrame (GLSLContext::UnitType_Fragment, "particle-random.frag", source,
                            assets, emptyConstants, emptyTextures, emptyTextures,
                            authoredOn, discreteAtlas);
    REQUIRE (randomFrame.compile ().find ("#define SPRITESHEETBLEND 0") != std::string::npos);
    REQUIRE (randomFrame.compile ().find ("#define SPRITESHEETBLEND 1") == std::string::npos);
}

TEST_CASE ("Sine wave opacity uses the equal scalar wave components", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "workshop/2402621362/effects/sine_wave.frag",
                         "varying vec2 v_TexCoord;\nuniform float u_WaveOpacity;\n"
                         "vec3 ApplyBlending(int mode, vec3 a, vec3 b, float opacity) { return mix(a,b,opacity); }\n"
                         "void main() { vec2 waveCoord = v_TexCoord;\n"
                         "waveCoord = pow(0.5, 1.0);\n"
                         "vec3 color = ApplyBlending(0, vec3(0.0), vec3(1.0), u_WaveOpacity * waveCoord);\n"
                         "gl_FragColor = vec4(color, 1.0); }\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    const auto& source = fragment.compile ();
    REQUIRE (source.find ("u_WaveOpacity * waveCoord.x") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nout vec2 v_TexCoord;\nvoid main() { v_TexCoord = vec2(0.5); gl_Position = vec4(1.0); }\n",
        source).second.empty ());
}

TEST_CASE ("Unused shadow mask function has no missing-return path", "[shader][compat]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const ComboMap shadowOff = {{"SHADOWMASK", 0}};
    ShaderUnit fragment (GLSLContext::UnitType_Fragment, "workshop/2821337237/effects/shadow_map.frag",
                         "vec3 ShadowMask(vec2 uv) {\n#if SHADOWMASK == 1\nreturn vec3(uv, 1.0);\n#endif\n}\n"
                         "void main() { gl_FragColor = vec4(1.0);\n"
                         "#if SHADOWMASK != 0\ngl_FragColor.rgb *= ShadowMask(vec2(0.5));\n#endif\n}\n",
                         assets, emptyConstants, emptyTextures, emptyTextures, shadowOff, emptyCombos);
    const auto& source = fragment.compile ();
    REQUIRE (source.find ("#if SHADOWMASK != 0\nvec3 ShadowMask") != std::string::npos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nvoid main() { gl_Position = vec4(1.0); }\n", source).second.empty ());
    const ComboMap shadowOn = {{"SHADOWMASK", 1}};
    ShaderUnit active (GLSLContext::UnitType_Fragment, "workshop/2821337237/effects/shadow_map.frag",
                       "vec3 ShadowMask(vec2 uv) {\n#if SHADOWMASK == 1\nreturn vec3(uv, 1.0);\n#endif\n}\n"
                       "void main() { gl_FragColor = vec4(1.0);\n"
                       "#if SHADOWMASK != 0\ngl_FragColor.rgb *= ShadowMask(vec2(0.5));\n#endif\n}\n",
                       assets, emptyConstants, emptyTextures, emptyTextures, shadowOn, emptyCombos);
    REQUIRE_FALSE (GLSLContext::get ().toGlsl (
        "#version 330\nvoid main() { gl_Position = vec4(1.0); }\n", active.compile ()).second.empty ());
}

TEST_CASE ("Matching conditional varying widths survive compatibility rewriting", "[shader][conditional-varying]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    const std::string declarations =
        "#if LIGHTMAP\nvarying vec4 v_TexCoord;\n"
        "#else\nvarying vec2 v_TexCoord;\n#endif\n";
    for (const int lightmap : {0, 1}) {
        CAPTURE (lightmap);
        const ComboMap combos {{"LIGHTMAP", lightmap}};
        ShaderUnit vertex (GLSLContext::UnitType_Vertex, "conditional-width.vert",
            "attribute vec2 a_TexCoord;\nattribute vec4 a_TexCoordVec4;\n" + declarations +
            "void main() {\n#if LIGHTMAP\nv_TexCoord = a_TexCoordVec4;\n"
            "#else\nv_TexCoord = a_TexCoord;\n#endif\ngl_Position = vec4(1.0);\n}\n",
            assets, emptyConstants, emptyTextures, emptyTextures, combos, emptyCombos);
        ShaderUnit fragment (GLSLContext::UnitType_Fragment, "conditional-width.frag",
            declarations + "void main() {\n#if LIGHTMAP\ngl_FragColor = v_TexCoord;\n"
            "#else\ngl_FragColor = vec4(v_TexCoord, 0.0, 1.0);\n#endif\n}\n",
            assets, emptyConstants, emptyTextures, emptyTextures, combos, emptyCombos);
        vertex.linkToUnit (&fragment);
        fragment.linkToUnit (&vertex);
        const auto translated = GLSLContext::get ().toGlsl (vertex.compile (), fragment.compile ());
        REQUIRE_FALSE (translated.first.empty ());
        REQUIRE_FALSE (translated.second.empty ());
    }
}

TEST_CASE ("Compiler version selects native radius-scaled PBR independently of material version",
           "[shader][lighting][version]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    // The shipped genericimage3/generic3 branch predates LightingV1 and
    // depends on the engine environment, not its authored VERSION=2 combo.
    const std::string source =
        "#if SHADERVERSION < 62\n"
        "#define LIGHT_RADIUS_FACTOR(radius) 1.0\n"
        "#else\n"
        "#define LIGHT_RADIUS_FACTOR(radius) ((radius)*(radius))\n"
        "#endif\n"
        "void main() { gl_FragColor = vec4(LIGHT_RADIUS_FACTOR(250.0)); }\n";
    const ComboMap materialVersion {{"VERSION", 2}};
    ShaderUnit unit (GLSLContext::UnitType_Fragment, "version-light.frag", source,
                     assets, emptyConstants, emptyTextures, emptyTextures, materialVersion, emptyCombos);
    const auto& compiled = unit.compile ();
    const auto environment = compiled.find ("#define SHADERVERSION 69\n");
    REQUIRE (environment != std::string::npos);
    REQUIRE (environment < compiled.find ("#if SHADERVERSION < 62"));
    REQUIRE (compiled.find ("#define VERSION 2\n") != std::string::npos);
}

TEST_CASE ("Shader metadata preserves native numeric defaults with decimal leading zeroes",
           "[shader][metadata]") {
    using WallpaperEngine::Render::Shaders::parseShaderMetadata;
    const auto data = parseShaderMetadata (
        R"({"default":0.75,"range":[0,01,-001.25,000e2],"label":"001"})", "metadata.frag");
    REQUIRE (data.at ("default").get<float> () == 0.75f);
    REQUIRE (data.at ("range").at (1).get<int> () == 1);
    REQUIRE (data.at ("range").at (2).get<float> () == -1.25f);
    REQUIRE (data.at ("range").at (3).get<float> () == 0.0f);
    REQUIRE (data.at ("label").get<std::string> () == "001");
    REQUIRE_THROWS (parseShaderMetadata (R"({"default":0.75,"options":{Bad":0}})", "metadata.frag"));
    REQUIRE_THROWS (WallpaperEngine::Data::JSON::parseAuthoringJson (R"({"range":[0,01]})", "scene.json"));
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit shader (GLSLContext::UnitType_Fragment, "metadata.frag",
        "uniform float g_Value; // {\"material\":\"Value\",\"default\":0.75,\"range\":[0,01]}\n"
        "void main() { gl_FragColor = vec4(g_Value); }\n",
        assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (shader.getParameters ().size () == 1);
    REQUIRE (shader.getParameters ()[0]->getFloat () == 0.75f);
}

TEST_CASE ("Valid scalar material metadata without a default registers zero", "[shader][metadata]") {
    auto files = std::make_unique<Container> ();
    AssetLocator assets (std::move (files));
    ShaderUnit shader (GLSLContext::UnitType_Fragment, "defaults.frag",
        "uniform float g_Registered; // {\"material\":\"Registered\"}\n"
        "uniform float g_Zero = 0.0;\n"
        "uniform float g_Quarter = 0.25;\n"
        "// uniform float g_Unbound = 3.0;\n"
        "uniform float g_Unbound;\n"
        "void main() { gl_FragColor = vec4(g_Registered + g_Zero + g_Quarter + g_Unbound); }\n",
        assets, emptyConstants, emptyTextures, emptyTextures, emptyCombos, emptyCombos);
    REQUIRE (shader.getParameters ().size () == 1);
    REQUIRE (shader.getParameters ()[0]->getFloat () == 0.0f);
}
