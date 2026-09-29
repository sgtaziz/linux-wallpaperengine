#include "CText.h"
#include "TextRaster.h"
#include "TextCodepoints.h"
#include "TextShaping.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "WallpaperEngine/Data/Model/DynamicValue.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Data/Model/Effect.h"
#include "WallpaperEngine/Data/Model/Material.h"
#include "WallpaperEngine/Data/Model/UserSetting.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/EffectClearAction.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

using namespace WallpaperEngine::Render::Objects;

namespace {
const Material kTextEffectMaterial {};
// Used only when an embedded font cannot be loaded or a system font was authored.
const std::vector<std::string> kFontCandidates = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
};

const char* kVertexShader = R"glsl(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uBackdropMVP;
out vec2 vUV;
out vec4 vBackdropPosition;
void main() {
    vUV = aUV;
    vBackdropPosition = uBackdropMVP * vec4(aPos, 0.0, 1.0);
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 330 core
in vec2 vUV;
in vec4 vBackdropPosition;
uniform sampler2D uTexture;
uniform sampler2D uBackdropTexture;
uniform sampler2D uTransmittanceTexture;
uniform vec4 uColor;
uniform float uNodeAlpha;
uniform bool uFlipV;
uniform bool uOpaqueBackground;
uniform vec4 uBackgroundColor;
uniform vec2 uGlyphUvOffset;
uniform vec2 uGlyphUvScale;
uniform bool uEffectFinal;
uniform bool uOffscreenBackdrop;
out vec4 FragColor;
void main() {
    if (uEffectFinal) {
        vec2 effectUV = vec2(vUV.x, uFlipV ? 1.0 - vUV.y : vUV.y);
        vec4 effectColor = texture(uTexture, effectUV);
        FragColor = vec4(effectColor.rgb, effectColor.a * uNodeAlpha);
        return;
    }
    vec2 glyphUV = (uOpaqueBackground || uOffscreenBackdrop)
        ? (vUV - uGlyphUvOffset) * uGlyphUvScale : vUV;
    vec4 glyph = vec4(0.0);
    bool withinGlyph = all(greaterThanEqual(glyphUV, vec2(0.0))) &&
        all(lessThanEqual(glyphUV, vec2(1.0)));
    if (withinGlyph) {
        glyphUV.y = uFlipV ? 1.0 - glyphUV.y : glyphUV.y;
        glyph = texture(uTexture, glyphUV);
    }
    if (uOpaqueBackground) {
        FragColor = withinGlyph ? glyph : vec4(uBackgroundColor.rgb, uNodeAlpha);
    } else if (uOffscreenBackdrop) {
        vec2 backdropUV = vBackdropPosition.xy / vBackdropPosition.w * 0.5 + 0.5;
        vec3 backdrop = texture(uBackdropTexture, backdropUV).rgb;
        float remaining = withinGlyph ? texture(uTransmittanceTexture, glyphUV).r : 1.0;
        FragColor = vec4(glyph.rgb + backdrop * remaining, glyph.a);
    } else {
        FragColor = vec4(glyph.rgb, uNodeAlpha * uColor.a * glyph.a);
    }
}
)glsl";

GLuint compileShader (GLenum type, const char* source) {
    GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &source, nullptr);
    glCompileShader (shader);

    GLint status = GL_FALSE;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
	char log[1024];
	glGetShaderInfoLog (shader, sizeof (log), nullptr, log);
	sLog.error ("CText shader compile failed: ", log);
	glDeleteShader (shader);
	return 0;
    }
    return shader;
}
} // namespace

CText::CText (Wallpapers::CScene& scene, const Text& text) :
    CObject (scene, text), CRenderable (scene, text, kTextEffectMaterial),
    ScriptableObject (scene, text), m_text (text),
    m_horizontalAlign (text.alignment), m_verticalAlign (text.verticalalign) {
    for (const auto& binding : Scripting::scriptPropertyBindings (text))
	this->registerProperty (binding.name, binding.value);
    // Effect visibility and shader constants retain their own UserSettings.
    // Queue nested scripts without exposing synthetic names on thisLayer.
    // CPass reads these same per-instance DynamicValues on subsequent frames.
    for (size_t effectIndex = 0; effectIndex < text.effects.size (); ++effectIndex) {
	const auto& effect = text.effects[effectIndex];
	const std::string prefix = "textEffect" + std::to_string (effectIndex)
	    + "_object" + std::to_string (getId ()) + "_";
	auto queue = [this, &prefix] (const std::string& suffix, DynamicValue& value) {
	    getScene ().getScriptEngine ().queueScript (prefix + suffix, value, *this);
	};
	queue ("visible", *effect->visible->value);
	for (size_t passIndex = 0; passIndex < effect->effect->passes.size (); ++passIndex) {
	    const auto& pass = effect->effect->passes[passIndex];
	    if (!pass->material) continue;
	    for (size_t materialIndex = 0; materialIndex < (*pass->material)->passes.size (); ++materialIndex) {
		for (const auto& [name, value] : (*pass->material)->passes[materialIndex]->constants)
		    queue ("pass" + std::to_string (passIndex) + "_material"
		           + std::to_string (materialIndex) + "_constant_" + name, *value->value);
	    }
	}
	for (size_t passIndex = 0; passIndex < effect->passOverrides.size (); ++passIndex) {
	    for (const auto& [name, value] : effect->passOverrides[passIndex]->constants)
		queue ("override" + std::to_string (passIndex)
		       + "_constant_" + name, *value->value);
	}
    }
}

CText::~CText () {
    m_effectSteps.clear ();
    if (m_passPosition != 0) glDeleteBuffers (1, &m_passPosition);
    if (m_passTexCoord != 0) glDeleteBuffers (1, &m_passTexCoord);
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_vao != 0) {
	glDeleteVertexArrays (1, &m_vao);
    }
    if (m_program != 0) {
	glDeleteProgram (m_program);
    }
    if (m_glyphTexture != 0) {
	glDeleteTextures (1, &m_glyphTexture);
    }
    if (m_glyphTransmittanceTexture != 0)
	glDeleteTextures (1, &m_glyphTransmittanceTexture);
    if (m_ftFace != nullptr) {
	FT_Done_Face (m_ftFace);
    }
    if (m_bundledEmojiFace) FT_Done_Face (m_bundledEmojiFace);
    for (FT_Face face : m_fallbackFaces) FT_Done_Face (face);
    if (m_ftLibrary != nullptr) {
	FT_Done_FreeType (m_ftLibrary);
    }
}

void CText::setup () {
    const bool scripted = m_text.text->value->getScriptSource ().has_value ();
    const auto& text = m_text.text->value->getString ();

    // Nothing to render and no script to produce text later → bail.
    if (text.empty () && !scripted) {
	return;
    }

    if (!initFreeType ()) {
	return;
    }

    if (!loadEmbeddedFont () && !loadSystemFont ()) {
	return;
    }

    m_lastCharacterSize26_6 = computeCharacterSize26_6 ();
    FT_Set_Char_Size (m_ftFace, 0, m_lastCharacterSize26_6, 300, 300);

    buildShader ();
    // Scripted text may have an empty placeholder; use a single space so the
    // glyph texture has non-zero dimensions until the script produces a value.
    rebuildTextureFrom (text.empty () ? std::string (" ") : text);
    setupEffects ();

    m_valid = m_glyphTexture != 0 && m_program != 0 && m_vao != 0;
}

bool CText::initFreeType () {
    if (FT_Init_FreeType (&m_ftLibrary) == 0) {
	return true;
    }
    sLog.error ("CText: FT_Init_FreeType failed for object ", m_text.name);
    return false;
}

bool CText::loadEmbeddedFont () {
    // Wallpapers packed in .pkg don't expose physical paths, so we read the font
    // into memory and use FT_New_Memory_Face. m_fontData must outlive the face.
    // `systemfont_*` references signal "use a system font"; let the fallback handle them.
    if (m_text.font.empty () || m_text.font.rfind ("systemfont_", 0) == 0) {
	return false;
    }

    try {
	auto stream = getAssetLocator ().read (m_text.font);
	stream->seekg (0, std::ios::end);
	const auto size = stream->tellg ();
	stream->seekg (0, std::ios::beg);
	m_fontData.resize (static_cast<size_t> (size));
	stream->read (reinterpret_cast<char*> (m_fontData.data ()), size);

	if (FT_New_Memory_Face (
		m_ftLibrary, m_fontData.data (), static_cast<FT_Long> (m_fontData.size ()), 0, &m_ftFace
	    )
	    == 0) {
	    return true;
	}

	sLog.error ("CText: FT_New_Memory_Face failed for '", m_text.font, "', falling back to system font");
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot read font '", m_text.font, "': ", e.what (), ", falling back to system font");
    }

    m_fontData.clear ();
    return false;
}

bool CText::loadSystemFont () {
    std::string fontPath;
    for (const auto& candidate : kFontCandidates) {
	if (std::filesystem::exists (candidate)) {
	    fontPath = candidate;
	    break;
	}
    }
    if (fontPath.empty ()) {
	sLog.error ("CText: no usable system font found");
	return false;
    }
    if (FT_New_Face (m_ftLibrary, fontPath.c_str (), 0, &m_ftFace) != 0) {
	sLog.error ("CText: FT_New_Face failed for ", fontPath);
	return false;
    }
    return true;
}

FT_Face CText::faceForCodepoint (char32_t codepoint) {
    if (FT_Get_Char_Index (m_ftFace, static_cast<FT_ULong> (codepoint)) != 0)
        return m_ftFace;
    if (const auto found = m_characterFaces.find (codepoint); found != m_characterFaces.end ())
        return found->second;

    // Of the native eight-face fallback list, TwemojiMozilla is shipped in
    // engine assets. Resolve it through the scene's overlay/container so the
    // source can be a directory or package and no Steam path is baked in.
    if (!m_bundledEmojiAttempted) {
        m_bundledEmojiAttempted = true;
        try {
            auto stream = getAssetLocator ().read ("fonts/TwemojiMozilla.ttf");
            stream->seekg (0, std::ios::end);
            const auto size = stream->tellg ();
            stream->seekg (0, std::ios::beg);
            if (size > 0) {
                m_bundledEmojiData.resize (static_cast<size_t> (size));
                stream->read (reinterpret_cast<char*> (m_bundledEmojiData.data ()), size);
                if (FT_New_Memory_Face (m_ftLibrary, m_bundledEmojiData.data (),
                        static_cast<FT_Long> (m_bundledEmojiData.size ()), 0,
                        &m_bundledEmojiFace) == 0)
                    FT_Set_Char_Size (m_bundledEmojiFace, 0, m_lastCharacterSize26_6, 300, 300);
                else m_bundledEmojiData.clear ();
            }
        } catch (const std::exception&) {
            m_bundledEmojiData.clear ();
        }
    }
    if (m_bundledEmojiFace
        && FT_Get_Char_Index (m_bundledEmojiFace, static_cast<FT_ULong> (codepoint)) != 0) {
        m_characterFaces.emplace (codepoint, m_bundledEmojiFace);
        return m_bundledEmojiFace;
    }

    FT_Face selected = m_ftFace;
    FcPattern* pattern = FcPatternCreate ();
    FcCharSet* charset = FcCharSetCreate ();
    if (pattern && charset && FcCharSetAddChar (charset, static_cast<FcChar32> (codepoint))) {
        FcPatternAddCharSet (pattern, FC_CHARSET, charset);
        FcConfigSubstitute (nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute (pattern);
        FcResult status = FcResultNoMatch;
        FcPattern* match = FcFontMatch (nullptr, pattern, &status);
        FcChar8* path = nullptr;
        int faceIndex = 0;
        if (match && FcPatternGetString (match, FC_FILE, 0, &path) == FcResultMatch) {
            FcPatternGetInteger (match, FC_INDEX, 0, &faceIndex);
            const std::string key = std::string (reinterpret_cast<const char*> (path))
                + "#" + std::to_string (faceIndex);
            if (const auto cached = m_fontPathFaces.find (key); cached != m_fontPathFaces.end ()) {
                selected = cached->second;
            } else {
                FT_Face candidate = nullptr;
                if (FT_New_Face (m_ftLibrary, reinterpret_cast<const char*> (path),
                                 faceIndex, &candidate) == 0) {
                    FT_Set_Char_Size (candidate, 0, m_lastCharacterSize26_6, 300, 300);
                    m_fallbackFaces.push_back (candidate);
                    m_fontPathFaces.emplace (key, candidate);
                    selected = candidate;
                }
            }
        }
        if (match) FcPatternDestroy (match);
    }
    if (charset) FcCharSetDestroy (charset);
    if (pattern) FcPatternDestroy (pattern);
    if (FT_Get_Char_Index (selected, static_cast<FT_ULong> (codepoint)) == 0)
        selected = m_ftFace;
    m_characterFaces.emplace (codepoint, selected);
    return selected;
}

void CText::setupEffects () {
    if (m_text.effects.empty ()) return;
    resizeEffectTargets ();
    if (!m_textTarget) return;
    m_texture = m_textTarget;

    const GLfloat positions[] = {
	-1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
	1.0f, 1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f,
    };
    const GLfloat uvs[] = {0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
			   1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    glGenBuffers (1, &m_passPosition);
    glBindBuffer (GL_ARRAY_BUFFER, m_passPosition);
    glBufferData (GL_ARRAY_BUFFER, sizeof (positions), positions, GL_STATIC_DRAW);
    glGenBuffers (1, &m_passTexCoord);
    glBindBuffer (GL_ARRAY_BUFFER, m_passTexCoord);
    glBufferData (GL_ARRAY_BUFFER, sizeof (uvs), uvs, GL_STATIC_DRAW);

    for (const auto& effect : m_text.effects) {
	const auto provider = std::make_shared<FBOProvider> (this);
	m_effectProviders.push_back (provider);
	for (const auto& fbo : effect->effect->fbos) {
	    provider->create (*fbo, TextureFlags_NoFlags, glm::vec2 (m_effectTargetSize));
	    m_sizedTargets.push_back ({fbo.get (), provider});
	}
	auto override = effect->passOverrides.begin ();
	for (const auto& effectPass : effect->effect->passes) {
	    if (effectPass->command.has_value () && *effectPass->command == Command_Swap) {
		m_effectSteps.push_back ({effect.get (), provider, nullptr, effectPass->command,
		                         effectPass->source.value_or (""), effectPass->target.value_or ("")});
		continue;
	    }
	    if (!effectPass->material.has_value ()) {
		if (effectPass->command != Command_Copy || !effectPass->source.has_value ()) {
		    sLog.error ("Text effect pass without material/copy source is unsupported: object ", m_text.id);
		    continue;
		}
		auto copy = std::make_unique<MaterialPass> (MaterialPass {
		    .blending = BlendingMode_Normal, .cullmode = CullingMode_Disable,
		    .depthtest = DepthtestMode_Disabled, .depthwrite = DepthwriteMode_Disabled,
		    .shader = "commands/copy", .textures = {{0, *effectPass->source}},
		    .usertextures = {}, .combos = {}, .constants = {},
		});
		const auto& materialPass = *m_virtualEffectMaterials.emplace_back (std::move (copy));
		std::optional<std::reference_wrapper<std::string>> target;
		if (effectPass->target.has_value ()) target = *effectPass->target;
		m_effectSteps.push_back ({effect.get (), provider,
		    std::make_unique<Effects::CPass> (*this, provider, materialPass,
		                                       std::nullopt, std::nullopt, target),
		    std::nullopt, "", ""});
		continue;
	    }
	    for (const auto& materialPass : (*effectPass->material)->passes) {
		std::optional<std::reference_wrapper<const ImageEffectPassOverride>> passOverride;
		if (override != effect->passOverrides.end ()) passOverride = **override;
		std::optional<std::reference_wrapper<std::string>> target;
		if (effectPass->target.has_value ()) target = *effectPass->target;
		m_effectSteps.push_back ({effect.get (), provider,
		    std::make_unique<Effects::CPass> (*this, provider, *materialPass, passOverride,
		                                       effectPass->binds, target),
		    std::nullopt, "", ""});
	    }
	    if (override != effect->passOverrides.end ()) ++override;
	}
    }
}

void CText::resizeEffectTargets () {
    if (m_text.effects.empty ()) return;
    const glm::vec2 pad = glm::clamp (m_text.padding->value->getVec2 (), glm::vec2 (0.0f), glm::vec2 (512.0f));
    const glm::vec2 dimensions = glm::vec2 (m_textureSize) + 2.0f * pad;
    const glm::ivec2 size = glm::max (glm::ivec2 (1), glm::ivec2 (glm::ceil (dimensions)));
    if (size == m_effectTargetSize && m_textTarget) return;
    m_effectTargetSize = size;
    const auto width = static_cast<uint32_t> (size.x);
    const auto height = static_cast<uint32_t> (size.y);
    if (!m_textTarget) {
	m_textTarget = std::make_shared<CFBO> ("_text_base", TextureFormat_ARGB8888,
	    TextureFlags_NoFlags, 1.0f, width, height, width, height);
	m_effectMain = std::make_shared<CFBO> ("_text_main", TextureFormat_ARGB8888,
	    TextureFlags_NoFlags, 1.0f, width, height, width, height);
	m_effectSub = std::make_shared<CFBO> ("_text_sub", TextureFormat_ARGB8888,
	    TextureFlags_NoFlags, 1.0f, width, height, width, height);
    } else {
	m_textTarget->resize (width, height, width, height);
	m_effectMain->resize (width, height, width, height);
	m_effectSub->resize (width, height, width, height);
    }
    m_effectMain->clear (glm::vec4 (0.0f));
    m_effectSub->clear (glm::vec4 (0.0f));
    for (const auto& target : m_sizedTargets)
	target.provider->create (*target.descriptor, TextureFlags_NoFlags, dimensions);
}

bool CText::hasVisibleEffects () const {
    return std::any_of (m_text.effects.begin (), m_text.effects.end (), [] (const auto& effect) {
	return effect->visible->value->getBool ();
    });
}

bool CText::executeMaterialFunction (const std::string& name) {
    bool executed = false;
    for (size_t index = 0; index < m_effectProviders.size (); ++index)
	executed = executeEffectClearAction (*m_text.effects[index]->effect,
	    *m_effectProviders[index], name) || executed;
    return executed;
}

std::shared_ptr<const TextureProvider> CText::runEffects () {
    std::shared_ptr<const TextureProvider> input = m_textTarget;
    std::shared_ptr<const TextureProvider> effectInput;
    bool inTargetSequence = false;
    bool useMain = true;
    m_effectiveColor4 = m_text.color->value->getVec4 ();
    m_effectiveColor4.a = 1.0f;
    for (auto& step : m_effectSteps) {
	if (!step.effect->visible->value->getBool ()) continue;
	if (step.command == Command_Swap) {
	    step.provider->swap (step.source, step.target);
	    continue;
	}
	if (!step.pass) continue;
	std::shared_ptr<const CFBO> destination;
	if (step.pass->getTarget ().has_value ()) {
	    destination = step.provider->find (step.pass->getTarget ().value ());
	    if (!destination) destination = getScene ().findFBO (step.pass->getTarget ().value ());
	    if (!destination) {
		sLog.error ("Text effect target unavailable: ", step.pass->getTarget ().value ().get ());
		continue;
	    }
	    if (!inTargetSequence) {
		effectInput = input;
		inTargetSequence = true;
	    }
	} else {
	    destination = useMain ? m_effectMain : m_effectSub;
	    if (destination == input) destination = useMain ? m_effectSub : m_effectMain;
	    useMain = destination == m_effectSub;
	}
	step.pass->setDestination (destination);
	step.pass->setInput (input);
	step.pass->setPreviousInput (inTargetSequence ? effectInput : nullptr);
	step.pass->setPosition (m_passPosition);
	step.pass->setTexCoord (m_passTexCoord, 1.0f, 0.0f);
	step.pass->setModelViewProjectionMatrix (&m_effectIdentity);
	step.pass->setModelViewProjectionMatrixInverse (&m_effectIdentity);
	step.pass->setModelMatrix (&m_effectIdentity);
	step.pass->setViewProjectionMatrix (&m_effectIdentity);
	glColorMask (true, true, true, true);
	step.pass->render ();
	input = destination;
	if (!step.pass->getTarget ().has_value ()) {
	    inTargetSequence = false;
	    effectInput.reset ();
	}
    }
    return input;
}

const float& CText::getBrightness () const { return m_effectBrightness; }
const float& CText::getUserAlpha () const { return m_effectUnitAlpha; }
const float& CText::getAlpha () const { return m_effectUnitAlpha; }
const glm::vec3& CText::getColor () const { return m_text.color->value->getVec3 (); }
const glm::vec4& CText::getColor4 () const { return m_effectiveColor4; }
const glm::vec3& CText::getCompositeColor () const { return m_text.color->value->getVec3 (); }

long CText::computeCharacterSize26_6 () const {
    // Native scene text passes the authored point size as 26.6 FreeType units
    // at 300 DPI. Its optional canvas-height factor is disabled for this
    // caller; object scale applies only in the model transform.
    const float pointSize = m_text.pointSize->value->getFloat ();
    if (!std::isfinite (pointSize)) return 64;
    return static_cast<long> (std::clamp (pointSize, 1.0f, 256.0f) * 64.0f);
}

void CText::rebuildTextureFrom (const std::string& text) {
    // Measure actual ink as well as shaped advances so negative left bearings
    // and right overhangs are not clipped. Shape contiguous font-face runs
    // before both width decisions and rasterization.
    const auto decoded = decodeTextCodepoints (text);
    std::vector<char32_t> normalized;
    normalized.reserve (decoded.size ());
    for (const char32_t codepoint : decoded) {
	// Native scene text erase-removes U+000D before splitting on U+000A.
	if (codepoint != '\r') normalized.push_back (codepoint);
    }
    const glm::vec2 authoredSpacing = m_text.spacing->value->getVec2 ();
    const float letterSpacing = std::isfinite (authoredSpacing.x) ? authoredSpacing.x : 0.0f;
    const float rowSpacing = std::isfinite (authoredSpacing.y) ? authoredSpacing.y : 0.0f;
    const float authoredMaxWidth = m_text.maxWidth->value->getFloat ();
    const bool limitWidth = m_text.limitWidth->value->getBool ();
    struct FacedGlyph { FT_Face face; ShapedTextGlyph glyph; };
    const auto shape = [this] (std::span<const char32_t> value) {
        std::vector<FacedGlyph> result;
        for (size_t start = 0; start < value.size ();) {
            FT_Face face = faceForCodepoint (value[start]);
            size_t end = start + 1;
            while (end < value.size () && faceForCodepoint (value[end]) == face) ++end;
            const ShapedTextRun shaped = shapeTextRow (face, value.subspan (start, end - start));
            for (auto glyph : shaped.glyphs) {
                glyph.cluster += static_cast<uint32_t> (start);
                result.push_back ({face, glyph});
            }
            start = end;
        }
        return result;
    };
    std::vector<char32_t> wrapped;
    if (limitWidth && std::isfinite (authoredMaxWidth) && authoredMaxWidth > 0.0f) {
	wrapped.reserve (normalized.size ());
	float rowWidth = 0.0f;
	float pendingWidth = 0.0f;
	std::vector<char32_t> pendingSpaces;
	bool rowHasWord = false;
	const auto measure = [&shape, letterSpacing] (std::span<const char32_t> value) {
	    const auto shaped = shape (value);
	    float width = 0.0f;
	    for (const auto& glyph : shaped)
		width += static_cast<float> (glyph.glyph.xAdvance26_6 >> 6) + letterSpacing;
	    return width;
	};
	for (size_t index = 0; index < normalized.size ();) {
	    const char32_t codepoint = normalized[index];
	    if (codepoint == '\n') {
		wrapped.insert (wrapped.end (), pendingSpaces.begin (), pendingSpaces.end ());
		pendingSpaces.clear ();
		pendingWidth = 0.0f;
		wrapped.push_back ('\n');
		rowWidth = 0.0f;
		rowHasWord = false;
		++index;
		continue;
	    }
	    if (codepoint == ' ' || codepoint == '\t') {
		pendingSpaces.push_back (codepoint);
		pendingWidth = measure (pendingSpaces);
		++index;
		continue;
	    }
	    const size_t wordStart = index;
	    while (index < normalized.size () && normalized[index] != '\n'
		   && normalized[index] != ' ' && normalized[index] != '\t') {
		++index;
	    }
	    const float wordWidth = measure (std::span<const char32_t> (
	        normalized.data () + wordStart, index - wordStart));
	    if (rowHasWord && rowWidth + pendingWidth + wordWidth > authoredMaxWidth) {
		wrapped.push_back ('\n');
		rowWidth = 0.0f;
		pendingSpaces.clear ();
		pendingWidth = 0.0f;
	    } else {
		wrapped.insert (wrapped.end (), pendingSpaces.begin (), pendingSpaces.end ());
		rowWidth += pendingWidth;
		pendingSpaces.clear ();
		pendingWidth = 0.0f;
	    }
	    const auto word = std::span<const char32_t> (
	        normalized.data () + wordStart, index - wordStart);
	    if (wordWidth <= authoredMaxWidth) {
		wrapped.insert (wrapped.end (), word.begin (), word.end ());
		rowWidth += wordWidth;
		rowHasWord = true;
		continue;
	    }
	    // An authored width also constrains a word with no whitespace. The
	    // native width branch can start another row after a non-space glyph.
	    // Split only at shaped cluster boundaries so a ligature or combining cluster stays
	    // intact, and measure each resulting run with the same shaping path used
	    // for rasterization.
	    std::vector<size_t> cuts {0, word.size ()};
	    for (const auto& faced : shape (word))
		if (faced.glyph.cluster > 0 && faced.glyph.cluster < word.size ())
		    cuts.push_back (faced.glyph.cluster);
	    std::sort (cuts.begin (), cuts.end ());
	    cuts.erase (std::unique (cuts.begin (), cuts.end ()), cuts.end ());
	    for (size_t start = 0; start < word.size ();) {
		size_t best = start;
		const auto firstCut = std::upper_bound (cuts.begin (), cuts.end (), start);
		if (letterSpacing >= 0.0f) {
		    // Search measured cluster prefixes without reshaping every prefix
		    // of a long media title. Contextual shaping can make widths
		    // nonmonotonic; the chosen prefix is checked to fit, but it may
		    // break earlier than a complete scan in that case.
		    size_t lo = static_cast<size_t> (firstCut - cuts.begin ());
		    size_t hi = lo;
		    size_t step = 1;
		    const auto fits = [&] (size_t cut) {
			return rowWidth + measure (word.subspan (start, cuts[cut] - start))
			    <= authoredMaxWidth;
		    };
		    // Bracket the next break exponentially. A title thousands of
		    // clusters long then only reshapes nearby prefixes on each row.
		    while (hi < cuts.size () && fits (hi)) {
			best = cuts[hi];
			lo = hi + 1;
			hi += std::min (step, cuts.size () - hi);
			step = step > cuts.size () / 2 ? cuts.size () : step * 2;
		    }
		    while (lo < hi) {
			const size_t mid = lo + (hi - lo) / 2;
			if (fits (mid)) {
			    best = cuts[mid];
			    lo = mid + 1;
			} else hi = mid;
		    }
		} else {
		    // Negative authored spacing need not make prefix widths monotonic.
		    for (auto it = firstCut; it != cuts.end (); ++it)
			if (rowWidth + measure (word.subspan (start, *it - start)) <= authoredMaxWidth)
			    best = *it;
		}
		if (best == start && rowWidth > 0.0f) {
		    wrapped.push_back ('\n');
		    rowWidth = 0.0f;
		    continue;
		}
		if (best == start)
		    best = *std::upper_bound (cuts.begin (), cuts.end (), start);
		const auto segment = word.subspan (start, best - start);
		wrapped.insert (wrapped.end (), segment.begin (), segment.end ());
		rowWidth += measure (segment);
		rowHasWord = true;
		start = best;
		if (start < word.size ()) {
		    wrapped.push_back ('\n');
		    rowWidth = 0.0f;
		}
	    }
	}
	wrapped.insert (wrapped.end (), pendingSpaces.begin (), pendingSpaces.end ());
    } else {
	wrapped = std::move (normalized);
    }
    // Native splits only segments followed by content. A terminal LF ends
    // the last segment and does not create an additional blank layout row.
    if (!wrapped.empty () && wrapped.back () == '\n') wrapped.pop_back ();
    const int authoredMaxRows = m_text.maxRows->value->getInt ();
    const bool limitRows = m_text.limitRows->value->getBool ();
    const bool limitUseEllipsis = m_text.limitUseEllipsis->value->getBool ();
    std::vector<char32_t> codepoints;
    codepoints.reserve (wrapped.size ());
    int acceptedRows = 1;
    for (size_t index = 0; index < wrapped.size (); ++index) {
	const char32_t codepoint = wrapped[index];
	if (codepoint == '\n') {
	    if (limitRows && authoredMaxRows > 0 && acceptedRows >= authoredMaxRows) {
		// The native row-limit path trims terminal whitespace on the
		// retained row before discarding later rows.
		while (!codepoints.empty () && (codepoints.back () == ' '
		       || codepoints.back () == '\t')) codepoints.pop_back ();
		if (limitUseEllipsis && (codepoints.empty () || codepoints.back () != 0x2026))
		    codepoints.push_back (0x2026);
		break;
	    }
	    codepoints.push_back ('\n');
	    ++acceptedRows;
	} else {
	    codepoints.push_back (codepoint);
	}
    }
    // Native adds authored Y spacing directly to the face line metric; it
    // does not clamp overlapping or reverse-order rows.
    const float lineHeight = static_cast<float> (m_ftFace->size->metrics.height >> 6) + rowSpacing;
    int line = 0;
    int minInkX = 0, maxInkX = 0, minInkY = 0, maxInkY = 0;
    float maxAdvance = 0.0f;
    bool hasInk = false;
    struct PlacedGlyph { FT_Face face; FT_UInt index; float x; float baselineY; bool color = false; };
    std::vector<PlacedGlyph> glyphs;
    glyphs.reserve (codepoints.size ());
    std::vector<char32_t> row;
    const auto flushRow = [&] {
	const auto shaped = shape (row);
	float penX = 0.0f;
	float penY = 0.0f;
	for (const auto& faced : shaped) {
	    const auto& glyph = faced.glyph;
	    glyphs.push_back ({faced.face, glyph.glyphIndex,
	        penX + static_cast<float> (glyph.xOffset26_6 >> 6),
	        line * lineHeight - penY - static_cast<float> (glyph.yOffset26_6 >> 6)});
	    penX += static_cast<float> (glyph.xAdvance26_6 >> 6) + letterSpacing;
	    penY += static_cast<float> (glyph.yAdvance26_6 >> 6);
	}
	maxAdvance = std::max (maxAdvance, penX);
	row.clear ();
    };
    for (size_t index = 0; index < codepoints.size (); ++index) {
	const char32_t codepoint = codepoints[index];
	if (codepoint == '\n') {
	    flushRow ();
	    ++line;
	    continue;
	}
	row.push_back (codepoint);
    }
    flushRow ();
    for (auto& glyph : glyphs) {
	if (FT_Load_Glyph (glyph.face, glyph.index, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
	    continue;
	FT_GlyphSlot slot = glyph.face->glyph;
	const auto& bitmap = slot->bitmap;
	glyph.color = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
	if (bitmap.width && bitmap.rows) {
	    const int left = static_cast<int> (std::lround (glyph.x)) + slot->bitmap_left;
	    const int top = static_cast<int> (std::lround (glyph.baselineY)) - slot->bitmap_top;
	    if (!hasInk) {
		minInkX = left;
		maxInkX = left + static_cast<int> (bitmap.width);
		minInkY = top;
		maxInkY = top + static_cast<int> (bitmap.rows);
		hasInk = true;
	    } else {
		minInkX = std::min (minInkX, left);
		maxInkX = std::max (maxInkX, left + static_cast<int> (bitmap.width));
		minInkY = std::min (minInkY, top);
		maxInkY = std::max (maxInkY, top + static_cast<int> (bitmap.rows));
	    }
	}
    }
    const int originX = std::min (0, minInkX);
    const int width = std::max (1, std::max (static_cast<int> (std::ceil (maxAdvance)), maxInkX) - originX);
    const int lineCount = line + 1;
    m_layoutRows = lineCount;
    m_layoutLineHeight = lineHeight;
    const int ascent = std::max (0, static_cast<int> (m_ftFace->size->metrics.ascender >> 6));
    const int descent = std::max (0, static_cast<int> (-m_ftFace->size->metrics.descender >> 6));
    // Empty leading/trailing lines occupy logical line boxes even though they
    // contribute no ink. Keep the old tight single-line bounds for authored
    // text that has no explicit line breaks.
    const int firstOrLastBaseline = static_cast<int> (std::lround ((lineCount - 1) * lineHeight));
    const int logicalTop = std::min (0, firstOrLastBaseline) - ascent;
    const int logicalBottom = std::max (0, firstOrLastBaseline) + descent;
    const int originY = lineCount > 1 ? std::min (minInkY, logicalTop) : hasInk ? minInkY : 0;
    const int bottomY = lineCount > 1
        ? std::max (maxInkY, logicalBottom)
        : hasInk ? maxInkY : 1;
    const int height = std::max (1, bottomY - originY);
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height * 4, 0);
    const glm::vec3 rasterColor = glm::clamp (m_text.color->value->getVec3 (),
                                              glm::vec3 (0.0f), glm::vec3 (1.0f));
    const uint8_t monoRed = static_cast<uint8_t> (std::lround (rasterColor.r * 255.0f));
    const uint8_t monoGreen = static_cast<uint8_t> (std::lround (rasterColor.g * 255.0f));
    const uint8_t monoBlue = static_cast<uint8_t> (std::lround (rasterColor.b * 255.0f));
    const bool opaqueBackground = m_text.opaqueBackground->value->getBool ();
    const float rasterOpacity = std::clamp (m_text.color->value->getVec4 ().a
        * m_text.alpha->value->getFloat (), 0.0f, 1.0f);
    const bool offscreenGlyphBlend = !m_text.effects.empty () && hasVisibleEffects ();
    const bool copyBackdrop = offscreenGlyphBlend && !opaqueBackground;
    // The native clear-alpha preparation retains the scene RGB across the
    // entire text target. Keep the glyph blend's remaining backdrop fraction
    // independently from the target alpha, which uses SRC_ALPHA for alpha too.
    std::vector<uint8_t> transmittance;
    if (copyBackdrop)
        transmittance.assign (static_cast<size_t> (width) * height, 255);
    // Ordinary glyphs use authored RGB even at antialiased zero-alpha edges;
    // the fragment shader then reproduces the old color × coverage blend.
    if (opaqueBackground) {
        const glm::vec3 background = glm::clamp (m_text.backgroundColor->value->getVec3 (),
                                                glm::vec3 (0.0f), glm::vec3 (1.0f));
        const uint8_t channels[4] = {
            static_cast<uint8_t> (std::lround (background.r * 255.0f)),
            static_cast<uint8_t> (std::lround (background.g * 255.0f)),
            static_cast<uint8_t> (std::lround (background.b * 255.0f)),
            static_cast<uint8_t> (std::lround (rasterOpacity * 255.0f)),
        };
        for (size_t index = 0; index < pixels.size (); index += 4)
            std::copy (std::begin (channels), std::end (channels), pixels.begin () + index);
    } else if (!offscreenGlyphBlend)
        for (size_t index = 0; index < pixels.size (); index += 4) {
            pixels[index] = monoRed;
            pixels[index + 1] = monoGreen;
            pixels[index + 2] = monoBlue;
        }

    // Native layout geometry is submitted as monochrome atlas, then color
    // atlas, even if color codepoints precede monochrome ones in the string.
    for (bool colorPass : {false, true}) {
        for (const auto& glyph : glyphs) {
            if (glyph.color != colorPass
                || FT_Load_Glyph (glyph.face, glyph.index, FT_LOAD_RENDER | FT_LOAD_COLOR) != 0)
                continue;
            FT_GlyphSlot slot = glyph.face->glyph;
            const auto& bmp = slot->bitmap;
            const int glyphX = static_cast<int> (std::lround (glyph.x)) + slot->bitmap_left - originX;
            const int glyphY = static_cast<int> (std::lround (glyph.baselineY)) - slot->bitmap_top - originY;
            for (unsigned int row = 0; row < bmp.rows; ++row) {
                for (unsigned int col = 0; col < bmp.width; ++col) {
                    const int dstX = glyphX + static_cast<int> (col);
                    const int dstY = glyphY + static_cast<int> (row);
                    if (dstX < 0 || dstX >= width || dstY < 0 || dstY >= height) continue;
			const uint8_t coverage = static_cast<uint8_t> (std::lround (
			    textBitmapCoverage (bmp, row, col) * rasterOpacity));
                    auto* destination = pixels.data ()
                        + (static_cast<size_t> (dstY) * width + dstX) * 4;
                    if (copyBackdrop) {
                        auto& remaining = transmittance[static_cast<size_t> (dstY) * width + dstX];
                        remaining = static_cast<uint8_t> (
                            (static_cast<int> (remaining) * (255 - coverage) + 127) / 255);
                    }
                    if (glyph.color) {
                        if (destination[3] == 0 && !opaqueBackground)
                            destination[0] = destination[1] = destination[2] = 0;
                        const auto* source = bmp.buffer
                            + static_cast<std::ptrdiff_t> (row) * bmp.pitch
                            + static_cast<std::ptrdiff_t> (col) * 4;
                        if (offscreenGlyphBlend)
                            compositeTextOffscreenRgba (
                                destination, source[2], source[1], source[0], coverage);
                        else
                            compositeTextRgba (
                                destination, source[2], source[1], source[0], coverage);
                    } else {
                        if (offscreenGlyphBlend)
                            compositeTextOffscreenRgba (
                                destination, monoRed, monoGreen, monoBlue, coverage);
                        else
                            compositeTextRgba (
                                destination, monoRed, monoGreen, monoBlue, coverage);
                    }
                }
            }
        }
    }

    const bool firstUpload = (m_glyphTexture == 0);
    if (firstUpload) {
	glGenTextures (1, &m_glyphTexture);
    }
    glBindTexture (GL_TEXTURE_2D, m_glyphTexture);
    GLint previousAlignment = 4, previousRowLength = 0;
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &previousAlignment);
    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &previousRowLength);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data ());
    if (copyBackdrop) {
        if (m_glyphTransmittanceTexture == 0)
            glGenTextures (1, &m_glyphTransmittanceTexture);
        glBindTexture (GL_TEXTURE_2D, m_glyphTransmittanceTexture);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
                      GL_UNSIGNED_BYTE, transmittance.data ());
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture (GL_TEXTURE_2D, m_glyphTexture);
    }
    glPixelStorei (GL_UNPACK_ROW_LENGTH, previousRowLength);
    glPixelStorei (GL_UNPACK_ALIGNMENT, previousAlignment);
    if (firstUpload) {
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    m_textureSize = { width, height };
    m_quadSize = { static_cast<float> (width), static_cast<float> (height) };
    m_lastRenderedText = text;
    m_lastSpacing = authoredSpacing;
    m_lastRasterColor = m_text.color->value->getVec3 ();
    m_lastRasterBackgroundColor = m_text.backgroundColor->value->getVec3 ();
    m_lastRasterOpacity = rasterOpacity;
    m_lastOpaqueBackground = opaqueBackground;
    m_lastOffscreenGlyphBlend = offscreenGlyphBlend;
    m_lastMaxRows = authoredMaxRows;
    m_lastLimitRows = limitRows;
    m_lastMaxWidth = authoredMaxWidth;
    m_lastLimitWidth = limitWidth;
    m_lastLimitUseEllipsis = limitUseEllipsis;

    uploadQuadVertices ();
}

void CText::buildShader () {
    GLuint vs = compileShader (GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader (GL_FRAGMENT_SHADER, kFragmentShader);
    if (vs == 0 || fs == 0) {
	if (vs) {
	    glDeleteShader (vs);
	}
	if (fs) {
	    glDeleteShader (fs);
	}
	return;
    }

    m_program = glCreateProgram ();
    glAttachShader (m_program, vs);
    glAttachShader (m_program, fs);
    glLinkProgram (m_program);
    glDeleteShader (vs);
    glDeleteShader (fs);

    GLint status = GL_FALSE;
    glGetProgramiv (m_program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
	char log[1024];
	glGetProgramInfoLog (m_program, sizeof (log), nullptr, log);
	sLog.error ("CText program link failed: ", log);
	glDeleteProgram (m_program);
	m_program = 0;
	return;
    }

    m_uMVP = glGetUniformLocation (m_program, "uMVP");
    m_uColor = glGetUniformLocation (m_program, "uColor");
    m_uNodeAlpha = glGetUniformLocation (m_program, "uNodeAlpha");
    m_uTexture = glGetUniformLocation (m_program, "uTexture");
    m_uFlipV = glGetUniformLocation (m_program, "uFlipV");
    m_uOpaqueBackground = glGetUniformLocation (m_program, "uOpaqueBackground");
    m_uBackgroundColor = glGetUniformLocation (m_program, "uBackgroundColor");
    m_uGlyphUvOffset = glGetUniformLocation (m_program, "uGlyphUvOffset");
    m_uGlyphUvScale = glGetUniformLocation (m_program, "uGlyphUvScale");
    m_uEffectFinal = glGetUniformLocation (m_program, "uEffectFinal");
    m_uOffscreenBackdrop = glGetUniformLocation (m_program, "uOffscreenBackdrop");
    m_uBackdropTexture = glGetUniformLocation (m_program, "uBackdropTexture");
    m_uTransmittanceTexture = glGetUniformLocation (m_program, "uTransmittanceTexture");
    m_uBackdropMVP = glGetUniformLocation (m_program, "uBackdropMVP");
}

void CText::uploadQuadVertices () {
    // Quad centered at the origin, sized in pixels. Scene-space placement is
    // done via the model matrix using the object's origin/scale. VBO contents
    // are re-uploaded whenever the glyph bitmap is rebuilt so the quad always
    // matches the current texture dimensions.
    const float hx = m_quadSize.x * 0.5f;
    const float hy = m_quadSize.y * 0.5f;
    // With vflip=true (Wayland/GLFW), GL y- = screen top. So the quad bottom (y=-hy,
    // lower GL y) appears at screen top. UV.v=0 here = FT glyph top → shows at screen top ✓
    const float verts[] = {
	// pos        // uv
	-hx, -hy, 0.0f, 0.0f, hx, -hy, 1.0f, 0.0f, hx,  hy, 1.0f, 1.0f,
	-hx, -hy, 0.0f, 0.0f, hx, hy,  1.0f, 1.0f, -hx, hy, 0.0f, 1.0f,
    };

    const bool firstUpload = (m_vao == 0);
    if (firstUpload) {
	glGenVertexArrays (1, &m_vao);
	glGenBuffers (1, &m_vbo);
    }
    glBindVertexArray (m_vao);
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (verts), verts, GL_DYNAMIC_DRAW);
    if (firstUpload) {
	glEnableVertexAttribArray (0);
	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (0));
	glEnableVertexAttribArray (1);
	glVertexAttribPointer (
	    1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), reinterpret_cast<void*> (2 * sizeof (float))
	);
    }
    glBindVertexArray (0);
}

glm::vec2 CText::getLayoutOffset () const {
    float horizontalOffset = 0.0f;
    if (m_horizontalAlign == "left") horizontalOffset = m_quadSize.x * 0.5f;
    else if (m_horizontalAlign == "right") horizontalOffset = -m_quadSize.x * 0.5f;
    float verticalOffset = 0.0f;
    if (m_ftFace && (m_verticalAlign == "top" || m_verticalAlign == "bottom")) {
        const float ascender = static_cast<float> (m_ftFace->size->metrics.ascender >> 6);
        const float descender = static_cast<float> (m_ftFace->size->metrics.descender >> 6);
        const float precedingRows = static_cast<float> ((m_layoutRows - 1) * m_layoutLineHeight);
        const float centerAnchor = (ascender - precedingRows) * 0.5f;
        const float chosenAnchor = m_verticalAlign == "top"
            ? ascender : descender - precedingRows;
        verticalOffset = centerAnchor - chosenAnchor;
    }
    return {horizontalOffset, verticalOffset};
}

void CText::render () {
    if (!m_valid) {
	return;
    }
    const auto transform = Wallpapers::resolveSceneTransform (m_text, [this] (int parentId) -> const Object* {
        const auto* parent = getScene ().getObject (parentId);
        return parent ? &parent->getObject () : nullptr;
    }, [this] (const Object& parent, const std::string& name) {
        return getScene ().getPuppetAttachmentTransform (parent.id, name);
    });
    if (!transform.visible) {
	return;
    }

#if !NDEBUG
    std::string str = "Text " + this->getObject ().name + " (" + std::to_string (this->getObject ().id) + ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif /* DEBUG */
    const std::string currentText = m_text.text->value->getString ();
    const std::string renderedText = currentText.empty () ? std::string (" ") : currentText;

    const long characterSize = computeCharacterSize26_6 ();
    const bool opaqueBackground = m_text.opaqueBackground->value->getBool ();
    const float rasterOpacity = std::clamp (m_text.color->value->getVec4 ().a
        * m_text.alpha->value->getFloat (), 0.0f, 1.0f);
    const bool offscreenGlyphBlend = !m_text.effects.empty () && hasVisibleEffects ();
    if (characterSize != m_lastCharacterSize26_6) {
	m_lastCharacterSize26_6 = characterSize;
	FT_Set_Char_Size (m_ftFace, 0, m_lastCharacterSize26_6, 300, 300);
	for (FT_Face face : m_fallbackFaces)
	    FT_Set_Char_Size (face, 0, m_lastCharacterSize26_6, 300, 300);
	if (m_bundledEmojiFace)
	    FT_Set_Char_Size (m_bundledEmojiFace, 0, m_lastCharacterSize26_6, 300, 300);
	rebuildTextureFrom (renderedText);
    } else if (renderedText != m_lastRenderedText || m_text.spacing->value->getVec2 () != m_lastSpacing
	       || m_text.color->value->getVec3 () != m_lastRasterColor
	       || m_text.backgroundColor->value->getVec3 () != m_lastRasterBackgroundColor
	       || rasterOpacity != m_lastRasterOpacity
	       || opaqueBackground != m_lastOpaqueBackground
	       || offscreenGlyphBlend != m_lastOffscreenGlyphBlend
	       || m_text.maxRows->value->getInt () != m_lastMaxRows
	       || m_text.limitRows->value->getBool () != m_lastLimitRows
	       || m_text.maxWidth->value->getFloat () != m_lastMaxWidth
	       || m_text.limitWidth->value->getBool () != m_lastLimitWidth
	       || m_text.limitUseEllipsis->value->getBool () != m_lastLimitUseEllipsis) {
	rebuildTextureFrom (renderedText);
    }

    const glm::vec4 color = m_text.color->value->getVec4 ();
    // Use the same authored Y-down world transform as images. The final
    // presentation and composition passes both expect this convention; using
    // a second direct-text convention reverses rotated text and moves it to
    // the opposite side of an off-center image at the same authored origin.
    const float scene_w = getScene ().getCamera ().getWidth ();
    const float scene_h = getScene ().getCamera ().getHeight ();
    // Native perspective projects authored XYZ directly. Preserve the
    // orthographic canvas conversion used by existing scene text, including
    // its accepted clock placement.
    glm::mat4 imageWorld = transform.authoredMatrix;
    if (getScene ().getCamera ().isOrthogonal ()) {
	const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
	imageWorld = glm::translate (
	    glm::mat4 (1.0f), Wallpapers::scenePointForCamera (
		glm::vec3 (0.0f), scene_w, scene_h, true))
	    * flip * transform.authoredMatrix;
    }
    // Native scene text moves the centered layout by half its measured width
    // for left/right anchoring. Apply the offset in authored local space so
    // it follows rotated and scaled parents instead of shifting on screen.
    // Native enum: bottom=0, center=1, top=2. Its top branch uses the
    // FreeType size ascender, bottom uses descender minus preceding row
    // heights, and center uses their shared layout reference. Apply only
    // the branch difference here; this quad's absolute center still comes
    // from the Linux raster bounds rather than the native layout object.
    const glm::vec2 layoutOffset = getLayoutOffset ();
    imageWorld = imageWorld * glm::translate (glm::mat4 (1.0f),
	glm::vec3 (layoutOffset, 0.0f));
    // A layer's camera offset is in world space, outside its authored and
    // parent transforms. Effects composite through this same final matrix.
    const glm::vec2 depth = m_text.parallaxDepth->value->getVec2 ();
    const glm::vec3 parallax = m_text.parent
        ? getScene ().getLayerParallaxOffset (depth)
        : getScene ().getLayerParallaxOffset (m_text.origin->value->getVec3 (), depth);
    imageWorld = glm::translate (glm::mat4 (1.0f), parallax) * imageWorld;
    glm::mat4 mvp = getScene ().getCamera ().getProjection ()
	* getScene ().getCamera ().getLookAt () * imageWorld;
    if (getScene ().isChildCompositionScope ()) {
	mvp = getScene ().getActiveRenderProjection () * imageWorld;
    }

    const auto sceneTarget = getScene ().getActiveRenderTarget ();
    const bool useEffects = hasVisibleEffects () && m_textTarget;
    glm::mat4 drawMvp = mvp;
    if (useEffects) {
	resizeEffectTargets ();
	m_textTarget->clear (glm::vec4 (0.0f));
	glBindFramebuffer (GL_FRAMEBUFFER, m_textTarget->getFramebuffer ());
	glViewport (0, 0, m_textTarget->getRealWidth (), m_textTarget->getRealHeight ());
	glColorMask (true, true, true, true);
	glDisable (GL_BLEND);
	drawMvp = glm::ortho (-m_effectTargetSize.x * 0.5f, m_effectTargetSize.x * 0.5f,
	                      -m_effectTargetSize.y * 0.5f, m_effectTargetSize.y * 0.5f)
	    * glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    } else {
	glBindFramebuffer (GL_FRAMEBUFFER, sceneTarget->getFramebuffer ());
	glViewport (0, 0, sceneTarget->getRealWidth (), sceneTarget->getRealHeight ());
	glColorMask (true, true, true, getScene ().isChildCompositionScope ());
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate (GL_FUNC_ADD,
	    getScene ().isMaxAlphaCompositionScope () ? GL_MAX : GL_FUNC_ADD);
	if (getScene ().isMaxAlphaCompositionScope ())
	    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    }
    glUseProgram (m_program);
    glUniform4f (m_uColor, color.r, color.g, color.b,
                 1.0f);
    glUniform1f (m_uNodeAlpha, opaqueBackground ? rasterOpacity : 1.0f);
    glUniform1i (m_uEffectFinal, 0);
    glUniform1i (m_uOffscreenBackdrop, useEffects && !opaqueBackground ? 1 : 0);
    const glm::vec2 paddedSize = useEffects
        ? glm::vec2 (m_effectTargetSize)
        : m_quadSize + 2.0f * glm::clamp (m_text.padding->value->getVec2 (),
              glm::vec2 (0.0f), glm::vec2 (512.0f));
    if (opaqueBackground || (useEffects && offscreenGlyphBlend)) {
	// Native text expands its background target by twice the authored
	// padding, capped at 512 on each axis. Composite its background and
	// glyph coverage in one draw so scene alpha applies only once.
	const glm::vec2 pad = (paddedSize - m_quadSize) * 0.5f;
	drawMvp = drawMvp * glm::scale (glm::mat4 (1.0f),
	    glm::vec3 (paddedSize / m_quadSize, 1.0f));
	const glm::vec4 background = m_text.backgroundColor->value->getVec4 ();
	glUniform4f (m_uBackgroundColor, background.r, background.g, background.b, 1.0f);
	glUniform2f (m_uGlyphUvOffset, pad.x / paddedSize.x, pad.y / paddedSize.y);
	glUniform2f (m_uGlyphUvScale, paddedSize.x / m_quadSize.x, paddedSize.y / m_quadSize.y);
    }
    glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (drawMvp));
    const glm::mat4 backdropMvp = mvp * glm::scale (glm::mat4 (1.0f),
        glm::vec3 (paddedSize / m_quadSize, 1.0f));
    glUniformMatrix4fv (m_uBackdropMVP, 1, GL_FALSE, glm::value_ptr (backdropMvp));
    glUniform1i (m_uOpaqueBackground, opaqueBackground ? 1 : 0);

    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, m_glyphTexture);
    glUniform1i (m_uTexture, 0);
    glUniform1i (m_uFlipV, 1);
    if (useEffects && !opaqueBackground) {
        glActiveTexture (GL_TEXTURE1);
        glBindTexture (GL_TEXTURE_2D, sceneTarget->getTextureID (0));
        glUniform1i (m_uBackdropTexture, 1);
        glActiveTexture (GL_TEXTURE2);
        glBindTexture (GL_TEXTURE_2D, m_glyphTransmittanceTexture);
        glUniform1i (m_uTransmittanceTexture, 2);
        glActiveTexture (GL_TEXTURE0);
    }

    glBindVertexArray (m_vao);
    glDrawArrays (GL_TRIANGLES, 0, 6);
    if (useEffects) {
	const auto effected = runEffects ();
	glBindFramebuffer (GL_FRAMEBUFFER, sceneTarget->getFramebuffer ());
	glViewport (0, 0, sceneTarget->getRealWidth (), sceneTarget->getRealHeight ());
	glColorMask (true, true, true, getScene ().isChildCompositionScope ());
	glEnable (GL_BLEND);
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquationSeparate (GL_FUNC_ADD,
	    getScene ().isMaxAlphaCompositionScope () ? GL_MAX : GL_FUNC_ADD);
	if (getScene ().isMaxAlphaCompositionScope ())
	    glBlendFuncSeparate (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
	const glm::mat4 finalMvp = mvp * glm::scale (glm::mat4 (1.0f),
	    glm::vec3 (glm::vec2 (m_effectTargetSize) / m_quadSize, 1.0f));
	glUseProgram (m_program);
	glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (finalMvp));
        glUniform1f (m_uNodeAlpha, 1.0f);
	glUniform1i (m_uEffectFinal, 1);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, effected->getTextureID (0));
	glBindVertexArray (m_vao);
	glDrawArrays (GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray (0);
#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}
