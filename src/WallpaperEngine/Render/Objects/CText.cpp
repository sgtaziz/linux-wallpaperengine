#include "CText.h"
#include "TextRaster.h"
#include "TextDrawState.h"
#include "TextCodepoints.h"
#include "TextShaping.h"
#include "TextLayout.h"
#include "TextRowLayout.h"

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
uniform vec4 uBackgroundRect;
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
        bool withinBackground = all(greaterThanEqual(vUV, uBackgroundRect.xy)) &&
            all(lessThanEqual(vUV, uBackgroundRect.zw));
        FragColor = withinGlyph ? glyph
            : withinBackground ? vec4(uBackgroundColor.rgb, uNodeAlpha) : vec4(0.0);
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
    m_horizontalAlign (text.alignment), m_verticalAlign (text.verticalalign), m_font (text.font) {
    for (const auto& binding : Scripting::scriptPropertyBindings (text))
	this->registerProperty (binding.name, binding.value);
    // Effect visibility and shader constants retain their own UserSettings.
    // Queue nested scripts without exposing synthetic names on thisLayer.
    // CPass reads these same per-instance DynamicValues on subsequent frames.
    for (size_t effectIndex = 0; effectIndex < text.effects.size (); ++effectIndex) {
	const auto& effect = text.effects[effectIndex];
	const std::string prefix = "textEffect" + std::to_string (effectIndex)
	    + "_object" + std::to_string (getId ()) + "_";
	// Native binds the effect's keys, then each pass in order with
	// its constantshadervalues sorted.
	auto queue = [this, &prefix, effectIndex] (const std::string& suffix, DynamicValue& value, uint32_t pass,
	                                         const std::string& jsonKey) {
	    getScene ().getScriptEngine ().queueScript (prefix + suffix, value, *this,
	        Scripting::ScriptPlacement {.phase = Scripting::ScriptPhase::AfterOwn,
	                                    .group = static_cast<uint32_t> (effectIndex), .subgroup = pass,
	                                    .key = jsonKey});
	};
	queue ("visible", *effect->visible->value, 0, "visible");
	for (size_t passIndex = 0; passIndex < effect->effect->passes.size (); ++passIndex) {
	    const auto& pass = effect->effect->passes[passIndex];
	    if (!pass->material) continue;
	    for (size_t materialIndex = 0; materialIndex < (*pass->material)->passes.size (); ++materialIndex) {
		for (const auto& [name, value] : (*pass->material)->passes[materialIndex]->constants)
		    queue ("pass" + std::to_string (passIndex) + "_material"
		           + std::to_string (materialIndex) + "_constant_" + name, *value->value,
		           static_cast<uint32_t> (passIndex + 1), name);
	    }
	}
	for (size_t passIndex = 0; passIndex < effect->passOverrides.size (); ++passIndex) {
	    for (const auto& [name, value] : effect->passOverrides[passIndex]->constants)
		queue ("override" + std::to_string (passIndex)
		       + "_constant_" + name, *value->value, static_cast<uint32_t> (passIndex + 1), name);
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
    m_loadedFont = m_font;

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
    if (m_font.empty () || m_font.rfind ("systemfont_", 0) == 0) {
	return false;
    }

    try {
	auto stream = getAssetLocator ().read (m_font);
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

	sLog.error ("CText: FT_New_Memory_Face failed for '", m_font, "', falling back to system font");
    } catch (const std::exception& e) {
	sLog.error ("CText: cannot read font '", m_font, "': ", e.what (), ", falling back to system font");
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

// Called when a script wrote a different `font`. The native relayout
// rebuilds the text with the stored font, falling back the same
// way setup () does. If no face loads at all, the previous face is kept.
bool CText::reloadFont () {
    m_loadedFont = m_font;
    FT_Face previous = m_ftFace;
    std::vector<uint8_t> previousData = std::move (m_fontData);
    m_fontData.clear ();
    m_ftFace = nullptr;
    if (!loadEmbeddedFont () && !loadSystemFont ()) {
	m_ftFace = previous;
	m_fontData = std::move (previousData);
	return false;
    }
    if (previous != nullptr) FT_Done_Face (previous);
    // Fallback choices were made against the previous primary face.
    m_characterFaces.clear ();
    FT_Set_Char_Size (m_ftFace, 0, m_lastCharacterSize26_6, 300, 300);
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
    // Every face lacks the scalar: native treats it as exhausted.
    if (FT_Get_Char_Index (selected, static_cast<FT_ULong> (codepoint)) == 0)
        selected = nullptr;
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
    // Native: the layout box plus padding, truncated to integers.
    const glm::vec2 dimensions = getLayoutSize ();
    const glm::ivec2 size = glm::max (glm::ivec2 (1), glm::ivec2 (dimensions));
    if (size == m_effectTargetSize && m_textTarget) return;
    m_effectTargetSize = size;
    const auto width = static_cast<uint32_t> (size.x);
    const auto height = static_cast<uint32_t> (size.y);
    if (!m_textTarget) {
        // Native selects enum14 (RGBA16_FLOAT) for the text's
        // retained/effect targets when the rendering context is HDR-active.
        const auto format = getScene ().isHdrPostprocessingActive ()
            ? TextureFormat_RGBA16161616f : TextureFormat_ARGB8888;
	m_textTarget = std::make_shared<CFBO> ("_text_base", format,
	    TextureFlags_NoFlags, 1.0f, width, height, width, height);
	m_effectMain = std::make_shared<CFBO> ("_text_main", format,
	    TextureFlags_NoFlags, 1.0f, width, height, width, height);
	m_effectSub = std::make_shared<CFBO> ("_text_sub", format,
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

glm::vec2 CText::layerPadding () const {
    const glm::vec2 authored = m_text.padding->value->getVec2 ();
    // The saved layer sizes of installed scenes count only visible effects.
    const bool effects = hasVisibleEffects ();
    const bool opaque = m_text.opaqueBackground->value->getBool ();
    return {textLayerPadding (authored.x, effects, opaque), textLayerPadding (authored.y, effects, opaque)};
}

glm::vec2 CText::getLayoutSize () const {
    return glm::vec2 (m_layoutBox.width (), m_layoutBox.height ()) + 2.0f * layerPadding ();
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
    // and right overhangs are not clipped. Rows follow native:
    // face runs are shaped before width decisions and rasterization.
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
    const int authoredMaxRows = m_text.maxRows->value->getInt ();
    const bool limitRows = m_text.limitRows->value->getBool ();
    const bool limitUseEllipsis = m_text.limitUseEllipsis->value->getBool ();
    TextLayoutLimits limits;
    if (limitWidth && std::isfinite (authoredMaxWidth) && authoredMaxWidth > 0.0f)
        limits.maxWidth = authoredMaxWidth;
    if (limitRows && authoredMaxRows > 0) limits.maxRows = authoredMaxRows;
    limits.ellipsis = limitUseEllipsis;
    limits.letterSpacing = letterSpacing;
    limits.primary = m_ftFace;
    const auto rows = layoutTextRows (normalized,
        [this] (char32_t codepoint) { return faceForCodepoint (codepoint); }, limits);
    // Native adds authored Y spacing directly to the face line metric; it
    // does not clamp overlapping or reverse-order rows.
    const float lineHeight = static_cast<float> (m_ftFace->size->metrics.height >> 6) + rowSpacing;
    m_layoutBox = textLayoutBox (rows, letterSpacing, lineHeight,
                                 static_cast<float> (m_ftFace->size->metrics.ascender >> 6));
    int minInkX = 0, maxInkX = 0, minInkY = 0, maxInkY = 0;
    float maxAdvance = 0.0f;
    bool hasInk = false;
    struct PlacedGlyph { FT_Face face; FT_UInt index; float x; float baselineY; bool color = false; };
    std::vector<PlacedGlyph> glyphs;
    struct LayoutRow { size_t begin; size_t end; TextLayoutRange bounds; };
    std::vector<LayoutRow> layoutRows;
    float maxRowWidth = 0.0f;
    for (size_t line = 0; line < rows.size (); ++line) {
	LayoutRow layoutRow {glyphs.size (), 0, {}};
	float penX = 0.0f;
	float penY = 0.0f;
	for (const auto& laid : rows[line].glyphs) {
	    const auto& glyph = laid.glyph;
	    glyphs.push_back ({laid.face, glyph.glyphIndex,
	        penX + static_cast<float> (glyph.xOffset26_6 >> 6),
	        static_cast<float> (line) * lineHeight - penY - static_cast<float> (glyph.yOffset26_6 >> 6)});
	    // Bitmap-only fallback faces have no outline CBox and keep their
	    // rendered bounds rather than dropping their layout.
	    float low = 0.0f, high = 0.0f;
	    if (textGlyphHorizontalInk (laid.face, glyph.glyphIndex, low, high))
	        layoutRow.bounds.include (glyphs.back ().x + low, glyphs.back ().x + high);
	    penX += static_cast<float> (glyph.xAdvance26_6 >> 6) + letterSpacing;
	    penY += static_cast<float> (glyph.yAdvance26_6 >> 6);
	}
	maxAdvance = std::max (maxAdvance, penX);
	layoutRow.end = glyphs.size ();
	maxRowWidth = std::max (maxRowWidth, layoutRow.bounds.width ());
	layoutRows.push_back (layoutRow);
    }
    TextLayoutRange layoutBounds;
    for (const auto& layoutRow : layoutRows) {
        const float offset = textRowAlignmentOffset (layoutRow.bounds.width (), maxRowWidth,
                                                     m_horizontalAlign);
        // Native retains the pre-alignment global bounds; row offsets move
        // mesh vertices without expanding the measured layout width.
        layoutBounds.include (layoutRow.bounds.min, layoutRow.bounds.max);
        for (size_t index = layoutRow.begin; index < layoutRow.end; ++index)
            glyphs[index].x += offset;
    }
    m_layoutMinX = layoutBounds.min;
    m_layoutMaxX = layoutBounds.max;
    m_lastRasterHorizontalAlign = m_horizontalAlign;
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
    // Every row record counts, including rows without glyphs.
    const int lineCount = static_cast<int> (rows.size ());
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
    // Retain the bitmap's baseline datum independently of its padded or
    // tight extent. Native glyph layout anchors raw baselines, not ink centers.
    m_rasterCenter = {originX + width * 0.5f, -(originY + height * 0.5f)};
    std::vector<uint8_t> pixels (static_cast<size_t> (width) * height * 4, 0);
    const float brightness = getScene ().isHdrPostprocessingActive ()
        ? m_text.brightness->value->getFloat () : 1.0f;
    const float backgroundBrightness = getScene ().isHdrPostprocessingActive ()
        ? m_text.backgroundBrightness->value->getFloat () : 1.0f;
    // Keep accepted unit/LDR rasterization byte-identical. Nonunit native HDR
    // factors apply to the separate fill/background inputs before composition,
    // with a floating target so RGB > 1 reaches coverage and later effects.
    const bool floatRaster = brightness != 1.0f
        || (m_text.opaqueBackground->value->getBool () && backgroundBrightness != 1.0f);
    std::vector<float> floatPixels;
    if (floatRaster) floatPixels.assign (pixels.size (), 0.0f);
    const glm::vec3 rasterColor = glm::clamp (m_text.color->value->getVec3 (),
                                              glm::vec3 (0.0f), glm::vec3 (1.0f));
    const glm::vec3 monoFloatColor = rasterColor * brightness;
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
        // Native draws the background over the layout box plus
        // padding only; raster pixels outside it start as plain glyph texels.
        const glm::vec2 pad = layerPadding ();
        const float left = m_layoutBox.minX - pad.x - static_cast<float> (originX);
        const float right = m_layoutBox.maxX + pad.x - static_cast<float> (originX);
        const float top = -m_layoutBox.maxY - pad.y - static_cast<float> (originY);
        const float bottom = -m_layoutBox.minY + pad.y - static_cast<float> (originY);
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                const size_t index = (static_cast<size_t> (row) * width + col) * 4;
                const float x = static_cast<float> (col) + 0.5f, y = static_cast<float> (row) + 0.5f;
                if (x < left || x > right || y < top || y > bottom) {
                    pixels[index] = monoRed;
                    pixels[index + 1] = monoGreen;
                    pixels[index + 2] = monoBlue;
                    if (floatRaster) {
                        floatPixels[index] = monoFloatColor.r;
                        floatPixels[index + 1] = monoFloatColor.g;
                        floatPixels[index + 2] = monoFloatColor.b;
                    }
                    continue;
                }
                std::copy (std::begin (channels), std::end (channels), pixels.begin () + index);
                if (floatRaster) {
                    floatPixels[index] = background.r * backgroundBrightness;
                    floatPixels[index + 1] = background.g * backgroundBrightness;
                    floatPixels[index + 2] = background.b * backgroundBrightness;
                    floatPixels[index + 3] = channels[3] / 255.0f;
                }
            }
        }
    } else if (!offscreenGlyphBlend)
        for (size_t index = 0; index < pixels.size (); index += 4) {
            pixels[index] = monoRed;
            pixels[index + 1] = monoGreen;
            pixels[index + 2] = monoBlue;
            if (floatRaster) {
                floatPixels[index] = monoFloatColor.r;
                floatPixels[index + 1] = monoFloatColor.g;
                floatPixels[index + 2] = monoFloatColor.b;
            }
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
                    if (floatRaster) {
                        auto* floatDestination = floatPixels.data ()
                            + (static_cast<size_t> (dstY) * width + dstX) * 4;
                        glm::vec3 sourceColor = monoFloatColor;
                        if (glyph.color) {
                            // COLORFONT samples the atlas RGB and only the
                            // node alpha; monochrome brightness never tints it.
                            if (floatDestination[3] == 0.0f && !opaqueBackground)
                                floatDestination[0] = floatDestination[1] = floatDestination[2] = 0.0f;
                            const auto* source = bmp.buffer
                                + static_cast<std::ptrdiff_t> (row) * bmp.pitch
                                + static_cast<std::ptrdiff_t> (col) * 4;
                            sourceColor = glm::vec3 (source[2], source[1], source[0]) / 255.0f;
                        }
                        if (offscreenGlyphBlend)
                            compositeTextOffscreenRgba (floatDestination,
                                sourceColor.r, sourceColor.g, sourceColor.b, coverage);
                        else
                            compositeTextRgba (floatDestination,
                                sourceColor.r, sourceColor.g, sourceColor.b, coverage);
                        continue;
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
    if (floatRaster)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, floatPixels.data ());
    else
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
    m_lastPadding = m_text.padding->value->getVec2 ();
    m_lastRasterColor = m_text.color->value->getVec3 ();
    m_lastRasterBackgroundColor = m_text.backgroundColor->value->getVec3 ();
    m_lastRasterOpacity = rasterOpacity;
    m_lastRasterBrightness = brightness;
    m_lastRasterBackgroundBrightness = backgroundBrightness;
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
    m_uBackgroundRect = glGetUniformLocation (m_program, "uBackgroundRect");
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
    const float horizontalOffset = textHorizontalAnchorOffset (m_rasterCenter.x,
        {m_layoutMinX, m_layoutMaxX}, m_horizontalAlign);
    float verticalOffset = 0.0f;
    if (m_ftFace) {
        const float ascender = static_cast<float> (m_ftFace->size->metrics.ascender >> 6);
        const float descender = static_cast<float> (m_ftFace->size->metrics.descender >> 6);
        verticalOffset = textVerticalAnchorOffset (m_rasterCenter.y, ascender, descender,
            m_layoutRows, m_layoutLineHeight, m_verticalAlign);
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

    const bool fontChanged = m_font != m_loadedFont && reloadFont ();
    const long characterSize = computeCharacterSize26_6 ();
    const bool opaqueBackground = m_text.opaqueBackground->value->getBool ();
    const float rasterOpacity = std::clamp (m_text.color->value->getVec4 ().a
        * m_text.alpha->value->getFloat (), 0.0f, 1.0f);
    const bool offscreenGlyphBlend = !m_text.effects.empty () && hasVisibleEffects ();
    const float brightness = getScene ().isHdrPostprocessingActive ()
        ? m_text.brightness->value->getFloat () : 1.0f;
    const float backgroundBrightness = getScene ().isHdrPostprocessingActive ()
        ? m_text.backgroundBrightness->value->getFloat () : 1.0f;
    if (characterSize != m_lastCharacterSize26_6) {
	m_lastCharacterSize26_6 = characterSize;
	FT_Set_Char_Size (m_ftFace, 0, m_lastCharacterSize26_6, 300, 300);
	for (FT_Face face : m_fallbackFaces)
	    FT_Set_Char_Size (face, 0, m_lastCharacterSize26_6, 300, 300);
	if (m_bundledEmojiFace)
	    FT_Set_Char_Size (m_bundledEmojiFace, 0, m_lastCharacterSize26_6, 300, 300);
	rebuildTextureFrom (renderedText);
    } else if (fontChanged || renderedText != m_lastRenderedText
               || m_horizontalAlign != m_lastRasterHorizontalAlign
               || m_text.spacing->value->getVec2 () != m_lastSpacing
               || (opaqueBackground && m_text.padding->value->getVec2 () != m_lastPadding)
	       || m_text.color->value->getVec3 () != m_lastRasterColor
	       || m_text.backgroundColor->value->getVec3 () != m_lastRasterBackgroundColor
	       || rasterOpacity != m_lastRasterOpacity
	       || brightness != m_lastRasterBrightness
	       || backgroundBrightness != m_lastRasterBackgroundBrightness
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
    // Convert this centered bitmap back to the raw glyph baseline, then
    // apply native layout bounds and font-metric anchors in authored local
    // space. The full rotated/scaled parent basis carries this translation.
    const glm::vec2 layoutOffset = getLayoutOffset ();
    imageWorld = imageWorld * glm::translate (glm::mat4 (1.0f),
	glm::vec3 (layoutOffset, 0.0f));
    // A layer's camera offset is in world space, outside its authored and
    // parent transforms. Effects composite through this same final matrix.
    imageWorld = glm::translate (glm::mat4 (1.0f), getScene ().getLayerParallaxOffset (m_text))
        * imageWorld;
    glm::mat4 mvp = getScene ().getActiveRenderProjection () * imageWorld;

    const auto sceneTarget = getScene ().getActiveRenderTarget ();
    const bool useEffects = hasVisibleEffects () && m_textTarget;
    if (useEffects) resizeEffectTargets ();
    // Quad-local rectangles, y up, relative to the raster center that `mvp`
    // places. Native draws the background over the layout box
    // plus padding; draws into a target that holds that box at
    // (padding, padding).
    const glm::vec2 layerPad = layerPadding ();
    const glm::vec2 boxCenter = glm::vec2 (m_layoutBox.centerX (), m_layoutBox.centerY ()) - m_rasterCenter;
    const glm::vec2 boxHalf = glm::vec2 (m_layoutBox.width (), m_layoutBox.height ()) * 0.5f;
    const glm::vec2 backgroundMin = boxCenter - boxHalf - layerPad;
    const glm::vec2 backgroundMax = boxCenter + boxHalf + layerPad;
    const glm::vec2 targetSize (m_effectTargetSize);
    const glm::vec2 targetCenter = boxCenter - boxHalf - layerPad + targetSize * 0.5f;
    const glm::mat4 targetQuad = glm::translate (glm::mat4 (1.0f), glm::vec3 (targetCenter, 0.0f))
        * glm::scale (glm::mat4 (1.0f), glm::vec3 (targetSize / m_quadSize, 1.0f));
    glm::mat4 drawMvp = mvp;
    if (useEffects) {
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
    if (opaqueBackground || (useEffects && offscreenGlyphBlend)) {
	// Composite the background and glyph coverage in one draw so scene
	// alpha applies only once. The direct quad covers the background and
	// the whole raster, whose ink native leaves unclipped; the effect
	// quad covers its target.
	const glm::vec2 rasterHalf = m_quadSize * 0.5f;
	const glm::vec2 drawMin = useEffects ? targetCenter - targetSize * 0.5f
	    : glm::min (backgroundMin, -rasterHalf);
	const glm::vec2 drawMax = useEffects ? targetCenter + targetSize * 0.5f
	    : glm::max (backgroundMax, rasterHalf);
	const glm::vec2 drawSize = drawMax - drawMin;
	const glm::vec2 drawCenter = (drawMin + drawMax) * 0.5f;
	drawMvp = drawMvp * glm::translate (glm::mat4 (1.0f),
	        glm::vec3 (useEffects ? drawCenter - targetCenter : drawCenter, 0.0f))
	    * glm::scale (glm::mat4 (1.0f), glm::vec3 (drawSize / m_quadSize, 1.0f));
	const glm::vec4 background = m_text.backgroundColor->value->getVec4 ();
	glUniform4f (m_uBackgroundColor, background.r * backgroundBrightness,
            background.g * backgroundBrightness, background.b * backgroundBrightness, 1.0f);
	const glm::vec2 glyphOffset = (-rasterHalf - drawMin) / drawSize;
	glUniform2f (m_uGlyphUvOffset, glyphOffset.x, glyphOffset.y);
	glUniform2f (m_uGlyphUvScale, drawSize.x / m_quadSize.x, drawSize.y / m_quadSize.y);
	const glm::vec2 backgroundLow = (backgroundMin - drawMin) / drawSize;
	const glm::vec2 backgroundHigh = (backgroundMax - drawMin) / drawSize;
	glUniform4f (m_uBackgroundRect, backgroundLow.x, backgroundLow.y, backgroundHigh.x, backgroundHigh.y);
    }
    glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (drawMvp));
    const glm::mat4 backdropMvp = mvp * targetQuad;
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

    applyTextDrawState ();
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
	const glm::mat4 finalMvp = mvp * targetQuad;
	glUseProgram (m_program);
	glUniformMatrix4fv (m_uMVP, 1, GL_FALSE, glm::value_ptr (finalMvp));
        glUniform1f (m_uNodeAlpha, 1.0f);
	glUniform1i (m_uEffectFinal, 1);
	glActiveTexture (GL_TEXTURE0);
	glBindTexture (GL_TEXTURE_2D, effected->getTextureID (0));
	applyTextDrawState ();
	glBindVertexArray (m_vao);
	glDrawArrays (GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray (0);
#if !NDEBUG
    glPopDebugGroup ();
#endif /* DEBUG */
}
