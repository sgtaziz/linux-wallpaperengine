#include "CParticle.h"
#include "ParticleRemapOperators.h"
#include "ParticleNativeSlotScope.h"
#include "ParticleCore.h"
#include "ParticleBirthGeometry.h"
#include "ParticleInitialColor.h"
#include "ParticleControlPointConstraints.h"
#include "ParticleChildControlPoints.h"
#include "ParticleImageEmitterReadback.h"
#include "CImage.h"

#include "WallpaperEngine/Audio/AudioContext.h"
#include "WallpaperEngine/Audio/ParticleAudioResponse.h"
#include "WallpaperEngine/Data/Model/Property.h"
#include "WallpaperEngine/Data/Parsers/ObjectParser.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Maths.h"
#include "WallpaperEngine/Render/Utils/NoiseUtils.h"
#include "WallpaperEngine/Render/Utils/NativeParticleGradientNoise.h"
#include "WallpaperEngine/Render/TextureAnimation.h"
#include "WallpaperEngine/Scripting/ScriptPropertyBindings.h"
#include "WallpaperEngine/Scripting/ParticleScriptBindings.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"

#include <GL/glew.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>

extern float g_Time;

using namespace WallpaperEngine::Render::Objects;
using namespace WallpaperEngine::Render::Utils;
using namespace WallpaperEngine::Data::Model;

ParticleImageEmitterReadback::~ParticleImageEmitterReadback () {
    if (m_program) glDeleteProgram (m_program);
    if (m_vertexArray) glDeleteVertexArrays (1, &m_vertexArray);
}

bool ParticleImageEmitterReadback::ensureProgram () {
    if (m_program) return true;
    constexpr const char* vertexSource = R"glsl(#version 330 core
out vec2 uv;
uniform vec2 sourceExtent;
void main() {
    vec2 clip = gl_VertexID == 0 ? vec2(-1.0, -1.0)
        : (gl_VertexID == 1 ? vec2(3.0, -1.0) : vec2(-1.0, 3.0));
    uv = (clip * 0.5 + 0.5) * sourceExtent;
    gl_Position = vec4(clip, 0.0, 1.0);
}
)glsl";
    constexpr const char* fragmentSource = R"glsl(#version 330 core
in vec2 uv;
uniform sampler2D sourceImage;
uniform sampler2D opacityMask;
uniform int hasOpacityMask;
uniform vec2 sourceTexel;
out vec4 color;
void main() {
    vec2 delta = sourceTexel * 2.0;
    color = 0.25 * (
        texture(sourceImage, uv - delta) + texture(sourceImage, uv + delta)
        + texture(sourceImage, uv + vec2(-delta.x, delta.y))
        + texture(sourceImage, uv + vec2(delta.x, -delta.y)));
    if (hasOpacityMask != 0) color.a *= texture(opacityMask, uv).r;
}
)glsl";
    auto compile = [] (GLenum type, const char* source) -> GLuint {
        GLuint shader = glCreateShader (type);
        glShaderSource (shader, 1, &source, nullptr);
        glCompileShader (shader);
        GLint okay = GL_FALSE;
        glGetShaderiv (shader, GL_COMPILE_STATUS, &okay);
        if (okay != GL_TRUE) {
            glDeleteShader (shader);
            return 0;
        }
        return shader;
    };
    const GLuint vertex = compile (GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compile (GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader (vertex);
        if (fragment) glDeleteShader (fragment);
        return false;
    }
    const GLuint program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);
    GLint okay = GL_FALSE;
    glGetProgramiv (program, GL_LINK_STATUS, &okay);
    if (okay != GL_TRUE) {
        glDeleteProgram (program);
        return false;
    }
    glGenVertexArrays (1, &m_vertexArray);
    m_program = program;
    return true;
}

bool ParticleImageEmitterReadback::sample (
    const TextureProvider& source, const TextureProvider* mask,
    std::vector<ParticleCore::ImageEmitterSample>& output) {
    const uint32_t sourceWidth = source.getRealWidth ();
    const uint32_t sourceHeight = source.getRealHeight ();
    const glm::uvec2 size = ParticleCore::imageEmitterReadbackSize (sourceWidth, sourceHeight);
    const uint32_t textureWidth = source.getTextureWidth (0);
    const uint32_t textureHeight = source.getTextureHeight (0);
    if (size.x == 0 || size.y == 0 || textureWidth == 0 || textureHeight == 0
        || !ensureProgram ()) return false;
    std::vector<uint8_t> rgba (static_cast<size_t> (size.x) * size.y * 4);

    GLint oldDraw = 0, oldRead = 0, oldProgram = 0, oldVao = 0;
    GLint oldActiveTexture = 0, oldTexture = 0, oldSampler = 0;
    GLint oldMaskTexture = 0, oldMaskSampler = 0, oldPack = 0;
    GLint oldPackRow = 0, oldPackSkipRows = 0, oldPackSkipPixels = 0;
    GLint oldPackBuffer = 0, oldUnpackBuffer = 0, oldViewport[4] {};
    GLboolean oldColorMask[4] {};
    const GLboolean oldDepth = glIsEnabled (GL_DEPTH_TEST);
    const GLboolean oldBlend = glIsEnabled (GL_BLEND);
    const GLboolean oldCull = glIsEnabled (GL_CULL_FACE);
    const GLboolean oldScissor = glIsEnabled (GL_SCISSOR_TEST);
    glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &oldDraw);
    glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &oldRead);
    glGetIntegerv (GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv (GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetIntegerv (GL_VIEWPORT, oldViewport);
    glGetIntegerv (GL_PACK_ALIGNMENT, &oldPack);
    glGetIntegerv (GL_PACK_ROW_LENGTH, &oldPackRow);
    glGetIntegerv (GL_PACK_SKIP_ROWS, &oldPackSkipRows);
    glGetIntegerv (GL_PACK_SKIP_PIXELS, &oldPackSkipPixels);
    glGetIntegerv (GL_PIXEL_PACK_BUFFER_BINDING, &oldPackBuffer);
    glGetIntegerv (GL_PIXEL_UNPACK_BUFFER_BINDING, &oldUnpackBuffer);
    glGetBooleanv (GL_COLOR_WRITEMASK, oldColorMask);
    glActiveTexture (GL_TEXTURE0);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv (GL_SAMPLER_BINDING, &oldSampler);
    glActiveTexture (GL_TEXTURE1);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &oldMaskTexture);
    glGetIntegerv (GL_SAMPLER_BINDING, &oldMaskSampler);
    glActiveTexture (GL_TEXTURE0);

    GLuint texture = 0, framebuffer = 0;
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures (1, &texture);
    glBindTexture (GL_TEXTURE_2D, texture);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei> (size.x),
        static_cast<GLsizei> (size.y), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers (1, &framebuffer);
    glBindFramebuffer (GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    const bool complete = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (complete) {
        glViewport (0, 0, static_cast<GLsizei> (size.x), static_cast<GLsizei> (size.y));
        glDisable (GL_DEPTH_TEST);
        glDisable (GL_BLEND);
        glDisable (GL_CULL_FACE);
        glDisable (GL_SCISSOR_TEST);
        glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram (m_program);
        glUniform1i (glGetUniformLocation (m_program, "sourceImage"), 0);
        glUniform1i (glGetUniformLocation (m_program, "opacityMask"), 1);
        glUniform1i (glGetUniformLocation (m_program, "hasOpacityMask"), mask ? 1 : 0);
        glUniform2f (glGetUniformLocation (m_program, "sourceTexel"),
            1.0f / static_cast<float> (textureWidth),
            1.0f / static_cast<float> (textureHeight));
        glUniform2f (glGetUniformLocation (m_program, "sourceExtent"),
            static_cast<float> (sourceWidth) / static_cast<float> (textureWidth),
            static_cast<float> (sourceHeight) / static_cast<float> (textureHeight));
        glBindTexture (GL_TEXTURE_2D, source.getTextureID (0));
        glBindSampler (0, 0);
        if (mask) {
            glActiveTexture (GL_TEXTURE1);
            glBindTexture (GL_TEXTURE_2D, mask->getTextureID (0));
            glBindSampler (1, 0);
            glActiveTexture (GL_TEXTURE0);
        }
        glBindVertexArray (m_vertexArray);
        glDrawArrays (GL_TRIANGLES, 0, 3);
        glReadBuffer (GL_COLOR_ATTACHMENT0);
        glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glPixelStorei (GL_PACK_ROW_LENGTH, 0);
        glPixelStorei (GL_PACK_SKIP_ROWS, 0);
        glPixelStorei (GL_PACK_SKIP_PIXELS, 0);
        glReadPixels (0, 0, static_cast<GLsizei> (size.x), static_cast<GLsizei> (size.y),
            GL_RGBA, GL_UNSIGNED_BYTE, rgba.data ());
    }
    glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (oldDraw));
    glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (oldRead));
    glDeleteFramebuffers (1, &framebuffer);
    glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (oldTexture));
    glBindSampler (0, static_cast<GLuint> (oldSampler));
    glActiveTexture (GL_TEXTURE1);
    glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (oldMaskTexture));
    glBindSampler (1, static_cast<GLuint> (oldMaskSampler));
    glActiveTexture (GL_TEXTURE0);
    glDeleteTextures (1, &texture);
    glUseProgram (static_cast<GLuint> (oldProgram));
    glBindVertexArray (static_cast<GLuint> (oldVao));
    glActiveTexture (static_cast<GLenum> (oldActiveTexture));
    glPixelStorei (GL_PACK_ALIGNMENT, oldPack);
    glPixelStorei (GL_PACK_ROW_LENGTH, oldPackRow);
    glPixelStorei (GL_PACK_SKIP_ROWS, oldPackSkipRows);
    glPixelStorei (GL_PACK_SKIP_PIXELS, oldPackSkipPixels);
    glBindBuffer (GL_PIXEL_PACK_BUFFER, static_cast<GLuint> (oldPackBuffer));
    glBindBuffer (GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint> (oldUnpackBuffer));
    glViewport (oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    glColorMask (oldColorMask[0], oldColorMask[1], oldColorMask[2], oldColorMask[3]);
    if (oldDepth) glEnable (GL_DEPTH_TEST); else glDisable (GL_DEPTH_TEST);
    if (oldBlend) glEnable (GL_BLEND); else glDisable (GL_BLEND);
    if (oldCull) glEnable (GL_CULL_FACE); else glDisable (GL_CULL_FACE);
    if (oldScissor) glEnable (GL_SCISSOR_TEST); else glDisable (GL_SCISSOR_TEST);
    if (complete) output = ParticleCore::imageEmitterSamples (
        rgba, size.x, size.y, sourceWidth, sourceHeight);
    return complete;
}

namespace {
struct ParticlePresentationFrontFace {
    GLint previous = GL_CCW;
    bool reversed;
    explicit ParticlePresentationFrontFace (bool reverse) : reversed (reverse) {
        if (!reversed) return;
        glGetIntegerv (GL_FRONT_FACE, &previous);
        glFrontFace (previous == GL_CCW ? GL_CW : GL_CCW);
    }
    ParticlePresentationFrontFace (const ParticlePresentationFrontFace&) = delete;
    ParticlePresentationFrontFace& operator= (const ParticlePresentationFrontFace&) = delete;
    ~ParticlePresentationFrontFace () {
        if (reversed) glFrontFace (static_cast<GLenum> (previous));
    }
};

const Material& particleMaterialOrEmpty (const Particle& particle) {
    static const Material empty;
    return particle.material && particle.material->material
        ? *particle.material->material : empty;
}

bool traceParticleChildren (const CParticle& particle) {
    return particle.getScene ().getContext ().getApp ().getContext ().settings.render.debug.passLog;
}

int nextParticleChildId (const Wallpapers::CScene& scene) {
    // Script property keys contain object IDs. Never recycle an ID when a
    // short-lived child is removed, and share the sequence across parents.
    static std::atomic<int64_t> next { -1 };
    for (;;) {
        const int64_t candidate = next.fetch_sub (1, std::memory_order_relaxed);
        if (candidate < std::numeric_limits<int>::min ())
            throw std::overflow_error ("Particle child object IDs exhausted");
        if (!scene.getObject (static_cast<int> (candidate)))
            return static_cast<int> (candidate);
    }
}

float sampleParticleAudio (const CParticle& particle,
                           const WallpaperEngine::Audio::ParticleAudioSettings& settings) {
    const auto& spectrum = particle.getScene ().getAudioSpectrum ();
    return WallpaperEngine::Audio::particleAudioResponse (
	    spectrum.audio16[0], spectrum.audio16[1], settings);
}

WallpaperEngine::Audio::ParticleAudioSettings audioSettings (const ParticleEmitter& emitter) {
    return {
        .mode = emitter.audioProcessingMode,
        .lowerBound = emitter.audioProcessingBounds.x,
        .upperBound = emitter.audioProcessingBounds.y,
        .exponent = emitter.audioProcessingExponent,
        .firstBand = emitter.audioProcessingFrequencyStart,
        .lastBand = emitter.audioProcessingFrequencyEnd,
    };
}

WallpaperEngine::Audio::ParticleAudioSettings audioSettings (
    const TurbulentVelocityRandomInitializer& initializer) {
    const glm::vec2 bounds = initializer.audioProcessingBounds->value->getVec2 ();
    return {
        .mode = initializer.audioProcessingMode->value->getInt (),
        .lowerBound = bounds.x,
        .upperBound = bounds.y,
        .exponent = initializer.audioProcessingExponent->value->getFloat (),
        .firstBand = initializer.audioProcessingFrequencyStart->value->getInt (),
        .lastBand = initializer.audioProcessingFrequencyEnd->value->getInt (),
    };
}

WallpaperEngine::Audio::ParticleAudioSettings audioSettings (const VortexOperator& vortex) {
    const glm::vec2 bounds = vortex.audioProcessingBounds->value->getVec2 ();
    return {
        .mode = vortex.audioProcessingMode->value->getInt (),
        .lowerBound = bounds.x,
        .upperBound = bounds.y,
        .exponent = vortex.audioProcessingExponent->value->getFloat (),
        .firstBand = vortex.audioProcessingFrequencyStart->value->getInt (),
        .lastBand = vortex.audioProcessingFrequencyEnd->value->getInt (),
    };
}

WallpaperEngine::Audio::ParticleAudioSettings audioSettings (const TurbulenceOperator& turbulence) {
    const glm::vec2 bounds = turbulence.audioProcessingBounds->value->getVec2 ();
    return {
        .mode = turbulence.audioProcessingMode->value->getInt (),
        .lowerBound = bounds.x,
        .upperBound = bounds.y,
        .exponent = turbulence.audioProcessingExponent->value->getFloat (),
        .firstBand = turbulence.audioProcessingFrequencyStart->value->getInt (),
        .lastBand = turbulence.audioProcessingFrequencyEnd->value->getInt (),
    };
}

std::optional<ParticleCore::BlendEnvelope> operatorEnvelope (
    const ParticleOperatorBase::BlendEnvelope* blend) {
    if (!blend) return std::nullopt;
    return ParticleCore::BlendEnvelope {
        blend->inStart->value->getFloat (), blend->inEnd->value->getFloat (),
        blend->outStart->value->getFloat (), blend->outEnd->value->getFloat ()
    };
}
} // namespace

CParticle::CParticle (Wallpapers::CScene& scene, const Particle& particle, uint32_t childDepth,
                      std::vector<std::string> ancestry, CParticle* parentRuntime) :
    CObject (scene, particle), CRenderable (scene, particle, particleMaterialOrEmpty (particle)),
    ScriptableObject (scene, particle), m_particle (particle), m_childDepth (childDepth),
    m_childAncestry (std::move (ancestry)), m_parentParticleRuntime (parentRuntime),
    m_rng (scene.getParticleRandom ()) {
    if (!m_parentParticleRuntime)
        m_instancePatchWrites = std::make_unique<ParticleCore::InstancePatchWrites> (
            particle.instanceOverride);
    m_instanceControlPointOverrides.fill (glm::vec3 (std::numeric_limits<float>::max (), 0.0f, 0.0f));
    m_instanceControlPointAngleOverrides.fill (glm::vec3 (std::numeric_limits<float>::max (), 0.0f, 0.0f));
    for (size_t index = 0; index < particle.instanceOverride.controlPoints.size (); ++index) {
        const auto& authored = particle.instanceOverride.controlPoints[index];
        if (authored && authored->value)
            m_instanceControlPointOverrides[index] = authored->value->getVec3 ();
        const auto& authoredAngle = particle.instanceOverride.controlPointAngles[index];
        if (authoredAngle && authoredAngle->value)
            m_instanceControlPointAngleOverrides[index] = authoredAngle->value->getVec3 ();
    }
    if (!particle.particleFile.empty ())
	m_childAncestry.push_back (std::filesystem::path (particle.particleFile).lexically_normal ().generic_string ());
    for (const auto& binding : Scripting::scriptPropertyBindings (particle))
	this->registerProperty (binding.name, binding.value);
    Scripting::forEachParticleScriptSetting (particle,
        [this] (const std::string& name, DynamicValue& value) {
            getScene ().getScriptEngine ().queueScript (
                "particle" + std::to_string (getId ()) + "_" + name, value, *this);
        });

    if (!m_material.passes.empty ()) this->detectTexture ();
    // Read renderer configuration early to determine rendering mode
    m_hasOrdinaryRopeRenderer = std::any_of (
        m_particle.renderers.begin (), m_particle.renderers.end (),
        [] (const auto& renderer) { return renderer.name == "rope"; });
    if (!m_particle.renderers.empty ()) {
	if (std::all_of (m_particle.renderers.begin (), m_particle.renderers.end (),
	                 [] (const auto& renderer) {
	                     return (renderer.name == "sprite" || renderer.name == "spritetrail")
	                         && (renderer.orientation == "screen"
	                             || renderer.orientation == "fixed"
	                             || renderer.orientation == "upright");
	                 })) {
	    // 1402366f0 walks the compiled renderer stream and invokes type-1
	    // sprite draw for every record, including repeated sprite records.
	    m_spriteRendererCount = static_cast<uint32_t> (m_particle.renderers.size ());
	} else if (m_particle.renderers.size () >= 2
	           && std::any_of (m_particle.renderers.begin (), m_particle.renderers.end (),
	               [] (const auto& renderer) {
	                   return renderer.name == "rope" || renderer.name == "ropetrail";
	               })
	           && std::all_of (m_particle.renderers.begin (), m_particle.renderers.end (),
	               [] (const auto& renderer) {
	                   return (renderer.name == "rope" || renderer.name == "ropetrail"
	                           || renderer.name == "sprite" || renderer.name == "spritetrail")
	                       && (renderer.orientation == "screen"
	                           || renderer.orientation == "fixed"
	                           || renderer.orientation == "upright");
	               })) {
	    m_mixedSpriteRopeRenderer = true;
	    // Ordinary-rope history is independent of sprite-trail shader state.
	    m_useTrailRenderer = false;
	    m_ropeRendererIndex = static_cast<size_t> (std::find_if (
	        m_particle.renderers.begin (), m_particle.renderers.end (),
	        [] (const auto& renderer) {
	            return renderer.name == "rope" || renderer.name == "ropetrail";
	        })
	        - m_particle.renderers.begin ());
	    m_useRopeRenderer = true;
	    const auto& rope = m_particle.renderers[m_ropeRendererIndex];
	    m_ropeUVScale = rope.uvScale;
	    m_ropeUVScrolling = rope.uvScrolling;
	    m_ropeUVSmoothing = rope.uvSmoothing;
	} else if (m_particle.renderers.size () > 1) {
	    sLog.error ("Particle renderer records need unsupported mode or mixed draw state: ",
	                m_particle.name, " count=", m_particle.renderers.size ());
	}
	const auto& renderer = m_particle.renderers[0];
	if (renderer.name == "rope" || renderer.name == "ropetrail") {
	    // Both rope and ropetrail use genericropeparticle shader
	    m_useRopeRenderer = true;
	    m_ropeUVScale = renderer.uvScale;
	    m_ropeUVScrolling = renderer.uvScrolling;
	    m_ropeUVSmoothing = renderer.uvSmoothing;

	    if (renderer.name == "ropetrail") {
		m_useTrailRenderer = true;
		m_trailLength = renderer.length;
		if (!std::isfinite (renderer.segments) || renderer.segments < 0.0f
		    || renderer.segments >= static_cast<float> (std::numeric_limits<int>::max ())) {
		    throw std::invalid_argument ("Invalid particle rope segment count");
		}
		m_ropeSegments = std::max (2, static_cast<int> (renderer.segments));
		m_ropeSegments = std::min (32, m_ropeSegments);
		m_ropeTrailFadeAlpha = renderer.fadeAlpha;
		m_ropeTrailFadeSize = renderer.fadeSize;
	    }
	} else if (renderer.name == "spritetrail") {
	    // spritetrail uses genericparticle with TRAILRENDERER combo
	    m_useTrailRenderer = !m_mixedSpriteRopeRenderer;
	    m_trailLength = renderer.length;
	    m_trailMaxLength = renderer.maxLength;
	    m_trailMinLength = renderer.minLength;
	}
	for (size_t rendererIndex = 0; rendererIndex < m_particle.renderers.size (); ++rendererIndex) {
	    const auto& trail = m_particle.renderers[rendererIndex];
	    if (trail.name != "ropetrail") continue;
	    // Native 1401d2340:693–695 overwrites the node's single segment count
	    // and interval for each rope-trail record. Its history is allocated only
	    // after the stream loop at 734–744, so the last record sets the shared
	    // sampling configuration while each record keeps its own draw options.
	    m_hasRopeTrailHistory = true;
	    m_ropeTrailRendererIndex = rendererIndex;
	    m_trailLength = trail.length;
	    if (!std::isfinite (trail.segments) || trail.segments < 0.0f
	        || trail.segments >= static_cast<float> (std::numeric_limits<int>::max ()))
	        throw std::invalid_argument ("Invalid particle rope segment count");
	    m_ropeSegments = std::clamp (static_cast<int> (trail.segments), 2, 32);
	    m_ropeTrailFadeAlpha = trail.fadeAlpha;
	    m_ropeTrailFadeSize = trail.fadeSize;
	    if (m_mixedSpriteRopeRenderer) m_useTrailRenderer = false;
	}
	// The compiled particle node has one reciprocal UV-scale field. Each rope
	// family record with nonzero scale overwrites it, while scrolling and
	// smoothing set family-specific node flag bits that remain set. See
	// 1401c5490:9838–9880 and 9979–10000.
	for (const auto& record : m_particle.renderers) {
	    if (record.name != "rope" && record.name != "ropetrail") continue;
	    if (record.uvScale != 0.0f) m_ropeUVScale = record.uvScale;
	    if (record.name == "rope") {
	        m_ordinaryRopeUVScrolling |= record.uvScrolling;
	        m_ordinaryRopeUVSmoothing |= !record.uvScrolling && record.uvSmoothing;
	    } else {
	        m_ropeTrailUVScrolling |= record.uvScrolling;
	    }
	}
	m_ropeUVScrolling = m_useTrailRenderer
	    ? m_ropeTrailUVScrolling : m_ordinaryRopeUVScrolling;
	m_ropeUVSmoothing = m_ordinaryRopeUVSmoothing;
    }

    // Native allocates the authored pool. Instance count scales production
    // in each emitter schedule, independently of this storage limit.
    m_maxParticles = ParticleCore::particleCapacity (particle.maxCount);

    m_particles.resize (m_maxParticles);
    if (ParticleCore::needsNativeSlotStreams (particle)) {
        if (!ParticleCore::nativeRuntimeArithmeticAvailable)
            throw std::invalid_argument ("Runtime control-point output requires native SSE arithmetic support");
        if (const auto error = ParticleCore::nativeSlotScopeError (particle, m_parentParticleRuntime != nullptr))
            throw std::invalid_argument (*error);
        ParticleInstance zero;
        zero.color = zero.initial.color = glm::vec3 (0.0f);
        zero.alpha = zero.initial.alpha = zero.size = zero.initial.size = 0.0f;
        zero.lifetime = zero.initial.lifetime = 0.0f;
        m_slotStreams.emplace (m_maxParticles, zero);
    }
    if (m_hasRopeTrailHistory) {
	m_ropeTrailHistory = ParticleCore::RopeTrailHistory (m_maxParticles, m_ropeSegments);
	// Native 1401d2340 stores length / segments as the sampling interval.
	m_ropeTrailInterval = std::max (0.001f, m_trailLength)
	    / static_cast<float> (m_ropeSegments);
    }

    // Calculate buffer sizes based on renderer type
    if (m_useRopeRenderer) {
	// Per-record ordinary ropes can request distinct subdivisions. Reserve the
	// largest stream, then select each record's subdivision before its draw.
	m_ropeRendererSubdivisions.resize (m_particle.renderers.size (), 1);
	size_t ropeFloatCount = 0;
	size_t ropeIndexCount = 0;
	for (size_t renderer = 0; renderer < m_particle.renderers.size (); ++renderer) {
	    const auto& record = m_particle.renderers[renderer];
	    if (record.name != "rope" && record.name != "ropetrail") continue;
	    const auto geometry = record.name == "ropetrail"
	        ? ParticleCore::ropeTrailGeometry (m_maxParticles, m_ropeSegments,
	            record.subdivision, ROPE_FLOATS_PER_VERTEX)
	        : ParticleCore::ropeOrdinaryGeometry (m_maxParticles, record.subdivision,
	            ROPE_FLOATS_PER_VERTEX);
	    if (!geometry) throw std::invalid_argument ("Particle rope geometry exceeds Linux allocation budget");
	    m_ropeRendererSubdivisions[renderer] = geometry->subdivision;
	    ropeFloatCount = std::max (ropeFloatCount, geometry->floatCount);
	    ropeIndexCount = std::max (ropeIndexCount, geometry->indexCount);
	}
	m_ropeSubdivision = m_ropeRendererSubdivisions[m_ropeRendererIndex];
	m_vertices.resize (ropeFloatCount);
	m_indices.resize (ropeIndexCount);
	if (m_mixedSpriteRopeRenderer) {
	    m_vertices.resize (std::max (m_vertices.size (),
	        static_cast<size_t> (m_maxParticles) * 4 * SPRITE_FLOATS_PER_VERTEX));
	    m_indices.resize (std::max (m_indices.size (),
	        static_cast<size_t> (m_maxParticles) * 6));
	}
    } else {
	// Trail particles: (N+1) * 2 vertices for ribbon strip, N * 6 indices for N quads
	// Normal particles: 4 vertices, 6 indices
	const int verticesPerParticle = 4;
	const int indicesPerParticle = 6;

	m_vertices.resize (m_maxParticles * verticesPerParticle * SPRITE_FLOATS_PER_VERTEX);
	m_indices.resize (m_maxParticles * indicesPerParticle);
    }
}

CParticle::~CParticle () {
    // Event child runtimes can retire long before scene shutdown. Run their
    // module destroy hooks and release their module state while thisLayer is
    // still valid; the base destructor's unregister is then idempotent.
    getScene ().getScriptEngine ().destroyObjectModules (*this);
    m_passes.clear ();
    if (!m_vaos.empty ()) glDeleteVertexArrays (static_cast<GLsizei> (m_vaos.size ()), m_vaos.data ());
    if (m_vbo != 0) {
	glDeleteBuffers (1, &m_vbo);
    }
    if (m_ebo != 0) {
	glDeleteBuffers (1, &m_ebo);
    }

    m_vertices.clear ();
    m_indices.clear ();
}

const CParticle* CParticle::instanceOverrideOwner () const {
    // Both static and event children are constructed with their parent's
    // instance context (14022ebe0 / 140236cd0 -> 1402293a0).
    const CParticle* owner = this;
    while (owner->m_parentParticleRuntime) owner = owner->m_parentParticleRuntime;
    return owner;
}

DynamicValue* CParticle::sharedInstanceOverrideValue (
    UserSettingUniquePtr ParticleInstanceOverride::* field) const {
    return (instanceOverrideOwner ()->m_particle.instanceOverride.*field)->value.get ();
}

DynamicValue* CParticle::lifetimeOverrideValue () const {
    return sharedInstanceOverrideValue (&ParticleInstanceOverride::lifetime);
}

DynamicValue* CParticle::sizeOverrideValue () const {
    return sharedInstanceOverrideValue (&ParticleInstanceOverride::size);
}

DynamicValue* CParticle::countOverrideValue () const {
    return sharedInstanceOverrideValue (&ParticleInstanceOverride::count);
}

DynamicValue* CParticle::alphaOverrideValue () const {
    return sharedInstanceOverrideValue (&ParticleInstanceOverride::alpha);
}

DynamicValue* CParticle::speedOverrideValue () const {
    return sharedInstanceOverrideValue (&ParticleInstanceOverride::speed);
}

glm::vec3 CParticle::instanceBirthRgbGain () const {
    const CParticle* root = instanceOverrideOwner ();
    return ParticleCore::particleInstanceBirthRgbGain (
        sharedInstanceOverrideValue (&ParticleInstanceOverride::colorn)->getVec3 (),
        root->m_particle.presetColorN, m_particle.presetColorN,
        sharedInstanceOverrideValue (&ParticleInstanceOverride::brightness)->getFloat (),
        root->m_particle.flags, m_particle.flags, m_particle.presetTintCompiled,
        root == this, getScene ().isHdrPostprocessingActive ());
}

glm::vec3 CParticle::instanceTintedColorEndpoint (const glm::vec3& authored) const {
    const CParticle* root = instanceOverrideOwner ();
    const glm::vec3 tint = sharedInstanceOverrideValue (
        &ParticleInstanceOverride::colorn)->getVec3 ();
    return ParticleCore::particleInstanceShiftColorEndpoint (authored, tint,
        m_particle.presetColorN, ParticleCore::particleInstanceTintValid (
            tint, root->m_particle.presetColorN, m_particle.presetColorN,
            root->m_particle.flags, m_particle.flags, root == this));
}

void CParticle::setup () {
    if (m_initialized) {
	return;
    }

    // Convert origin from screen space to centered space
    // Projection uses ortho(-width/2, width/2, -height/2, height/2)
    // but particle origins are in screen space where (0,0) is top-left
    m_lastScreenWidth = getScene ().getCamera ().getWidth ();
    m_lastScreenHeight = getScene ().getCamera ().getHeight ();


    // Load particle material constants
    if (m_particle.material && m_particle.material->material && !m_particle.material->material->passes.empty ()) {
	auto& firstPass = *m_particle.material->material->passes.begin ();

	// Read overbright constant (brightness multiplier for additive particles)
	auto overbrightIt = firstPass->constants.find ("ui_editor_properties_overbright");
	if (overbrightIt != firstPass->constants.end ()) {
	    m_overbright = overbrightIt->second->value->getFloat ();
	}
    }

    // Texture is resolved by CRenderable base class; read spritesheet data.
    // TextureParser computes spritesheet grid from TEXS frame data (animated textures)
    // or .tex-json metadata (static textures). For GIF-style animated textures (separate
    // GL texture per frame), the parser returns 0 cols/rows since a 1x1 grid can't hold
    // all frames — so no SPRITESHEET mode is needed (frame switching happens via texture ID).
    if (const auto texture = getTexture ()) {
	m_spritesheetCols = static_cast<int> (texture->getSpritesheetCols ());
	m_spritesheetRows = static_cast<int> (texture->getSpritesheetRows ());
	m_spritesheetFrames = static_cast<int> (texture->getSpritesheetFrames ());
	m_spritesheetDuration = texture->getSpritesheetDuration ();
	const auto& frames = texture->getFrames ();
        if (m_slotStreams && ((texture->getFlags () & TextureFlags_IsGif) != 0
            || texture->isAnimated () || m_spritesheetFrames > 1 || frames.size () > 1))
            throw std::invalid_argument (*ParticleCore::nativeSlotScopeError (m_particle, false, true));
	m_animationFrameCount = static_cast<int> (frames.size ());
	m_separatePageAnimation = frames.size () > 1
	    && std::any_of (frames.begin () + 1, frames.end (), [&] (const auto& frame) {
	        return frame && frames.front () && frame->frameNumber != frames.front ()->frameNumber;
	    });
	m_authoredFrameTimeline = m_animationFrameCount > 0
	    && std::all_of (frames.begin (), frames.end (), [&] (const auto& frame) {
		return frame && std::isfinite (frame->frametime) && frame->frametime > 0.0f;
	    });
	if (m_authoredFrameTimeline) {
	    m_spritesheetDuration = std::accumulate (frames.begin (), frames.end (), 0.0f,
	        [] (float sum, const auto& frame) { return sum + frame->frametime; });
	}
	if (m_animationFrameCount > 1 && m_spritesheetFrames == 0 && !m_separatePageAnimation)
	    sLog.error ("Particle animated texture has irregular same-page atlas without grid mapping: ",
	                m_particle.name);
    }

    setupEmitters ();
    setupInitializers ();
    setupOperators ();
    setupPass ();

    // Setup control points (max 8)
    updateMatrices ();
    m_controlPoints.resize (8);
    for (const auto& cp : m_particle.controlPoints) {
	if (cp.id >= 0 && cp.id < 8) {
	    m_controlPoints[cp.id].offset = cp.offset;
	    m_controlPoints[cp.id].angles = cp.angles;
	    m_controlPoints[cp.id].flags = cp.flags;
	    // Link to mouse if either flags bit 0 is set
	    m_controlPoints[cp.id].linkMouse = (cp.flags & 1) != 0;
	    m_controlPoints[cp.id].worldSpace = (cp.flags & 2) != 0;
            // 14022c3c0 creates generated CPs with identity basis and the
            // authored offset. Ownership suppresses later matrix writers.
            if ((cp.flags & 0x10000u) != 0) {
                m_controlPoints[cp.id].basis = glm::mat3 (1.0f);
                m_controlPoints[cp.id].position = ParticleCore::localControlPointPosition (cp.offset);
                continue;
            }
	    if (!m_controlPoints[cp.id].worldSpace)
		m_controlPoints[cp.id].basis = ParticleCore::localControlPointBasis (cp.angles);
	    if (!m_controlPoints[cp.id].linkMouse && m_controlPoints[cp.id].worldSpace
		&& m_controlPointTransformInvertible)
		m_controlPoints[cp.id].basis = glm::mat3 (m_controlPointInverse);

	    // Initialize position to offset for non-mouse-linked control points
	    // Mouse-linked CPs will have their position updated in update()
	    if (!m_controlPoints[cp.id].linkMouse) {
		if (m_controlPoints[cp.id].worldSpace) {
		    // World space: offset is in screen-centered coords, convert to particle local space
		    m_controlPoints[cp.id].position = Wallpapers::projectWorldControlPoint (
		        m_controlPointInverse, m_controlPointTransformInvertible,
		        cp.offset, m_controlPoints[cp.id].position);
		} else {
		    // Native stores the authored local offset directly in its CP matrix;
		    // Linux particle positions and emitter origins use reflected Y.
		    m_controlPoints[cp.id].position = ParticleCore::localControlPointPosition (cp.offset);
		}
	    }
	}
    }

    updateOrdinaryControlPoints ();

    for (auto& cp : m_controlPoints) cp.previousPosition = cp.position;

    // Initial/new nodes are patched before pending births and warm-up.
    if (m_instancePatchWrites) m_instancePatchWrites->consume ();
    patchInstanceSequenceSteps ();
    m_initialized = true;
    if (m_pendingEmitCount != 0) {
        const uint32_t pending = std::exchange (m_pendingEmitCount, 0u);
        emitParticles (static_cast<int32_t> (std::min<uint32_t> (
            pending, static_cast<uint32_t> (std::numeric_limits<int32_t>::max ()))));
    }
    if (m_childDepth < 64) {
	for (size_t i = 0; i < m_particle.children.size (); ++i)
	    if (m_particle.children[i].type == "static") spawnChild (i, nullptr, false);
    } else if (!m_particle.children.empty ()) {
	sLog.error ("Particle child nesting exceeds Linux safety depth 64 for object ", m_particle.id);
    }

    warmup ();
}

void CParticle::warmup () {
    const auto plan = ParticleCore::warmupPlan (m_particle.startTime, m_maxParticles);
    if (plan.truncated) {
	sLog.error ("Particle warm-up limited to ", ParticleCore::MAX_WARMUP_STEPS,
	            " steps for object ", m_particle.id);
    }
    const int configuredFps = getScene ().getContext ().getApp ().getContext ().settings.render.maximumFPS;
    const uint32_t fps = configuredFps > 0 ? static_cast<uint32_t> (configuredFps) : 0;
    // Native warm-up calls the inner tick directly, bypassing the outer
    // node-time accumulator; keep m_time at startup until ordinary render.
    for (uint32_t i = 0; i < plan.steps; ++i) {
	update (ParticleCore::warmupClock (plan.step, fps));
    }
    if (m_hasRopeTrailHistory) {
	for (uint32_t i = 0; i < m_particleCount; ++i)
	    m_ropeTrailHistory.born (i, m_particles[i].position);
	m_ropeTrailCountdown = 0.0f;
    }
}

void CParticle::render () {
    // Native outer-root flush precedes visibility, enabled and clock guards.
    // Inner warm-up/update and explicit emitParticles do not flush writes.
    if (m_initialized && m_instancePatchWrites && m_instancePatchWrites->consume ())
        patchInstanceSequenceSteps ();
    if (!m_initialized || !resolveTransform ().visible) {
	return;
    }

    // The application supplies the first frame's scene delta too. Skipping
    // it would leave warmed-up particles frozen for one live frame.
    float sceneDt = getScene ().getDeltaTime ();
    bool paused = false;
    for (const CParticle* ancestor = this; ancestor; ancestor = ancestor->m_parentParticleRuntime)
        paused |= ancestor->m_paused;
    if (const auto fixedStep = getScene ().getContext ().getApp ().getContext ()
                                   .settings.render.debug.particleStep) {
        sceneDt = *fixedStep;
        if (!paused && !m_preTickedByParent) m_time += *fixedStep;
    } else if (!paused) {
        m_time = g_Time;
    }

    if (m_preTickedByParent) {
	m_preTickedByParent = false;
    } else if (!paused && std::isfinite (sceneDt) && sceneDt > 0.0f) {
	// Linux's render clock has no native host slowdown factor. Bound long
	// stalls to the native main-loop ceiling before deriving the node clock.
	sceneDt = std::min (sceneDt, 0.25f);
	const int configuredFps = getScene ().getContext ().getApp ().getContext ().settings.render.maximumFPS;
	// Native 140230650 scales the node clock by instanceoverride.rate
	// (+0x854) after clamping that authored value to at least 0.01.
	const float authoredRate = m_particle.instanceOverride.rate->value->getFloat ();
	const float rate = ParticleCore::nodeRate (authoredRate);
	update (ParticleCore::tickClock (
	    sceneDt, sceneDt * rate, configuredFps > 0 ? static_cast<uint32_t> (configuredFps) : 0));
    }

    // Render particles
    if (m_particleCount > 0 && m_particle.material) {
	if (m_mixedSpriteRopeRenderer) {
	    for (size_t renderer = 0; renderer < m_particle.renderers.size (); ++renderer) {
	        m_activeRendererIndex = renderer;
	        const auto& record = m_particle.renderers[renderer];
	        if (record.name == "rope" || record.name == "ropetrail") {
	            m_useTrailRenderer = record.name == "ropetrail";
	            m_ropeSubdivision = m_ropeRendererSubdivisions[renderer];
	            m_ropeUVScrolling = m_useTrailRenderer
	                ? m_ropeTrailUVScrolling : m_ordinaryRopeUVScrolling;
	            m_ropeUVSmoothing = m_ordinaryRopeUVSmoothing;
	            m_ropeTrailFadeAlpha = record.fadeAlpha;
	            m_ropeTrailFadeSize = record.fadeSize;
	            if (m_useTrailRenderer) renderRopeTrail ();
	            else renderRope ();
	        } else {
	            m_useTrailRenderer = false;
	            renderSprites (static_cast<uint32_t> (renderer));
	        }
	    }
	    m_useTrailRenderer = false;
	} else if (m_useRopeRenderer) {
	    m_activeRendererIndex = 0;
	    if (m_useTrailRenderer) renderRopeTrail ();
	    else renderRope ();
	} else {
	    for (uint32_t renderer = 0; renderer < m_spriteRendererCount; ++renderer) {
	        m_activeRendererIndex = renderer;
		renderSprites (renderer);
	    }
	}
    }
    renderChildren ();
}

void CParticle::emitNewParticles (float dt) {
	std::vector<ParticleInstance> bornParticles;
	bool allocatedSlots = false;
	if (m_slotStreams) {
	    m_slotStreams->beginEmissionPass ();
	}
	if (m_emissionEnabled || m_forcedEmitCount != 0) for (auto& emitter : m_emitters) {
	    const uint32_t oldCount = m_particleCount;
	    uint32_t emissionCount = m_slotStreams ? m_slotStreams->nativeCount () : m_particleCount;
	    emitter (m_particles, emissionCount, dt);
	    if (m_slotStreams) continue;
	    m_particleCount = emissionCount;
	    for (uint32_t i = oldCount; i < m_particleCount; ++i) {
		const auto slot = m_nativeSlots.allocate (m_maxParticles);
		if (!slot) throw std::logic_error ("Particle native slot allocation exceeded pool capacity");
		m_particles[i].poolSlot = *slot;
		allocatedSlots = true;
		m_particles[i].birthId = m_nextParticleBirthId++;
		if (m_nextParticleBirthId == 0) m_nextParticleBirthId = 1;
		if (m_hasOrdinaryRopeRenderer) m_ropeBirthSlots.push_back (*slot);
		if (m_hasRopeTrailHistory)
		    m_ropeTrailHistory.born (i, m_particles[i].position);
		if (!m_particle.children.empty ()) bornParticles.push_back (m_particles[i]);
	    }
	}
	if (m_slotStreams) refreshNativeLiveView ();
	// Native operator, render and CP loops scan live SoA slots from zero to
	// high-water. A reused low slot must precede older high-slot survivors.
	if (allocatedSlots && !std::is_sorted (
	        m_particles.begin (), m_particles.begin () + m_particleCount,
	        [] (const ParticleInstance& a, const ParticleInstance& b) {
	            return a.poolSlot < b.poolSlot;
	        })) {
	    std::vector<uint32_t> slotOrder (m_particleCount);
	    std::iota (slotOrder.begin (), slotOrder.end (), 0u);
	    std::sort (slotOrder.begin (), slotOrder.end (), [this] (uint32_t a, uint32_t b) {
	        return m_particles[a].poolSlot < m_particles[b].poolSlot;
	    });
	    std::vector<ParticleInstance> ordered;
	    ordered.reserve (m_particleCount);
	    for (uint32_t source : slotOrder) ordered.push_back (m_particles[source]);
	    if (m_hasRopeTrailHistory) m_ropeTrailHistory.reorder (slotOrder);
	    std::copy (ordered.begin (), ordered.end (), m_particles.begin ());
	}
	// Native 1402378a0 gathers births across the full emitter record pass,
	// then walks child descriptors over that combined ID batch.
	if (!bornParticles.empty ()) {
	    processChildEvents ({}, bornParticles);
	    if (traceParticleChildren (*this))
		for (const auto& p : bornParticles)
		    sLog.out ("CHILD_TRACE birth object=", m_particle.id, " birth=", p.birthId,
		              " slot=", p.poolSlot, " px=", p.position.x,
		              " py=", p.position.y, " pz=", p.position.z,
		              " vx=", p.velocity.x, " vy=", p.velocity.y,
		              " vz=", p.velocity.z, " birthseed=", p.oscillatorRandom);
	}
}

uint32_t CParticle::emissionCapacity (const std::vector<ParticleInstance>& particles, uint32_t count) const {
    return m_slotStreams ? m_slotStreams->capacity () - m_slotStreams->nativeCount ()
                         : static_cast<uint32_t> (particles.size () - count);
}

ParticleInstance& CParticle::emittedParticleTarget (std::vector<ParticleInstance>& particles, uint32_t count) {
    if (!m_slotStreams) return particles[count];
    const auto slot = m_slotStreams->nextBirthSlot ();
    if (!slot) throw std::logic_error ("Native particle birth exceeded physical capacity");
    auto& particle = m_slotStreams->streams ()[*slot];
    particle.poolSlot = *slot;
    particle.birthId = m_nextParticleBirthId++;
    if (m_nextParticleBirthId == 0) m_nextParticleBirthId = 1;
    return particle;
}

void CParticle::finishEmittedParticle (ParticleInstance& particle, uint32_t& count) {
    if (!m_slotStreams) {
        ++count;
        return;
    }
    if (!std::isfinite (particle.lifetime) || particle.lifetime <= 0.0f)
        throw std::invalid_argument ("Runtime control-point output requires a finite positive birth lifetime");
    // Birth remaps consume the baseline proxy during ordered initialization.
    // Native current streams remain at raw defaults until interpreter restore;
    // unseparated RGB/alpha streams alias their updated baselines instead.
    particle.size = 0.5f;
    if (m_resetColorEachPass) particle.color = instanceBirthRgbGain ();
    if (m_resetAlphaEachPass) particle.alpha = alphaOverrideValue ()->getFloat ();
    m_slotStreams->commitBirth (particle.poolSlot);
    count = m_slotStreams->nativeCount ();
}

void CParticle::refreshNativeLiveView () {
    m_particleCount = 0;
    m_slotStreams->visitDrawSlots ([this] (const ParticleInstance& source, uint32_t) {
        m_particles[m_particleCount++] = source;
    });
}

void CParticle::resetStaticEmitterTree () {
    m_time = 0.0;
    resetSequenceCounters (false);
    m_emitters.clear ();
    m_emitterCanProduce.clear ();
    setupEmitters ();
    m_emissionEnabled = true;
    for (auto& node : m_childNodes)
        if (m_particle.children[node.descriptor].type == "static")
            node.runtime->resetStaticEmitterTree ();
}

void CParticle::resetPeriodicChildren () {
    // 14022f790 also resets flagged initializer 0x0d phase counters.
    resetSequenceCounters (true);
    // It restarts only static children whose descriptor +0x64 has bit 1.
    for (auto& node : m_childNodes)
        if (m_particle.children[node.descriptor].type == "static"
            && (m_particle.children[node.descriptor].flags & 2u) != 0)
            node.runtime->resetStaticEmitterTree ();
}

void CParticle::resetSequenceCounters (bool periodicOnly) {
    for (const auto& counter : m_sequenceCounters) {
        if (periodicOnly && (counter->flags & 2u) == 0) continue;
        ParticleCore::resetSequencePhase (counter->phase, counter->step);
    }
}

void CParticle::patchInstanceSequenceSteps () {
    const float count = countOverrideValue ()->getFloat ();
    for (const auto& counter : m_sequenceCounters)
        counter->instanceCountPatch.apply (counter->step, count);
    // Native 14022bd40 patches both active and inactive event vectors.
    for (auto& node : m_childNodes) node.runtime->patchInstanceSequenceSteps ();
    m_retainedChildNodes.visit ([] (ChildNode& node) {
        node.runtime->patchInstanceSequenceSteps ();
    });
}

void CParticle::emitParticles (int32_t count) {
    if (count == 0) count = 1;
    if (count < 0) return;
    if (!m_initialized) {
        const uint64_t queued = static_cast<uint64_t> (m_pendingEmitCount)
            + static_cast<uint32_t> (count);
        m_pendingEmitCount = static_cast<uint32_t> (std::min<uint64_t> (
            queued, static_cast<uint32_t> (std::numeric_limits<int32_t>::max ())));
        return;
    }
    const uint32_t previous = m_forcedEmitCount;
    m_forcedEmitCount = static_cast<uint32_t> (count);
    const bool traceExisting = traceParticleChildren (*this) && m_particleCount != 0;
    const uint32_t existingBirth = traceExisting ? m_particles[0].birthId : 0;
    const float existingAge = traceExisting ? m_particles[0].age : 0.0f;
    const float existingSize = traceExisting ? m_particles[0].size : 0.0f;
    try {
        updateMatrices ();
        updateOrdinaryControlPoints ();
        emitNewParticles (0.0f);
    } catch (...) {
        m_forcedEmitCount = previous;
        throw;
    }
    m_forcedEmitCount = previous;
    if (traceExisting) {
        const auto end = m_particles.begin () + m_particleCount;
        const auto old = std::find_if (m_particles.begin (), end,
            [existingBirth] (const ParticleInstance& instance) {
                return instance.birthId == existingBirth;
            });
        if (old != end)
            sLog.out ("CHILD_TRACE scripted-emit object=", m_particle.id,
                      " existingbirth=", existingBirth,
                      " beforeage=", existingAge, " afterage=", old->age,
                      " beforesize=", existingSize, " aftersize=", old->size);
    }
}

void CParticle::play () {
    m_paused = false;
    m_emissionEnabled = true;
    for (auto& node : m_childNodes)
        if (m_particle.children[node.descriptor].type == "static") node.runtime->play ();
}

void CParticle::pause () { m_paused = true; }

void CParticle::stop () {
    m_emissionEnabled = false;
    m_pendingEmitCount = 0;
    m_forcedEmitCount = 0;
    m_particleCount = 0;
    m_ropeBirthSlots.clear ();
    m_ropeExpiredCount = 0;
    m_nativeSlots = ParticleCore::NativeSlotAllocation {};
    m_eventSlotValues.clear ();
    if (m_slotStreams) m_slotStreams->stop ();
    for (size_t index = 0; index < m_childNodes.size ();) {
        auto& node = m_childNodes[index];
        if (m_particle.children[node.descriptor].type == "static") {
            node.runtime->stop ();
            ++index;
        } else {
            m_childNodes.erase (m_childNodes.begin () + static_cast<std::ptrdiff_t> (index));
        }
    }
    m_time = 0.0;
    resetSequenceCounters (false);
    if (m_hasRopeTrailHistory) {
        m_ropeTrailHistory = ParticleCore::RopeTrailHistory (m_maxParticles, m_ropeSegments);
        m_ropeTrailCountdown = 0.0f;
    }
    // Native 14024c680 -> 14022f6c0 restores each emitter's authored delay,
    // duration and instant count while disabling further automatic births.
    if (m_initialized) {
        m_emitters.clear ();
        m_emitterCanProduce.clear ();
        setupEmitters ();
    }
}

bool CParticle::isPlaying () const {
    if (m_paused) return false;
    if (m_particleCount != 0) return true;
    if (std::any_of (m_childNodes.begin (), m_childNodes.end (), [] (const ChildNode& node) {
            return node.runtime->isPlaying ();
        })) return true;
    return m_emissionEnabled && std::any_of (m_emitterCanProduce.begin (),
        m_emitterCanProduce.end (), [] (uint8_t canProduce) { return canProduce != 0; });
}

void CParticle::update (ParticleCore::TickClock clock) {
    if (clock.operatorPasses == 0) return;
    // Native retains one previous CP matrix throughout both low-FPS passes.
    for (auto& cp : m_controlPoints) cp.previousPosition = cp.position;
    float screenWidth = static_cast<float> (getScene ().getWidth ());
    float screenHeight = static_cast<float> (getScene ().getHeight ());
    // Live scripted origins and moving parents change the inverse even when
    // resolution stays fixed. World points are stored in local simulation space.
    updateMatrices ();
    updateOrdinaryControlPoints ();
    m_lastScreenWidth = screenWidth;
    m_lastScreenHeight = screenHeight;

    // Update control points with mouse position
    const glm::vec2* mousePos = getScene ().getMousePositionNormalized ();
    if (mousePos) {
        const auto& camera = getScene ().getCamera ();
        const auto world = Wallpapers::particleMouseWorldPosition (
            *mousePos, Wallpapers::particleMouseProjection (camera.getProjection (), camera.isOrthogonal ()),
            camera.getLookAt ());
        if (world) {
            for (auto& cp : m_controlPoints) {
                if (cp.linkMouse && (cp.flags & 0x10000u) == 0)
                    cp.position = Wallpapers::particleMouseControlPoint (
                        *world, (m_particle.flags & 1u) != 0, m_controlPointInverse,
                        m_controlPointTransformInvertible, cp.position);
            }
        }
    }

    for (size_t i = 0; i < m_inheritedControlPointPositions.size (); ++i)
	if (m_inheritedControlPointPositions[i] && (m_controlPoints[i].flags & 0x10000u) == 0)
	    m_controlPoints[i].position = *m_inheritedControlPointPositions[i];

    // CP flag 0x4 selects a CP on this node's parent (14022e3e0:132–145).
    // The native writer relates the previous and current matrix-stack tops
    // for local/world preset combinations (14022e3e0:149–440).
    if (m_parentParticleRuntime) {
        for (const auto& authored : m_particle.controlPoints) {
            if (authored.id < 0 || authored.id >= static_cast<int> (m_controlPoints.size ())
                || (authored.flags & 4u) == 0 || (authored.flags & 0x10000u) != 0) continue;
            if (authored.parentControlPoint < 0
                || authored.parentControlPoint
                       >= static_cast<int> (m_parentParticleRuntime->m_controlPoints.size ()))
                continue;
            const auto& source = m_parentParticleRuntime->m_controlPoints[
                static_cast<size_t> (authored.parentControlPoint)];
            auto& target = m_controlPoints[static_cast<size_t> (authored.id)];
            glm::mat4 parentControlPoint (1.0f);
            for (int axis = 0; axis < 3; ++axis)
                parentControlPoint[axis] = glm::vec4 (source.basis[axis], 0.0f);
            parentControlPoint[3] = glm::vec4 (source.position, 1.0f);
            glm::mat4 mapped = parentControlPoint;
            if ((authored.flags & 8u) == 0) {
                const bool childWorld = (m_particle.flags & 1u) != 0;
                const bool parentWorld = (m_parentParticleRuntime->m_particle.flags & 1u) != 0;
                const glm::mat4& previousStack = m_parentParticleRuntime->m_simulationModelMatrix;
                if (!childWorld) {
                    const auto inverseCurrent = Wallpapers::inverseFiniteTransform (
                        m_simulationModelMatrix);
                    if (!inverseCurrent) {
                        if (!m_reportedParentCPTransformFailure) {
                            sLog.error ("Particle parent control-point transform is singular: ",
                                        m_particle.name);
                            m_reportedParentCPTransformFailure = true;
                        }
                        continue;
                    }
                    mapped = *inverseCurrent
                        * (parentWorld ? parentControlPoint
                                       : previousStack * parentControlPoint);
                } else if (parentWorld) {
                    mapped = previousStack * parentControlPoint;
                }
            }
            target.position = glm::vec3 (mapped[3]);
            target.basis = glm::mat3 (mapped);
        }
    }

    ParticleCore::dispatchTick (clock, [this] (float dt) {
	std::vector<ParticleInstance> expiredParticles;
	if (m_slotStreams) {
	    m_slotStreams->ageAndExpire (dt, [] (const ParticleInstance&, uint32_t) {});
	    refreshNativeLiveView ();
	    emitNewParticles (dt);
	    return;
	}
	// Native tick ages and expires existing particles before running emitters.
	for (uint32_t i = 0; i < m_particleCount; i++) {
	    m_particles[i].age += dt;
	}

	uint32_t writeIdx = 0;
	for (uint32_t readIdx = 0; readIdx < m_particleCount; readIdx++) {
	    if (m_particles[readIdx].isAlive ()) {
		if (writeIdx != readIdx) {
		    m_particles[writeIdx] = m_particles[readIdx];
		    if (m_hasRopeTrailHistory)
			m_ropeTrailHistory.compact (writeIdx, readIdx);
		}
		writeIdx++;
	    } else {
		const auto slot = m_particles[readIdx].poolSlot;
		if (m_hasOrdinaryRopeRenderer) {
		    const auto ordered = std::find (m_ropeBirthSlots.begin (), m_ropeBirthSlots.end (), slot);
		    if (ordered != m_ropeBirthSlots.end ()) {
		        m_ropeBirthSlots.erase (ordered);
		        ++m_ropeExpiredCount;
		    }
		}
        if (!m_childNodes.empty () || !m_particle.children.empty ()) {
            const auto slot = m_particles[readIdx].poolSlot;
            if (m_eventSlotValues.size () <= slot) m_eventSlotValues.resize (slot + 1);
            m_eventSlotValues[slot] = eventValues (m_particles[readIdx]);
            m_eventSlotValues[slot].alive = false;
        }
		m_nativeSlots.release (m_particles[readIdx].poolSlot);
		if (!m_particle.children.empty ()) expiredParticles.push_back (m_particles[readIdx]);
	    }
	}
	m_particleCount = writeIdx;
	// Native 140236cd0 dispatches death and detaches follow children
	// immediately after compaction, before the emitter and operator stages.
	if (!m_particle.children.empty ()) {
	    processChildEvents (expiredParticles, {});
	    if (traceParticleChildren (*this))
		for (const auto& p : expiredParticles)
		    sLog.out ("CHILD_TRACE expire object=", m_particle.id, " birth=", p.birthId);
	}

	emitNewParticles (dt);
    }, [this] {
	auto& streams = m_slotStreams ? m_slotStreams->streams () : m_particles;
	const uint32_t restoreCount = m_slotStreams ? m_slotStreams->highWater () : m_particleCount;
	for (uint32_t i = 0; i < restoreCount; ++i) {
	    auto& p = streams[i];
	    ParticleCore::restoreOperatorStreams (
		p.alpha, p.size, p.initial.alpha, p.initial.size, m_resetAlphaEachPass);
	    if (m_resetColorEachPass) p.color = p.initial.color;
	}

    }, [this] (ParticleCore::MovementTime movementTime) {
	// Apply operators to living particles (including alphafade).
	if (traceParticleChildren (*this) && m_particleCount > 1 && !m_particle.children.empty ())
	    sLog.out ("CHILD_TRACE operator-order object=", m_particle.id,
	              " firstbirth=", m_particles[0].birthId,
	              " firstslot=", m_particles[0].poolSlot,
	              " secondbirth=", m_particles[1].birthId,
	              " secondslot=", m_particles[1].poolSlot);
	for (auto& op : m_operators) {
	    auto& streams = m_slotStreams ? m_slotStreams->streams () : m_particles;
	    const uint32_t domain = m_slotStreams ? m_slotStreams->paddedHighWater () : m_particleCount;
	    op (streams, domain, m_controlPoints, static_cast<float> (m_time), movementTime);
	}
        if (m_slotStreams) refreshNativeLiveView ();
    });

    if (m_hasRopeTrailHistory) {
	m_ropeTrailHistory.advance (clock.nodeDuration, m_ropeTrailInterval,
	                            m_ropeTrailCountdown, m_particleCount,
	                            [this] (uint32_t i) { return m_particles[i].position; });
    }


    // Update animation frames
    for (uint32_t i = 0; i < m_particleCount; i++) {
	auto& p = m_particles[i];

	const int frameCount = m_spritesheetFrames > 0 ? m_spritesheetFrames
	    : m_separatePageAnimation ? m_animationFrameCount : 0;
	if (frameCount > 0 && m_particle.animationMode != "randomframe") {
	    const double sequenceCycles = particleSequenceCycles (
		p.age, p.lifetime, m_particle.sequenceMultiplier);

	    if (m_particle.animationMode == "once") {
		if (m_authoredFrameTimeline) {
		    const double animationTime = sequenceCycles * m_spritesheetDuration;
		    if (animationTime >= m_spritesheetDuration)
			p.frame = static_cast<float> (p.frameOrdinal = frameCount - 1);
		    else if (const auto phase = framePhaseAtTime (getTexture ()->getFrames (), animationTime)) {
			p.frameOrdinal = static_cast<uint32_t> (phase->ordinal);
			p.frame = static_cast<float> (phase->ordinal + phase->fraction);
		    }
		} else {
		    p.frame = std::min (
			static_cast<float> (sequenceCycles * frameCount), static_cast<float> (frameCount - 1)
		    );
		}
	    } else {
		if (m_authoredFrameTimeline) {
		    if (const auto phase = framePhaseAtTime (
			getTexture ()->getFrames (), sequenceCycles * m_spritesheetDuration)) {
			p.frameOrdinal = static_cast<uint32_t> (phase->ordinal);
			p.frame = static_cast<float> (phase->ordinal + phase->fraction);
		    }
		} else if (m_spritesheetDuration > 0.0f) {
		    float cyclePos = static_cast<float> (std::fmod (sequenceCycles, 1.0));
		    p.frame = std::fmod (cyclePos * frameCount, static_cast<float> (frameCount));
		} else {
		    p.frame = std::fmod (
			static_cast<float> (sequenceCycles * frameCount), static_cast<float> (frameCount)
		    );
		}
	    }
	    if (!m_authoredFrameTimeline && std::isfinite (p.frame))
		p.frameOrdinal = static_cast<uint32_t> (std::clamp (
		    p.frame, 0.0f, static_cast<float> (frameCount - 1)));
	}
    }

    // Native 1402308a0 recursively advances event/static children during the
    // parent node tick, including the parent's warm-up loop. Child rendering
    // consumes this tick instead of deriving a second clock from scene delta.
    for (size_t childIndex = 0; childIndex < m_childNodes.size ();) {
	auto& node = m_childNodes[childIndex];
	if (!node.detached && node.parentBirthId == 0)
	    node.runtime->setChildAnchor (m_simulationModelMatrix, glm::vec3 (0.0f), true);
	if (node.follow) {
	    const auto parent = std::find_if (m_particles.begin (),
	        m_particles.begin () + m_particleCount,
	        [&node] (const ParticleInstance& p) { return p.birthId == node.parentBirthId; });
	    if (parent != m_particles.begin () + m_particleCount) {
		node.runtime->setChildAnchor (m_simulationModelMatrix, parent->position, false);
	    } else {
		node.follow = false;
		node.detached = true;
		node.runtime->disableStaticEmittersRecursively ();
        node.runtime->m_eventParentSlot.reset ();
		node.runtime->setChildAnchor (m_simulationModelMatrix,
		    node.runtime->m_childParentPosition, false);
	    }
	}
	node.runtime->inheritControlPointsFromParent (*this, m_particle.children[node.descriptor]);
	node.runtime->m_time = m_time;
	node.runtime->update (clock);
	node.runtime->m_preTickedByParent = true;
	if (traceParticleChildren (*this))
	    sLog.out ("CHILD_TRACE tick object=", m_particle.id,
		      " child=", node.model->id, " descriptor=", node.descriptor,
		      " parentbirth=", node.parentBirthId,
		      " anchorx=", node.runtime->m_childParentPosition.x,
		      " anchory=", node.runtime->m_childParentPosition.y,
		      " parentstackx=", node.runtime->m_childSceneParentMatrix[3].x,
		      " parentstacky=", node.runtime->m_childSceneParentMatrix[3].y,
		      " parentbasisxx=", node.runtime->m_childSceneParentMatrix[0].x,
		      " parentbasisxy=", node.runtime->m_childSceneParentMatrix[0].y,
		      " parentbasisyx=", node.runtime->m_childSceneParentMatrix[1].x,
		      " parentbasisyy=", node.runtime->m_childSceneParentMatrix[1].y,
		      " stackx=", node.runtime->m_simulationModelMatrix[3].x,
		      " stacky=", node.runtime->m_simulationModelMatrix[3].y,
		      " stackbasisxx=", node.runtime->m_simulationModelMatrix[0].x,
		      " stackbasisxy=", node.runtime->m_simulationModelMatrix[0].y,
		      " stackbasisyx=", node.runtime->m_simulationModelMatrix[1].x,
		      " stackbasisyy=", node.runtime->m_simulationModelMatrix[1].y,
		      " cp0x=", node.runtime->m_controlPoints.empty ()
			? 0.0f : node.runtime->m_controlPoints[0].position.x,
		      " cp0y=", node.runtime->m_controlPoints.empty ()
			? 0.0f : node.runtime->m_controlPoints[0].position.y,
		      " cp1x=", node.runtime->m_controlPoints.size () <= 1
			? 0.0f : node.runtime->m_controlPoints[1].position.x,
		      " cp1y=", node.runtime->m_controlPoints.size () <= 1
			? 0.0f : node.runtime->m_controlPoints[1].position.y,
		      " cp1basisx=", node.runtime->m_controlPoints.size () <= 1
			? 0.0f : node.runtime->m_controlPoints[1].basis[0].x,
		      " cp1basisy=", node.runtime->m_controlPoints.size () <= 1
			? 0.0f : node.runtime->m_controlPoints[1].basis[0].y,
		      " cp2x=", node.runtime->m_controlPoints.size () <= 2
			? 0.0f : node.runtime->m_controlPoints[2].position.x,
		      " count=", node.runtime->m_particleCount,
		      " firstage=", node.runtime->m_particleCount
			? node.runtime->m_particles[0].age : -1.0f,
		      " firstseed=", node.runtime->m_particleCount
			? node.runtime->m_particles[0].oscillatorRandom : -1.0f,
		      " firstx=", node.runtime->m_particleCount
			? node.runtime->m_particles[0].position.x : 0.0f,
		      " firsty=", node.runtime->m_particleCount
			? node.runtime->m_particles[0].position.y : 0.0f,
		      " nodeclock=", clock.nodeDuration);
	// Native 1402308a0 checks 14022c310 on every tick, including warm-up,
	// then moves completed event nodes into that descriptor's inactive pool.
	if (node.parentBirthId != 0 && node.runtime->isFinishedForEvent ()) {
	    if (traceParticleChildren (*this))
		sLog.out ("CHILD_TRACE retire object=", m_particle.id,
		          " child=", node.model->id, " parentbirth=", node.parentBirthId);
            m_retainedChildNodes.retire (node.descriptor, std::move (node));
	    m_childNodes.erase (m_childNodes.begin () + static_cast<std::ptrdiff_t> (childIndex));
	} else ++childIndex;
    }

}

void CParticle::spawnChild (size_t descriptor, const ParticleInstance* parent, bool follow) {
    if (m_childDepth >= 64 || descriptor >= m_particle.children.size ()) return;
    const auto& child = m_particle.children[descriptor];
    if (child.particleFile.empty ()) return;
    const std::string asset = std::filesystem::path (child.particleFile).lexically_normal ().generic_string ();
    if (std::find (m_childAncestry.begin (), m_childAncestry.end (), asset) != m_childAncestry.end ()) {
	sLog.error ("Particle child asset cycle at ", asset, " for object ", m_particle.id);
	return;
    }
    const size_t active = std::count_if (m_childNodes.begin (), m_childNodes.end (),
        [descriptor] (const ChildNode& node) { return node.descriptor == descriptor; });
    const size_t limit = child.type == "static" ? 1u
        : static_cast<size_t> (std::max (0, child.maxCount));
    if (active >= limit) return;

    try {
        ChildNode node;
        auto retained = parent ? m_retainedChildNodes.take (descriptor) : std::nullopt;
        const bool reused = retained.has_value ();
        if (retained) {
            node = std::move (*retained);
        } else {
            const auto vecString = [] (glm::vec3 value) {
                return std::to_string (value.x) + " " + std::to_string (value.y) + " "
                    + std::to_string (value.z);
            };
            using JSON = WallpaperEngine::Data::JSON::JSON;
            const JSON record = {
                { "id", nextParticleChildId (getScene ()) },
                { "name", child.name.empty () ? child.particleFile : child.name },
                { "particle", child.particleFile },
                { "origin", vecString (child.origin) },
                { "angles", vecString (child.angles) },
                { "scale", vecString (child.scale) },
            };
            auto parsed = WallpaperEngine::Data::Parsers::ObjectParser::parse (
                record, getScene ().getScene ().project);
            if (!parsed || !parsed->is<Particle> ()) return;
            auto model = std::unique_ptr<Particle> (static_cast<Particle*> (parsed.release ()));
            node.model = std::move (model);
            node.runtime = std::make_unique<CParticle> (
                getScene (), *node.model, m_childDepth + 1, m_childAncestry, this);
        }
        node.descriptor = descriptor;
        node.parentBirthId = parent ? parent->birthId : 0;
        node.follow = follow;
        node.detached = false;
        node.runtime->m_eventParentSlot.reset ();
        if (parent) {
            if (m_eventSlotValues.size () <= parent->poolSlot)
                m_eventSlotValues.resize (parent->poolSlot + 1);
            m_eventSlotValues[parent->poolSlot] = eventValues (*parent);
            node.runtime->m_eventParentSlot = parent->poolSlot;
        }
        node.runtime->m_hasChildParentMatrix = true;
        // One-shot spawn/death children keep their birth-space anchor. Static
        // children and live follow children inherit the changing parent basis.
        node.runtime->setChildAnchor (m_simulationModelMatrix,
            parent ? parent->position : glm::vec3 (0.0f), parent == nullptr);
        if (reused) {
            // Native reuse restores emitter clocks and sequence phase without
            // reconstructing CPs, compiled initializers, slot high-water or
            // static descendants. Reenable the retained static emitter tree.
            node.runtime->resetStaticEmitterTree ();
            node.runtime->m_preTickedByParent = false;
            node.runtime->warmup ();
        } else {
            node.runtime->setup ();
        }
        if (traceParticleChildren (*this))
            sLog.out ("CHILD_TRACE spawn object=", m_particle.id,
                      " child=", node.model->id, " descriptor=", descriptor,
		      " parentbirth=", node.parentBirthId, " reused=", reused);
        // Native 1402308a0 visits static children first, then descriptor
        // groups in authored order; keep creation order inside each group.
        const auto order = [] (const ChildNode& value) {
            return std::pair (value.parentBirthId == 0 ? 0 : 1, value.descriptor);
        };
        const auto insertion = std::upper_bound (m_childNodes.begin (), m_childNodes.end (),
            order (node), [&order] (const auto& key, const ChildNode& value) {
                return key < order (value);
            });
        m_childNodes.insert (insertion, std::move (node));
    } catch (const std::exception& error) {
        sLog.error ("Failed to create particle child ", child.name, " for ", m_particle.name,
                    ": ", error.what ());
    }
}

void CParticle::processChildEvents (const std::vector<ParticleInstance>& expired,
                                    const std::vector<ParticleInstance>& born) {
    for (const auto& particle : expired) {
        for (auto& node : m_childNodes) {
            if (node.follow && node.parentBirthId == particle.birthId) {
                node.follow = false;
                node.detached = true;
                node.runtime->disableStaticEmittersRecursively ();
                // Native event-follow invalidates +0x478 at parent expiry.
                node.runtime->m_eventParentSlot.reset ();
                node.runtime->setChildAnchor (m_simulationModelMatrix, particle.position, false);
                if (traceParticleChildren (*this))
                    sLog.out ("CHILD_TRACE detach object=", m_particle.id,
                              " child=", node.model->id, " parentbirth=", particle.birthId);
            }
        }
    }

    const auto dispatch = [this] (const ParticleInstance& particle, size_t descriptor) {
            const auto& child = m_particle.children[descriptor];
            const size_t active = std::count_if (m_childNodes.begin (), m_childNodes.end (),
                [descriptor] (const ChildNode& node) { return node.descriptor == descriptor; });
            if (active >= static_cast<size_t> (std::max (0, child.maxCount))) return;
            // Both native event branches draw for probability 0 and 1 too.
            // All particle nodes consume the scene-owned MT stream. Native
            // draw ordering in other initializer branches remains under audit.
            const float draw = ParticleCore::nativeRandomUnit (m_rng);
            if (!(draw <= child.probability)) return;
            spawnChild (descriptor, &particle, child.type == "eventfollow");
    };
    for (const auto& particle : expired) {
        for (size_t descriptor = 0; descriptor < m_particle.children.size (); ++descriptor) {
            if (m_particle.children[descriptor].type == "eventdeath") dispatch (particle, descriptor);
        }
    }
    // Native 1402378a0 walks birth descriptors outermost, then the combined
    // birth IDs from the full emitter stage. This keeps authored ordering.
    for (size_t descriptor = 0; descriptor < m_particle.children.size (); ++descriptor) {
        const auto& type = m_particle.children[descriptor].type;
        if (type != "eventfollow" && type != "eventspawn") continue;
        for (const auto& particle : born) dispatch (particle, descriptor);
    }
}

void CParticle::disableStaticEmittersRecursively () {
    // Native 14022f640 marks this node's emitter records and recurses only
    // through its static-child vector (+0x480), not event-child pools.
    m_emissionEnabled = false;
    for (auto& node : m_childNodes)
        if (m_particle.children[node.descriptor].type == "static")
            node.runtime->disableStaticEmittersRecursively ();
}

bool CParticle::isFinishedForEvent () const {
    // Native 14022c310 treats static child nodes as completed once their
    // nested emissions/particles are done; their allocation need not vanish.
    if (m_particleCount != 0) return false;
    if (m_emissionEnabled && std::any_of (m_emitterCanProduce.begin (),
                                         m_emitterCanProduce.end (),
                                         [] (uint8_t active) { return active != 0; })) return false;
    return std::all_of (m_childNodes.begin (), m_childNodes.end (),
                       [] (const ChildNode& node) { return node.runtime->isFinishedForEvent (); });
}

void CParticle::inheritControlPointsFromParent (const CParticle& parent,
                                                const ParticleChild& descriptor) {
    m_inheritedControlPointPositions.fill (std::nullopt);
    // Native 14022a580 enters for descriptor bit 0 and converts XYZ using
    // the parent's stack before the child node is pushed. This is distinct
    // from the authored parent-CP matrix link handled during child update.
    if ((descriptor.flags & 1) == 0) return;
    const bool parentWorld = (parent.m_particle.flags & 1u) != 0;
    const bool childWorld = (m_particle.flags & 1u) != 0;
    const std::optional<glm::mat4> inverseParentStack = parent.m_controlPointTransformInvertible
        ? std::optional<glm::mat4> (parent.m_controlPointInverse) : std::nullopt;
    if (parentWorld && !childWorld && !inverseParentStack) {
        if (!m_reportedInheritedCPBasisMismatch) {
            sLog.error ("Particle child control-point parent transform is singular: ", m_particle.id);
            m_reportedInheritedCPBasisMismatch = true;
        }
        return;
    }
    int destination = descriptor.controlPointStartIndex;
    if (destination < 0 || destination >= static_cast<int> (m_inheritedControlPointPositions.size ())) {
        if (!m_reportedInheritedCPBasisMismatch) {
            sLog.error ("Particle child CP start index outside Linux slots for object ", m_particle.id);
            m_reportedInheritedCPBasisMismatch = true;
        }
        return;
    }
    std::vector<const ParticleInstance*> sourceParticles;
    sourceParticles.reserve (parent.m_particleCount);
    for (uint32_t source = 0; source < parent.m_particleCount; ++source)
        sourceParticles.push_back (&parent.m_particles[source]);
    std::sort (sourceParticles.begin (), sourceParticles.end (),
               [] (const ParticleInstance* a, const ParticleInstance* b) {
                   return a->poolSlot < b->poolSlot;
               });
    for (const auto* source : sourceParticles) {
        if (destination >= 8) break;
        const auto& cp = m_controlPoints[static_cast<size_t> (destination)];
        // Native skips pointer/locked/generated CP slots and tries the same
        // destination again for the next live source particle.
        if ((cp.flags & 0x10005u) != 0) continue;
        m_inheritedControlPointPositions[static_cast<size_t> (destination)]
            = ParticleCore::childParticleControlPointPosition (
                source->position, parentWorld, childWorld,
                parent.m_simulationModelMatrix, inverseParentStack);
        ++destination;
    }
}

void CParticle::renderChildren () {
    for (size_t index = 0; index < m_childNodes.size ();) {
        auto& node = m_childNodes[index];
        node.runtime->render ();
        ++index;
    }
}

const Particle& CParticle::getParticle () const { return m_particle; }

glm::vec3 CParticle::getInstanceControlPoint (size_t index) const {
    const auto& authored = m_particle.instanceOverride.controlPoints.at (index);
    if (authored && authored->value) return authored->value->getVec3 ();
    return m_instanceControlPointOverrides.at (index);
}

void CParticle::setInstanceControlPoint (size_t index, const glm::vec3& position) {
    const auto& authored = m_particle.instanceOverride.controlPoints.at (index);
    if (authored && authored->value) authored->value->update (position, DynamicValue::Script);
    m_instanceControlPointOverrides.at (index) = position;
    if (!authored && m_instancePatchWrites) m_instancePatchWrites->mark ();
}

glm::vec3 CParticle::getInstanceControlPointAngle (size_t index) const {
    const auto& authored = m_particle.instanceOverride.controlPointAngles.at (index);
    if (authored && authored->value) return authored->value->getVec3 ();
    return m_instanceControlPointAngleOverrides.at (index);
}

void CParticle::setInstanceControlPointAngle (size_t index, const glm::vec3& angle) {
    const auto& authored = m_particle.instanceOverride.controlPointAngles.at (index);
    if (authored && authored->value) authored->value->update (angle, DynamicValue::Script);
    m_instanceControlPointAngleOverrides.at (index) = angle;
    if (!authored && m_instancePatchWrites) m_instancePatchWrites->mark ();
}

const float& CParticle::getBrightness () const { return m_overbright; }

const float& CParticle::getUserAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const float& CParticle::getAlpha () const { return m_particle.instanceOverride.alpha->value->getFloat (); }

const glm::vec3& CParticle::getColor () const {
    static const glm::vec3 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec3 ();
    }
    return defaultColor;
}

const glm::vec4& CParticle::getColor4 () const {
    static const glm::vec4 defaultColor (1.0f);
    if (m_particle.instanceOverride.color && m_particle.instanceOverride.color->value) {
	return m_particle.instanceOverride.color->value->getVec4 ();
    }
    return defaultColor;
}

const glm::vec3& CParticle::getCompositeColor () const { return getColor (); }

// ========== EMITTERS ==========

void CParticle::setupEmitters () {
    for (const auto& emitter : m_particle.emitters) {
	EmitterFunc func;

	if (emitter.name == "boxrandom") {
	    func = createBoxEmitter (emitter, m_emitters.size ());
	} else if (emitter.name == "sphererandom") {
	    func = createSphereEmitter (emitter, m_emitters.size ());
	} else if (emitter.name == "layerimage") {
	    func = createImageEmitter (emitter, m_emitters.size ());
	} else {
	    sLog.out ("Unknown emitter type: ", emitter.name);
	    continue;
	}

	if (func) {
	    m_emitters.push_back (std::move (func));
	    const auto schedule = ParticleCore::scheduleConfig (emitter, emitter.rate);
	    m_emitterCanProduce.push_back (ParticleCore::emitterCanProduceMore (
	        schedule, ParticleCore::initialState (schedule)) ? 1 : 0);
	}
    }
}

EmitterFunc CParticle::createBoxEmitter (const ParticleEmitter& emitter, size_t index) {
    float rate = emitter.rate;

    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) {
	    controlPointIndex = 0;
	}
    }

    glm::vec3 flippedDirections = emitter.directions;
    flippedDirections.y = -flippedDirections.y;

    const ParticleCore::EmitterScheduleConfig schedule = ParticleCore::scheduleConfig (emitter, rate);

    return
	[this, emitter, transformedEmitterOrigin, controlPointIndex, flippedDirections, schedule, index,
	 state = ParticleCore::initialState (schedule)]
	(std::vector<ParticleInstance>& particles, uint32_t& count, float dt) mutable {
	    auto activeSchedule = schedule;
	    // Native emitter descriptors multiply production by the shared scene
	    // instance count, unless the preset's bit 0x20 suppresses that binding.
	    activeSchedule.rate = ParticleCore::effectiveRate (
	        schedule.rate, (m_particle.flags & 0x20u) != 0
	            ? 1.0f : countOverrideValue ()->getFloat ());
	    if (emitter.audioProcessingMode != 0) {
		activeSchedule.rate = ParticleCore::effectiveRate (
		    activeSchedule.rate, sampleParticleAudio (*this, audioSettings (emitter)));
	    }
	    const uint32_t capacity = emissionCapacity (particles, count);
	    bool periodRestarted = false;
	    const uint32_t toEmit = m_forcedEmitCount != 0
	        ? ParticleCore::advanceEmitterForced (activeSchedule, state, m_forcedEmitCount,
	            capacity, [this] (float min, float max) {
	                return WallpaperEngine::Maths::randomFloat (m_rng, min, max);
	            }, &periodRestarted)
	        : ParticleCore::advanceEmitter (activeSchedule, state, dt, capacity,
	            [this] (float min, float max) { return WallpaperEngine::Maths::randomFloat (m_rng, min, max); },
	            &periodRestarted);
	    if (periodRestarted) resetPeriodicChildren ();
	    m_emitterCanProduce[index] = ParticleCore::emitterCanProduceMore (schedule, state);

	    // Emit particles
	    for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
		auto& p = emittedParticleTarget (particles, count);

		glm::vec3 spawnOrigin = transformedEmitterOrigin;
		if (controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())) {
		    spawnOrigin += m_controlPoints[controlPointIndex].position;
		}

		glm::vec3 randomPos = ParticleCore::nativeBoxDisplacement (
		    m_rng, flippedDirections, emitter.distanceMin, emitter.distanceMax);
		// 1402378a0: cVar43 is preset world bit OR nonzero CP index.
		if (((m_particle.flags & 1u) != 0 || controlPointIndex > 0)
		    && controlPointIndex >= 0
		    && controlPointIndex < static_cast<int> (m_controlPoints.size ()))
		    randomPos = m_controlPoints[controlPointIndex].basis * randomPos;

		p.position = spawnOrigin + randomPos;

		// Native normalizes the stored position delta, after float rounding of
		// spawnOrigin + randomPos (1402378a0:978–990).
		glm::vec3 velocityDirection = p.position - spawnOrigin;
		if (ParticleCore::nativeBoxNeedsFallback (velocityDirection)) {
		    // Native opcode 2 uses three more draws for a zero-direction fallback:
		    // Z, Y, X (1402378a0:994–1011), then normalizes it.
		    const float z = ParticleCore::nativeRandomUnit (m_rng);
		    const float y = ParticleCore::nativeRandomUnit (m_rng);
		    const float x = ParticleCore::nativeRandomUnit (m_rng);
		    velocityDirection = glm::vec3 (
		        (x + x - 1.0f) * flippedDirections.x,
		        (y + y - 1.0f) * flippedDirections.y,
		        (z + z - 1.0f) * flippedDirections.z);
		}
		const float directionLength = glm::length (velocityDirection);
		const glm::vec2 speedBounds = ParticleCore::emitterSpeedBounds (
		    emitter.speedMin, emitter.speedMax, speedOverrideValue ()->getFloat (), m_particle.flags);
		const float speed = WallpaperEngine::Maths::randomFloat (
		    m_rng, speedBounds.x, speedBounds.y);
		p.velocity = directionLength > 0.0f
		    ? velocityDirection * (speed / directionLength) : glm::vec3 (0.0f);
		p.acceleration = glm::vec3 (0.0f);
		p.rotation = glm::vec3 (0.0f);
		p.angularVelocity = glm::vec3 (0.0f);
		p.angularAcceleration = glm::vec3 (0.0f);

		p.color = instanceBirthRgbGain ();
		p.alpha = alphaOverrideValue ()->getFloat ();
		p.size = m_slotStreams ? 0.5f : 20.0f * ((m_particle.flags & 0x80u) != 0
		    ? 1.0f : sizeOverrideValue ()->getFloat ());
		p.lifetime = m_slotStreams ? 1.0f : lifetimeOverrideValue ()->getFloat ();
		p.age = 0.0f;
		p.oscillatorRandom = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
		p.alive = true;
	    p.previousPosition = p.position;
	    p.sequenceFraction = m_particle.animationMode == "randomframe" ? p.oscillatorRandom : 0.0f;
		p.frame = -1.0f;

		p.initial.color = p.color;
		p.initial.alpha = p.alpha;
		p.initial.size = p.size;
		p.initial.lifetime = p.lifetime;


		// Native 1402378a0 passes this emitter's selected CP upper 3x3 to
		// initializer opcode 9, independently of the spawn-offset branch.
		m_birthInitializerBasis = controlPointIndex >= 0
		    && controlPointIndex < static_cast<int> (m_controlPoints.size ())
		    ? m_controlPoints[controlPointIndex].basis : glm::mat3 (1.0f);
		// Apply initializers
		for (auto& init : m_initializers) {
		    init (p);
		}

		finishEmittedParticle (p, count);
	    }
	};
}

EmitterFunc CParticle::createSphereEmitter (const ParticleEmitter& emitter, size_t index) {
    float rate = emitter.rate;
    float lifetime = lifetimeOverrideValue ()->getFloat ();
    // Factory 1401b9100 reads scene orthogonalprojection bit 0x400, not
    // the particle's own perspective-render flag.
    const float distanceMax = !emitter.distanceMaxAuthored
        && !getScene ().getCamera ().isOrthogonal () ? 1.0f : emitter.distanceMax.x;

    // Convert emitter origin from screen space (Y down) to centered space (Y up)
    glm::vec3 transformedEmitterOrigin = emitter.origin;
    transformedEmitterOrigin.y = -transformedEmitterOrigin.y;

    int controlPointIndex = emitter.controlPoint;

    // Auto-detect control point 0 usage if controlPoint field not specified and CP0 has linkMouse
    if (controlPointIndex == -1 && !m_particle.controlPoints.empty ()) {
	const auto& cp0 = m_particle.controlPoints[0];
	if ((cp0.flags & 1) != 0) { // Bit 0: linkMouse flag
	    controlPointIndex = 0;
	}
    }

    const ParticleCore::EmitterScheduleConfig schedule = ParticleCore::scheduleConfig (emitter, rate);

    return [this, emitter, transformedEmitterOrigin, controlPointIndex, lifetime, distanceMax, schedule, index,
	    state = ParticleCore::initialState (schedule)]
	(std::vector<ParticleInstance>& particles, uint32_t& count, float dt) mutable {
	auto activeSchedule = schedule;
	activeSchedule.rate = ParticleCore::effectiveRate (
	    schedule.rate, (m_particle.flags & 0x20u) != 0
	        ? 1.0f : countOverrideValue ()->getFloat ());
	if (emitter.audioProcessingMode != 0) {
	    activeSchedule.rate = ParticleCore::effectiveRate (
		activeSchedule.rate, sampleParticleAudio (*this, audioSettings (emitter)));
	}
	const uint32_t capacity = emissionCapacity (particles, count);
	bool periodRestarted = false;
	const uint32_t toEmit = m_forcedEmitCount != 0
	    ? ParticleCore::advanceEmitterForced (activeSchedule, state, m_forcedEmitCount,
	        capacity, [this] (float min, float max) {
	            return WallpaperEngine::Maths::randomFloat (m_rng, min, max);
	        }, &periodRestarted)
	    : ParticleCore::advanceEmitter (activeSchedule, state, dt, capacity,
	        [this] (float min, float max) { return WallpaperEngine::Maths::randomFloat (m_rng, min, max); },
	        &periodRestarted);
	if (periodRestarted) resetPeriodicChildren ();
	m_emitterCanProduce[index] = ParticleCore::emitterCanProduceMore (schedule, state);

	for (uint32_t i = 0; i < toEmit && count < particles.size (); i++) {
	    auto& p = emittedParticleTarget (particles, count);

	    // Determine spawn origin (control point or emitter origin)
	    glm::vec3 spawnOrigin = transformedEmitterOrigin;
	    if (controlPointIndex >= 0 && controlPointIndex < static_cast<int> (m_controlPoints.size ())) {
		spawnOrigin += m_controlPoints[controlPointIndex].position;
	    }

	    // Opcode 1 consumes angle, axial cone coordinate and cube-root radius
	    // in that order on every birth, independently of the scene projection.
	    const float angle = ParticleCore::nativeRandomUnit (m_rng);
	    const float axial = ParticleCore::nativeRandomUnit (m_rng);
	    const float radius = ParticleCore::nativeRandomUnit (m_rng);
	    const glm::vec3 authored = ParticleCore::nativeSphereDisplacement (
		angle, axial, radius, emitter.directions, emitter.distanceMin.x,
		distanceMax, emitter.cone,
		(emitter.flags & 1u) != 0 ? emitter.sign : glm::ivec3 (0));
	    glm::vec3 randomPos (authored.x, -authored.y, authored.z);
	    // 1402378a0: cVar43 selects the CP basis for preset world bit or a
	    // nonzero CP index, just as in the box emitter.
	    const bool useCPBasis = ((m_particle.flags & 1u) != 0 || controlPointIndex > 0)
		&& controlPointIndex >= 0
		&& controlPointIndex < static_cast<int> (m_controlPoints.size ());
	    if (useCPBasis) randomPos = m_controlPoints[controlPointIndex].basis * randomPos;
	    p.position = spawnOrigin + randomPos;

	    glm::vec3 velocityDirection = randomPos;
	    if (glm::dot (randomPos, randomPos) < 0.0001f) {
		// Native tests transformed 4D length squared before choosing a
		// separate three-draw fallback velocity direction (X, Y, Z).
		const float fallbackX = ParticleCore::nativeRandomUnit (m_rng);
		const float fallbackY = ParticleCore::nativeRandomUnit (m_rng);
		const float fallbackZ = ParticleCore::nativeRandomUnit (m_rng);
		velocityDirection = glm::vec3 (
		    ((fallbackX + fallbackX) - 1.0f) * emitter.directions.x,
		    -((fallbackY + fallbackY) - 1.0f) * emitter.directions.y,
		    ((fallbackZ + fallbackZ) - 1.0f) * emitter.directions.z);
		if (useCPBasis)
		    velocityDirection = m_controlPoints[controlPointIndex].basis * velocityDirection;
	    }
	    // Native draws speed even for identical zero bounds, so its shared RNG
	    // stream stays aligned with birth random and subsequent initializers.
	    const glm::vec2 speedBounds = ParticleCore::emitterSpeedBounds (
		emitter.speedMin, emitter.speedMax, speedOverrideValue ()->getFloat (), m_particle.flags);
	    const float speed = WallpaperEngine::Maths::randomFloat (
		m_rng, speedBounds.x, speedBounds.y);
	    const float directionLength = glm::length (velocityDirection);
	    p.velocity = directionLength > 0.0f
		? velocityDirection * (speed / directionLength) : glm::vec3 (0.0f);

	    p.acceleration = glm::vec3 (0.0f);
	    p.rotation = glm::vec3 (0.0f);
	    p.angularVelocity = glm::vec3 (0.0f);
	    p.angularAcceleration = glm::vec3 (0.0f);

	    p.color = instanceBirthRgbGain ();
	    p.alpha = alphaOverrideValue ()->getFloat ();
	    p.size = m_slotStreams ? 0.5f : 20.0f * ((m_particle.flags & 0x80u) != 0
	        ? 1.0f : sizeOverrideValue ()->getFloat ());
	    p.lifetime = m_slotStreams ? 1.0f : lifetime;
	    p.age = 0.0f;
	    p.oscillatorRandom = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    p.alive = true;
		p.previousPosition = p.position;
		p.sequenceFraction = m_particle.animationMode == "randomframe" ? p.oscillatorRandom : 0.0f;
	    p.frame = -1.0f;

	    p.initial.color = p.color;
	    p.initial.alpha = p.alpha;
	    p.initial.size = p.size;
	    p.initial.lifetime = p.lifetime;


	    m_birthInitializerBasis = controlPointIndex >= 0
		&& controlPointIndex < static_cast<int> (m_controlPoints.size ())
		? m_controlPoints[controlPointIndex].basis : glm::mat3 (1.0f);
	    for (auto& init : m_initializers) {
		init (p);
	    }

	    finishEmittedParticle (p, count);
	}
    };
}

EmitterFunc CParticle::createImageEmitter (const ParticleEmitter& emitter, size_t index) {
    int sourceId = -1;
    std::string maskPath;
    for (const auto& dependency : m_particle.typedDependencies) {
        if (dependency.type == "emitterimage" && dependency.index == static_cast<int> (index)) {
            sourceId = dependency.id;
            maskPath = dependency.mask;
            break;
        }
    }
    if (sourceId < 0) {
        sLog.error ("Layer-image emitter has no emitterimage dependency: ", m_particle.name,
                    " emitter index ", index);
        return {};
    }
    if ((emitter.flags & 0x40000u) != 0) {
        sLog.error ("Layer-image emitter velocity-from-source flag needs native matrix path: ",
                    m_particle.name);
        return {};
    }

    const auto schedule = ParticleCore::scheduleConfig (emitter, emitter.rate);
    struct Cache {
        ParticleImageEmitterReadback reader;
        std::vector<ParticleCore::ImageEmitterSample> samples;
        GLuint textureId = 0;
        GLuint maskTextureId = 0;
        uint32_t width = 0, height = 0;
        float refreshElapsed = 0.0f;
        bool ready = false;
        bool pendingActivation = false;
        bool warnedPuppet = false;
        bool warnedMask = false;
    };
    auto cache = std::make_shared<Cache> ();
    return [this, emitter, index, sourceId, maskPath, schedule, cache,
            state = ParticleCore::initialState (schedule)]
        (std::vector<ParticleInstance>& particles, uint32_t& count, float dt) mutable {
        auto activeSchedule = schedule;
        activeSchedule.rate = ParticleCore::effectiveRate (
            schedule.rate, (m_particle.flags & 0x20u) != 0
                ? 1.0f : countOverrideValue ()->getFloat ());
        if (emitter.audioProcessingMode != 0) {
            activeSchedule.rate = ParticleCore::effectiveRate (
                activeSchedule.rate, sampleParticleAudio (*this, audioSettings (emitter)));
        }
        const uint32_t capacity = emissionCapacity (particles, count);
        bool periodRestarted = false;
        const uint32_t toEmit = m_forcedEmitCount != 0
            ? ParticleCore::advanceEmitterForced (activeSchedule, state, m_forcedEmitCount,
                capacity, [this] (float min, float max) {
                    return WallpaperEngine::Maths::randomFloat (m_rng, min, max);
                }, &periodRestarted)
            : ParticleCore::advanceEmitter (activeSchedule, state, dt, capacity,
                [this] (float min, float max) {
                    return WallpaperEngine::Maths::randomFloat (m_rng, min, max);
                }, &periodRestarted);
        if (periodRestarted) resetPeriodicChildren ();
        m_emitterCanProduce[index] = ParticleCore::emitterCanProduceMore (schedule, state);
        // 1402378a0:470 encloses opcode 3 in positive scheduled births;
        // zero-birth ticks neither prepare nor refresh its image cache.
        if (toEmit == 0) return;

        const auto* object = getScene ().getObject (sourceId);
        const auto* image = dynamic_cast<const CImage*> (object);
        if (!image) return;
        if (image->hasPuppetEmissionDeformation ()) {
            if (!cache->warnedPuppet) {
                sLog.error ("Layer-image emitter puppet source needs selected-pixel bone association: "
                            "particle=", getId (), " source=", sourceId,
                            " emitter index=", index);
                cache->warnedPuppet = true;
            }
            return;
        }
        const auto source = image->getTexture ();
        if (!source || !source->isReady ()) return;
        std::shared_ptr<const TextureProvider> mask;
        if (!maskPath.empty ()) {
            try {
                mask = getScene ().getContext ().resolveTexture (maskPath);
            } catch (const std::exception& error) {
                if (!cache->warnedMask) {
                    sLog.error ("Layer-image emitter opacity mask failed to load: particle=", getId (),
                        " source=", sourceId, " emitter index=", index,
                        " mask=", maskPath, " error=", error.what ());
                    cache->warnedMask = true;
                }
                return;
            }
        }
        if (!maskPath.empty () && (!mask || !mask->isReady ())) {
            if (!cache->warnedMask) {
                sLog.error ("Layer-image emitter opacity mask is unavailable: particle=", getId (),
                    " source=", sourceId, " emitter index=", index, " mask=", maskPath);
                cache->warnedMask = true;
            }
            return;
        }
        cache->warnedMask = false;
        const GLuint textureId = source->getTextureID (0);
        const GLuint maskTextureId = mask ? mask->getTextureID (0) : 0;
        const uint32_t width = source->getRealWidth (), height = source->getRealHeight ();
        if (textureId != cache->textureId || maskTextureId != cache->maskTextureId
            || width != cache->width || height != cache->height) {
            cache->ready = false;
            cache->pendingActivation = false;
            cache->textureId = textureId;
            cache->maskTextureId = maskTextureId;
            cache->width = width;
            cache->height = height;
        }
        if ((emitter.flags & 0x20000u) != 0) {
            cache->refreshElapsed += dt;
            if (cache->refreshElapsed > 1.0f) {
                cache->ready = false;
                cache->pendingActivation = false;
                cache->refreshElapsed = 0.0f;
            }
        }
        if (cache->pendingActivation) {
            cache->pendingActivation = false;
            cache->ready = true;
        } else if (!cache->ready) {
            cache->pendingActivation = cache->reader.sample (*source, mask.get (), cache->samples);
            if (cache->pendingActivation && traceParticleChildren (*this))
                sLog.out ("Particle image emitter sample list prepared: particle=", getId (),
                    " source=", sourceId, " count=", cache->samples.size (),
                    " sourceSize=", width, "x", height);
            return;
        }
        if (!cache->ready || cache->samples.empty () || toEmit == 0) return;

        auto findParent = [this] (int parentId) -> const Object* {
            const auto* parent = getScene ().getObject (parentId);
            return parent ? &parent->getObject () : nullptr;
        };
        const auto sourceTransform = Wallpapers::resolveSceneTransform (
            image->getImage (), findParent);
        const auto particleTransform = Wallpapers::resolveSceneTransform (
            m_particle, findParent);
        const auto inverseParticle = Wallpapers::inverseFiniteTransform (
            particleTransform.authoredMatrix);
        if ((m_particle.flags & 1u) == 0 && !inverseParticle) return;
        const glm::vec2 sourceSize = image->getSize ();
        const glm::vec2 pixelScale (
            sourceSize.x / static_cast<float> (width),
            sourceSize.y / static_cast<float> (height));

        for (uint32_t emitted = 0; emitted < toEmit && count < particles.size (); ++emitted) {
            auto& particle = emittedParticleTarget (particles, count);
            const uint32_t chosen = ParticleCore::nativeImageSampleIndex (
                m_rng, static_cast<uint32_t> (cache->samples.size ()));
            const auto& sample = cache->samples[chosen];
            glm::vec3 offset (0.0f);
            if ((emitter.flags & 0x80000u) != 0) {
                offset.x = WallpaperEngine::Maths::randomFloat (
                    m_rng, emitter.offsetMin.x, emitter.offsetMax.x);
                offset.y = WallpaperEngine::Maths::randomFloat (
                    m_rng, emitter.offsetMin.y, emitter.offsetMax.y);
                offset.z = WallpaperEngine::Maths::randomFloat (
                    m_rng, emitter.offsetMin.z, emitter.offsetMax.z);
            }
            const glm::vec4 sourcePoint (
                (static_cast<float> (sample.x) + offset.x) * pixelScale.x,
                (-static_cast<float> (sample.y) + offset.y) * pixelScale.y,
                offset.z, 1.0f);
            const glm::vec4 worldAuthored = sourceTransform.authoredMatrix * sourcePoint;
            const glm::vec4 birthAuthored = (m_particle.flags & 1u) != 0
                ? worldAuthored : *inverseParticle * worldAuthored;
            particle.position = {birthAuthored.x, -birthAuthored.y, birthAuthored.z};
            particle.velocity = glm::vec3 (0.0f);
            particle.acceleration = glm::vec3 (0.0f);
            particle.rotation = glm::vec3 (0.0f);
            particle.angularVelocity = glm::vec3 (0.0f);
            particle.angularAcceleration = glm::vec3 (0.0f);
            particle.color = instanceBirthRgbGain ();
            if ((emitter.flags & 0x10000u) != 0)
                particle.color *= glm::vec3 (sample.red, sample.green, sample.blue) / 255.0f;
            particle.alpha = alphaOverrideValue ()->getFloat ();
            particle.size = 20.0f * ((m_particle.flags & 0x80u) != 0
                ? 1.0f : sizeOverrideValue ()->getFloat ());
            particle.lifetime = lifetimeOverrideValue ()->getFloat ();
            particle.age = 0.0f;
            particle.oscillatorRandom = ParticleCore::nativeRandomUnit (m_rng);
            particle.alive = true;
            particle.frame = -1.0f;
            particle.initial.color = particle.color;
            particle.initial.alpha = particle.alpha;
            particle.initial.size = particle.size;
            particle.initial.lifetime = particle.lifetime;
            if (emitted == 0 && traceParticleChildren (*this))
                sLog.out ("Particle image emitter birth: particle=", getId (),
                    " source=", sourceId, " selected=", chosen,
                    " pixel=", sample.x, ",", sample.y,
                    " position=", particle.position.x, ",", particle.position.y,
                    " color=", particle.color.x, ",", particle.color.y, ",", particle.color.z);
            // Native image opcode 3 supplies scene-stack basis for world
            // particles and identity for local particles (238c54–cc6).
            m_birthInitializerBasis = (m_particle.flags & 1u) != 0
                ? glm::mat3 (m_simulationModelMatrix) : glm::mat3 (1.0f);
            for (auto& initializer : m_initializers) initializer (particle);
            finishEmittedParticle (particle, count);
        }
    };
}

// ========== INITIALIZERS ==========

void CParticle::setupInitializers () {
    for (const auto& initializer : m_particle.initializers) {
	if (!initializer) {
	    continue;
	}

	InitializerFunc func;

	if (initializer->is<InheritInitialValueFromEventInitializer> ()) {
            func = createInheritInitialValueFromEventInitializer (*initializer->as<InheritInitialValueFromEventInitializer> ());
        } else if (initializer->is<InheritControlPointVelocityInitializer> ()) {
            func = createInheritControlPointVelocityInitializer (*initializer->as<InheritControlPointVelocityInitializer> ());
        } else if (initializer->is<ColorRandomInitializer> ()) {
	    func = createColorRandomInitializer (*initializer->as<ColorRandomInitializer> ());
	} else if (initializer->is<HsvColorRandomInitializer> ()) {
	    func = createHsvColorRandomInitializer (*initializer->as<HsvColorRandomInitializer> ());
	} else if (initializer->is<ColorListInitializer> ()) {
	    func = createColorListInitializer (*initializer->as<ColorListInitializer> ());
	} else if (initializer->is<PositionOffsetRandomInitializer> ()) {
	    func = createPositionOffsetRandomInitializer (*initializer->as<PositionOffsetRandomInitializer> ());
	} else if (initializer->is<MapSequenceBetweenControlPointsInitializer> ()) {
	    func = createMapSequenceBetweenControlPointsInitializer (
	        *initializer->as<MapSequenceBetweenControlPointsInitializer> ());
	} else if (initializer->is<RemapInitialValueInitializer> ()) {
	    func = createRemapInitialValueInitializer (*initializer->as<RemapInitialValueInitializer> ());
	} else if (initializer->is<SizeRandomInitializer> ()) {
	    func = createSizeRandomInitializer (*initializer->as<SizeRandomInitializer> ());
	} else if (initializer->is<AlphaRandomInitializer> ()) {
	    func = createAlphaRandomInitializer (*initializer->as<AlphaRandomInitializer> ());
	} else if (initializer->is<LifetimeRandomInitializer> ()) {
	    const auto& lifeInit = *initializer->as<LifetimeRandomInitializer> ();
	    func = createLifetimeRandomInitializer (lifeInit);
	} else if (initializer->is<VelocityRandomInitializer> ()) {
	    func = createVelocityRandomInitializer (*initializer->as<VelocityRandomInitializer> ());
	} else if (initializer->is<RotationRandomInitializer> ()) {
	    func = createRotationRandomInitializer (*initializer->as<RotationRandomInitializer> ());
	} else if (initializer->is<AngularVelocityRandomInitializer> ()) {
	    func = createAngularVelocityRandomInitializer (*initializer->as<AngularVelocityRandomInitializer> ());
	} else if (initializer->is<TurbulentVelocityRandomInitializer> ()) {
	    func = createTurbulentVelocityRandomInitializer (*initializer->as<TurbulentVelocityRandomInitializer> ());
	} else if (initializer->is<MapSequenceAroundControlPointInitializer> ()) {
	    func = createMapSequenceAroundControlPointInitializer (
		*initializer->as<MapSequenceAroundControlPointInitializer> ()
	    );
	} else {
	    sLog.out ("Unknown initializer type");
	}

	if (func) {
	    m_initializers.push_back (std::move (func));
	}
    }
}

ParticleCore::EventParticleValues CParticle::eventValues (const ParticleInstance& p) const {
    const bool randomRotation = std::any_of (m_particle.initializers.begin (), m_particle.initializers.end (),
        [] (const auto& init) { return init && init->template is<RotationRandomInitializer> (); });
    const bool angularMovement = std::any_of (m_particle.operators.begin (), m_particle.operators.end (),
        [] (const auto& op) { return op && op->template is<AngularMovementOperator> (); });
    const bool randomAngularVelocity = std::any_of (m_particle.initializers.begin (), m_particle.initializers.end (),
        [] (const auto& init) { return init && init->template is<AngularVelocityRandomInitializer> (); });
    return {p.color, p.alpha, p.size, p.velocity, p.rotation, p.angularVelocity,
            randomRotation || angularMovement, randomAngularVelocity || angularMovement, true, p.alive && p.lifetime > 0.0f && p.age <= p.lifetime};
}

ParticleCore::EventParticleValues CParticle::parentEventValues () const {
    if (!m_eventParentSlot || !m_parentParticleRuntime) return {};
    const auto& parent = *m_parentParticleRuntime;
    // A compact vector is an implementation detail: match the native pool
    // slot, not the spatial event-follow birth ID or a generation guard.
    for (uint32_t i = 0; i < parent.m_particleCount; ++i)
        if (parent.m_particles[i].poolSlot == *m_eventParentSlot)
            return parent.eventValues (parent.m_particles[i]);
    return *m_eventParentSlot < parent.m_eventSlotValues.size ()
        ? parent.m_eventSlotValues[*m_eventParentSlot] : ParticleCore::EventParticleValues {};
}

void CParticle::applyEventValues (ParticleInstance& p, const ParticleCore::EventParticleValues& values, bool birth) {
    p.color = values.color; p.alpha = values.alpha; p.size = values.size;
    p.velocity = values.velocity; p.rotation = values.rotation; p.angularVelocity = values.angularVelocity;
    if (birth) { p.initial.color = p.color; p.initial.alpha = p.alpha; p.initial.size = p.size; }
}

InitializerFunc CParticle::createInheritControlPointVelocityInitializer (const InheritControlPointVelocityInitializer& init) {
    return [this, &init] (ParticleInstance& p) {
        const float rawIndex = init.controlPoint->value->getFloat ();
        const size_t index = !std::isfinite (rawIndex) || rawIndex < 0.0f || rawIndex >= 8.0f
            ? 7 : static_cast<size_t> (rawIndex);
        if (index >= m_controlPoints.size ()) return;
        const auto& cp = m_controlPoints[index];
        const float speed = (m_particle.flags & 0x10u) != 0 ? 1.0f : speedOverrideValue ()->getFloat ();
        // root+0x150 is the previous OUTER scene duration (14017fa70:215),
        // independent of this node's rate and any half-duration operator pass.
        // Preserve the native single MT draw on zero-time startup as well;
        // avoid poisoning the finite simulation when native 0/0 is undefined.
        const float previousSceneDuration = getScene ().getPreviousParticleSceneDuration ();
        const bool validDuration = std::isfinite (previousSceneDuration) && previousSceneDuration > 0.0f;
        const float elapsed = validDuration ? previousSceneDuration : 1.0f;
        const auto previous = validDuration ? cp.previousPosition : cp.position;
        p.velocity += ParticleCore::inheritedControlPointVelocity (
            m_rng, cp.position, previous, elapsed, init.min->value->getFloat () * speed,
            init.max->value->getFloat () * speed, m_birthInitializerBasis,
            glm::mat3 (m_controlPointInverse), (m_particle.flags & 1u) != 0, cp.worldSpace);
    };
}

InitializerFunc CParticle::createInheritInitialValueFromEventInitializer (const InheritInitialValueFromEventInitializer& init) {
    return [this, &init] (ParticleInstance& p) {
        auto values = eventValues (p);
        if (ParticleCore::applyEventInheritance (values, parentEventValues (),
                static_cast<ParticleCore::EventInheritanceMode> (init.mode)))
            applyEventValues (p, values, true);
    };
}

InitializerFunc CParticle::createHsvColorRandomInitializer (const HsvColorRandomInitializer& init) {
    return [this, &init] (ParticleInstance& p) {
        const CParticle* root = instanceOverrideOwner ();
        const glm::vec3 tint = sharedInstanceOverrideValue (&ParticleInstanceOverride::colorn)->getVec3 ();
        const bool tintValid = ParticleCore::particleInstanceTintValid (tint,
            root->m_particle.presetColorN, m_particle.presetColorN,
            root->m_particle.flags, m_particle.flags, root == this);
        // The Windows CRT stream is thread-local and performance-time seeded;
        // it must not consume or reseed the scene's shared particle MT stream.
        thread_local uint32_t crtState = static_cast<uint32_t> (
            std::chrono::steady_clock::now ().time_since_epoch ().count ());
        const float rawSteps = init.hueSteps->value->getFloat ();
        const int steps = std::isfinite (rawSteps) && rawSteps >= 0.0f && rawSteps < 2147483648.0f
            ? static_cast<int> (rawSteps) : 0;
        const ParticleCore::HsvRandomRange range {
            init.hueMin->value->getFloat (), init.hueMax->value->getFloat (), steps,
            init.saturationMin->value->getFloat (), init.saturationMax->value->getFloat (),
            init.valueMin->value->getFloat (), init.valueMax->value->getFloat () };
        p.color *= ParticleCore::nativeHsvRandomSample (
            m_rng, ParticleCore::nativeCrtRandomWord (crtState), range, tint, tintValid);
        p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createColorListInitializer (const ColorListInitializer& init) {
    return [this, &init] (ParticleInstance& p) {
        ParticleCore::ColorListRandomRange range;
        range.colorsRgb.clear ();
        for (const auto& color : init.colors) range.colorsRgb.push_back (color->value->getVec3 ());
        range.noise = { init.hueNoise->value->getFloat (), init.saturationNoise->value->getFloat (),
                        init.valueNoise->value->getFloat () };
        const CParticle* root = instanceOverrideOwner ();
        const glm::vec3 tint = sharedInstanceOverrideValue (&ParticleInstanceOverride::colorn)->getVec3 ();
        const bool tintValid = ParticleCore::particleInstanceTintValid (tint,
            root->m_particle.presetColorN, m_particle.presetColorN,
            root->m_particle.flags, m_particle.flags, root == this);
        p.color *= ParticleCore::nativeColorListSample (m_rng, range, tint, tintValid);
        p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createPositionOffsetRandomInitializer (const PositionOffsetRandomInitializer& init) {
    auto* scale = init.scale->value.get ();
    auto* distance = init.distance->value.get ();
    auto* timeScale = init.timeScale->value.get ();
    return [this, scale, distance, timeScale, directions = init.directions,
            sign = init.sign, octaves = init.octaves] (ParticleInstance& p) {
        const auto position = ParticleCore::positionOffsetBirth (
            ParticleCore::toAuthoredVector (p.position), getScene ().getParticleSceneTime (),
            scale->getFloat (), distance->getFloat (), timeScale->getFloat (),
            octaves, directions, sign);
        if (position) p.position = ParticleCore::toSimulationVector (*position);
        else sLog.error ("Particle position offset outside bounded noise domain: ", m_particle.name);
    };
}

InitializerFunc CParticle::createMapSequenceBetweenControlPointsInitializer (
    const MapSequenceBetweenControlPointsInitializer& init) {
    auto* start = init.controlPointStart->value.get ();
    auto* end = init.controlPointEnd->value.get ();
    auto counter = std::make_shared<SequenceCounter> ();
    counter->count = init.count->value.get ();
    counter->step = ParticleCore::betweenControlPointsStep (counter->count->getFloat ());
    counter->instanceCountPatch = { counter->count->getFloat (),
        (init.flags & 16u) != 0 && (m_particle.flags & 0x20u) == 0 };
    // Native sequence 14 periodic reset uses authored bit 0x20; its bit 2
    // tapers birth velocity and is unrelated to circular sequence reset.
    counter->flags = (init.flags & 0x20u) != 0 ? 2u : 0u;
    m_sequenceCounters.push_back (counter);
    return [this, start, end, counter, bounds = init.bounds,
            mirror = init.limitBehavior == "mirror", flags = init.flags,
            arcAmount = init.arcAmount, arcDirection = ParticleCore::toSimulationVector (init.arcDirection),
            sizeReduction = init.sizeReduction] (ParticleInstance& p) {
        const auto cpPosition = [this] (DynamicValue* value) {
            const float raw = value->getFloat ();
            // Native factory casts to unsigned and bounds CP indices at 7.
            const size_t index = std::isfinite (raw) && raw >= 0.0f && raw < 8.0f
                ? static_cast<size_t> (raw) : 7u;
            return index < m_controlPoints.size () ? m_controlPoints[index].position : glm::vec3 (0.0f);
        };
        ParticleCore::BetweenControlPointsState state { counter->phase, counter->step };
        const auto result = ParticleCore::betweenControlPointsBirth (
            p.position, p.velocity, p.size, cpPosition (start), cpPosition (end),
            state, bounds, mirror, flags, arcAmount, arcDirection, sizeReduction,
            (m_particle.flags & 1u) != 0);
        counter->phase = state.phase;
        counter->step = state.step;
        p.position = result.position;
        p.velocity = result.velocity;
        p.size = result.size;
        p.initial.size = p.size;
    };
}

InitializerFunc CParticle::createRemapInitialValueInitializer (const RemapInitialValueInitializer& init) {
    OperatorFunc remap;
    if (init.remap->is<ScalarRemapValueOperator> ())
        remap = createScalarRemapValueOperator (*init.remap->as<ScalarRemapValueOperator> (), true);
    else if (init.remap->is<VectorRemapValueOperator> ())
        remap = createVectorRemapValueOperator (*init.remap->as<VectorRemapValueOperator> (), true);
    if (!remap) return {};
    return [this, remap = std::move (remap), one = std::vector<ParticleInstance> (1)]
           (ParticleInstance& p) mutable {
        one[0] = p;
        remap (one, 1, m_controlPoints, 0.0f, ParticleCore::MovementTime { 0.0f, 0.0f });
        p = one[0];
        p.initial.color = p.color;
        p.initial.alpha = p.alpha;
        p.initial.size = p.size;
        p.initial.lifetime = p.lifetime;
    };
}

InitializerFunc CParticle::createColorRandomInitializer (const ColorRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    return [this, minValue, maxValue] (ParticleInstance& p) {
	const glm::vec3 min = instanceTintedColorEndpoint (minValue->getVec3 ());
	const glm::vec3 max = instanceTintedColorEndpoint (maxValue->getVec3 ());
	// Native initializer opcode 3 draws one scalar and uses it for all RGB
	// channels (14023b340:389–414). Independent per-channel draws also shift
	// every later initializer's RNG stream, including map-sequence speed.
	p.color = ParticleCore::colorRandomSample (m_rng, min, max)
	    * instanceBirthRgbGain ();
	p.initial.color = p.color;
    };
}

InitializerFunc CParticle::createSizeRandomInitializer (const SizeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* sizeOverride = sizeOverrideValue ();

    return [this, minValue, maxValue, exponentValue, sizeOverride] (ParticleInstance& p) {
	float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	float exponent = exponentValue->getFloat ();
	float min = minValue->getFloat ();
	float max = maxValue->getFloat ();

	// Apply exponent for non-linear distribution
	float adjustedT = std::pow (t, exponent);
	const float sample = (min + adjustedT * (max - min))
	    * ((m_particle.flags & 0x80u) != 0 ? 1.0f : sizeOverride->getFloat ());
	p.size = m_slotStreams ? p.initial.size * sample : sample / 2.0f;
	p.initial.size = p.size;
    };
}

InitializerFunc CParticle::createAlphaRandomInitializer (const AlphaRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();
    DynamicValue* alphaOverride = alphaOverrideValue ();

    return [this, minValue, maxValue, exponentValue, alphaOverride] (ParticleInstance& p) {
	const float min = minValue->getFloat ();
	const float max = maxValue->getFloat ();
	const float exponent = exponentValue->getFloat ();
	const float instanceAlpha = m_slotStreams ? p.initial.alpha : alphaOverride->getFloat ();
	// Keep the established linear draw bit-for-bit when the authored exponent
	// is omitted. A shaped draw consumes the same single MT word.
	p.alpha = exponent == 1.0f
	    ? WallpaperEngine::Maths::randomFloat (m_rng, min, max) * instanceAlpha
	    : ParticleCore::alphaRandomExponentSample (
	        WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f), min, max, exponent,
	        instanceAlpha);
	p.initial.alpha = p.alpha;
    };
}

InitializerFunc CParticle::createLifetimeRandomInitializer (const LifetimeRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* lifetimeOverride = lifetimeOverrideValue ();

    return [this, minValue, maxValue, lifetimeOverride] (ParticleInstance& p) {
	p.lifetime = WallpaperEngine::Maths::randomFloat (m_rng, minValue->getFloat (), maxValue->getFloat ())
	    * lifetimeOverride->getFloat ();
	if (m_slotStreams && p.lifetime <= 0.001f) p.lifetime = 0.001f;
	p.initial.lifetime = p.lifetime;
    };
}

InitializerFunc CParticle::createVelocityRandomInitializer (const VelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();

    return [this, minValue, maxValue, speedOverride] (ParticleInstance& p) {
	glm::vec3 vel = WallpaperEngine::Maths::randomVec3 (m_rng, minValue->getVec3 (), maxValue->getVec3 ())
	    * speedOverride->getFloat ();
	// Native initializer opcode 7 transforms the sampled vector through the
	// emitter-selected control point's upper 3x3 (14023b340 -> 140184440).
	// Emitters set this basis before running their initializer stream. Reflect
	// authored Y first because particle SoA positions/velocities use that basis.
	p.velocity += m_birthInitializerBasis * ParticleCore::toSimulationVector (vel);
    };
}

InitializerFunc CParticle::createRotationRandomInitializer (const RotationRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();

    return [this, minValue, maxValue] (ParticleInstance& p) {
	const glm::vec3 sample = WallpaperEngine::Maths::randomVec3 (
	    m_rng, minValue->getVec3 (), maxValue->getVec3 ());
	for (int axis = 0; axis < 3; ++axis) {
	    ParticleCore::addAngularSample (p.rotation[axis], sample[axis]);
	}
    };
}

InitializerFunc CParticle::createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init) {
    DynamicValue* minValue = init.min->value.get ();
    DynamicValue* maxValue = init.max->value.get ();
    DynamicValue* exponentValue = init.exponent->value.get ();

    return [this, minValue, maxValue, exponentValue] (ParticleInstance& p) {
	glm::vec3 minVec = minValue->getVec3 ();
	glm::vec3 maxVec = maxValue->getVec3 ();
	float exponent = exponentValue->getFloat ();

	// Apply exponent bias to random distribution
	// exponent = 1: uniform distribution
	// exponent -> 0: bias towards max
	// exponent >= 2: bias towards min
	glm::vec3 result;
	for (int i = 0; i < 3; i++) {
	    float t = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
	    t = std::pow (t, exponent);
	    result[i] = minVec[i] + t * (maxVec[i] - minVec[i]);
	}

	for (int axis = 0; axis < 3; ++axis) {
	    ParticleCore::addAngularSample (p.angularVelocity[axis], result[axis]);
	}
    };
}

InitializerFunc CParticle::createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init) {
    DynamicValue* speedMin = init.speedMin->value.get ();
    DynamicValue* speedMax = init.speedMax->value.get ();
    DynamicValue* offsetVal = init.offset->value.get ();
    DynamicValue* scaleVal = init.scale->value.get ();
    DynamicValue* forwardVal = init.forward->value.get ();
    DynamicValue* timeScaleVal = init.timeScale->value.get ();
    DynamicValue* phaseMinVal = init.phaseMin->value.get ();
    DynamicValue* phaseMaxVal = init.phaseMax->value.get ();
    DynamicValue* rightVal = init.right->value.get ();
    // Native opcode-6 speed-range patches read the shared instance context
    // passed to static/event children (14022f890 -> 1401d15a0).
    DynamicValue* speedOverride = speedOverrideValue ();
    const auto& projection = getScene ().getScene ().camera.projection;
    const glm::vec2 sceneSpeedDefaults = ParticleCore::turbulentVelocitySpeedDefaults (
        projection.isOrthogonal);
    const bool speedMinDefault = init.speedMinDefault;
    const bool speedMaxDefault = init.speedMaxDefault;
    // Factory 1401c5490 omits opcode-6 speed-range patches for flag 0x10.
    const bool patchSpeedRange = (m_particle.flags & 0x10u) == 0;
    const auto* audioInitializer = &init;
    return [this, speedMin, speedMax, offsetVal, scaleVal, forwardVal,
            timeScaleVal, phaseMinVal, phaseMaxVal, rightVal, speedOverride,
            sceneSpeedDefaults, speedMinDefault, speedMaxDefault, patchSpeedRange,
            audioInitializer] (ParticleInstance& p) {
        // Native 14023bdc0–bf4a consumes phase then speed from the shared
        // scene stream. The noise rotates the authored forward vector around
        // authored right; neither the birth position nor a 3D curl is read.
        const float phaseRandom = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
        const float speedRandom = WallpaperEngine::Maths::randomFloat (m_rng, 0.0f, 1.0f);
        const float phaseMin = phaseMinVal->getFloat ();
        const auto audio = audioSettings (*audioInitializer);
        const float phaseGain = audio.mode != 0 ? sampleParticleAudio (*this, audio) : 1.0f;
        const glm::vec2 speedRange = ParticleCore::turbulentBirthSpeedRange (
            speedMinDefault ? sceneSpeedDefaults.x : speedMin->getFloat (),
            speedMaxDefault ? sceneSpeedDefaults.y : speedMax->getFloat (),
            speedOverride->getFloat (), patchSpeedRange);
        const float sceneTime = getScene ().getParticleSceneTime ();
        const auto velocity = Utils::nativeTurbulentBirthVelocity (
            phaseRandom, speedRandom, phaseMin, (phaseMaxVal->getFloat () - phaseMin) * phaseGain,
            speedRange.x, speedRange.y - speedRange.x, sceneTime,
            timeScaleVal->getFloat (), scaleVal->getFloat (), offsetVal->getFloat (),
            forwardVal->getVec3 (), rightVal->getVec3 ());
        if (!velocity) {
            sLog.error ("Particle turbulent birth outside bounded gradient domain: ", m_particle.name);
            return;
        }
        const glm::vec3 simulationVelocity = m_birthInitializerBasis
            * ParticleCore::toSimulationVector (*velocity);
        if (traceParticleChildren (*this)) {
            std::ostringstream trace;
            trace << std::setprecision (9)
                  << "Particle turbulent birth: object=" << m_particle.id
                  // Registration assigns birthId after all initializers run.
                  << " phaseRandom=" << phaseRandom
                  << " speedRandom=" << speedRandom << " scene=" << sceneTime
                  << " phaseGain=" << phaseGain
                  << " px=" << p.position.x << " py=" << p.position.y
                  << " pz=" << p.position.z
                  << " bx=" << m_birthInitializerBasis[0].x
                  << " by=" << m_birthInitializerBasis[1].y
                  << " vx=" << velocity->x << " vy=" << velocity->y
                  << " vz=" << velocity->z
                  << " svx=" << simulationVelocity.x << " svy=" << simulationVelocity.y
                  << " svz=" << simulationVelocity.z;
            sLog.out (trace.str ());
        }
        p.velocity += simulationVelocity;
    };
}

InitializerFunc
CParticle::createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init) {
    DynamicValue* controlPointValue = init.controlPoint->value.get ();
    DynamicValue* countValue = init.count->value.get ();
    DynamicValue* speedMinValue = init.speedMin->value.get ();
    DynamicValue* speedMaxValue = init.speedMax->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();
    auto counter = std::make_shared<SequenceCounter> ();
    counter->count = countValue;
    counter->flags = init.flags;
    const float rawCount = countValue->getFloat ();
    const float count = std::isfinite (rawCount) ? std::max (rawCount, 0.0001f) : 1.0f;
    counter->step = 1.0f / count;
    m_sequenceCounters.push_back (counter);

    const glm::vec2 bounds = init.bounds;
    const glm::vec3 axis = init.axis;
    const bool mirror = init.limitBehavior == "mirror";
    return [this, controlPointValue, speedMinValue, speedMaxValue, speedOverride,
            counter, bounds, axis, mirror] (ParticleInstance& p) {
        const float angle = ParticleCore::sequenceAngle (counter->phase, counter->step, bounds, mirror);
        const float rawControlPoint = controlPointValue->getFloat ();
        const int controlPoint = std::isfinite (rawControlPoint) && rawControlPoint >= 0.0f
            && rawControlPoint < static_cast<float> (std::numeric_limits<int>::max ())
            ? static_cast<int> (rawControlPoint) : -1;
        glm::vec3 center (0.0f);
        auto basis = ParticleCore::sequenceBasis (axis);
        if (controlPoint >= 0 && controlPoint < static_cast<int> (m_controlPoints.size ())) {
            const auto& cp = m_controlPoints[static_cast<size_t> (controlPoint)];
            center = cp.position;
            // Native 14023b340 transforms all three factory basis vectors
            // through the control-point matrix, retaining its roll and scale.
            basis.axis = cp.basis * basis.axis;
            basis.b = cp.basis * basis.b;
            basis.c = cp.basis * basis.c;
        }
        const auto orbit = ParticleCore::sequenceOrbitInBasis (p.position, center, basis, angle);
        p.position = orbit.position;
        const glm::vec3 low = speedMinValue->getVec3 ();
        const glm::vec3 high = speedMaxValue->getVec3 ();
        const glm::vec3 speed = ParticleCore::sequenceSpeed (low, high,
            [this] (float min, float max) { return WallpaperEngine::Maths::randomFloat (m_rng, min, max); });
        const glm::vec3 addedVelocity = orbit.tangent * speed.x
            + orbit.radial * speed.y + orbit.axis * speed.z;
        p.velocity += addedVelocity * speedOverride->getFloat ();
    };
}

// ========== OPERATORS ==========

void CParticle::setupOperators () {
    for (const auto& op : m_particle.operators) {
	if (!op) {
	    continue;
	}

	OperatorFunc func;

	if (op->is<InheritValueFromEventOperator> ()) {
            const auto& event = *op->as<InheritValueFromEventOperator> ();
            if (event.mode <= 1 || event.mode == 4 || event.mode == 5) m_resetColorEachPass = true;
            if (event.mode >= 2 && event.mode <= 5) m_resetAlphaEachPass = true;
            func = createInheritValueFromEventOperator (event);
        } else if (op->is<MaintainDistanceToControlPointOperator> ()) {
            func = createMaintainDistanceToControlPointOperator (*op->as<MaintainDistanceToControlPointOperator> ());
        } else if (op->is<MaintainDistanceBetweenControlPointsOperator> ()) {
            func = createMaintainDistanceBetweenControlPointsOperator (*op->as<MaintainDistanceBetweenControlPointsOperator> ());
        } else if (op->is<ReduceMovementNearControlPointOperator> ()) {
            func = createReduceMovementNearControlPointOperator (*op->as<ReduceMovementNearControlPointOperator> ());
        } else if (op->is<MovementOperator> ()) {
	    func = createMovementOperator (*op->as<MovementOperator> ());
	} else if (op->is<AngularMovementOperator> ()) {
	    func = createAngularMovementOperator (*op->as<AngularMovementOperator> ());
	} else if (op->is<CapVelocityOperator> ()) {
	    func = createCapVelocityOperator (*op->as<CapVelocityOperator> ());
	} else if (op->is<ScalarRemapValueOperator> ()) {
	    const auto& remap = *op->as<ScalarRemapValueOperator> ();
	    if (remap.output == ScalarRemapValueOperator::Output::Opacity)
		m_resetAlphaEachPass = true;
	    func = createScalarRemapValueOperator (remap);
	} else if (op->is<VectorRemapValueOperator> ()) {
	    const auto& remap = *op->as<VectorRemapValueOperator> ();
	    if (remap.output == VectorRemapValueOperator::Output::Color)
		m_resetColorEachPass = true;
	    func = createVectorRemapValueOperator (remap);
	} else if (op->is<AlphaFadeOperator> ()) {
	    m_resetAlphaEachPass = true;
	    func = createAlphaFadeOperator (*op->as<AlphaFadeOperator> ());
	} else if (op->is<SizeChangeOperator> ()) {
	    func = createSizeChangeOperator (*op->as<SizeChangeOperator> ());
	} else if (op->is<AlphaChangeOperator> ()) {
	    m_resetAlphaEachPass = true;
	    func = createAlphaChangeOperator (*op->as<AlphaChangeOperator> ());
	} else if (op->is<ColorChangeOperator> ()) {
	    m_resetColorEachPass = true;
	    func = createColorChangeOperator (*op->as<ColorChangeOperator> ());
	} else if (op->is<TurbulenceOperator> ()) {
	    func = createTurbulenceOperator (*op->as<TurbulenceOperator> ());
	} else if (op->is<VortexOperator> ()) {
	    func = createVortexOperator (*op->as<VortexOperator> ());
	} else if (op->is<ControlPointAttractOperator> ()) {
	    func = createControlPointAttractOperator (*op->as<ControlPointAttractOperator> ());
	} else if (op->is<OscillateAlphaOperator> ()) {
	    m_resetAlphaEachPass = true;
	    func = createOscillateAlphaOperator (*op->as<OscillateAlphaOperator> ());
	} else if (op->is<OscillateSizeOperator> ()) {
	    func = createOscillateSizeOperator (*op->as<OscillateSizeOperator> ());
	} else if (op->is<OscillatePositionOperator> ()) {
	    func = createOscillatePositionOperator (*op->as<OscillatePositionOperator> ());
	} else {
	    sLog.out ("Unknown operator type");
	}

	if (func) {
	    m_operators.push_back (std::move (func));
	}
    }
}

namespace {
size_t constraintControlPointIndex (const UserSettingUniquePtr& setting) {
    const float value = setting->value->getFloat ();
    if (!std::isfinite (value) || value < 0.0f || value >= 8.0f) return 7;
    return static_cast<size_t> (value);
}
ParticleCore::ConstraintControlPoint constraintPoint (const ControlPointData& cp) {
    return {cp.position, cp.previousPosition, cp.basis};
}
float constraintEnvelope (const ParticleOperatorBase& op, const ParticleInstance& p) {
    const auto envelope = operatorEnvelope (op.blendEnvelope ? &*op.blendEnvelope : nullptr);
    return envelope && ParticleCore::usesBlendOpcode (*envelope)
        ? ParticleCore::blendWeight (p.getLifetimePos (), *envelope) : 1.0f;
}
}

OperatorFunc CParticle::createMaintainDistanceToControlPointOperator (const MaintainDistanceToControlPointOperator& op) {
    return [&op] (std::vector<ParticleInstance>& particles, uint32_t count,
                  const std::vector<ControlPointData>& cps, float, ParticleCore::MovementTime time) {
        const auto index = constraintControlPointIndex (op.controlPoint);
        if (index >= cps.size ()) return;
        const auto cp = constraintPoint (cps[index]);
        for (uint32_t i = 0; i < count; ++i) {
            auto& p = particles[i];
            if (const auto result = ParticleCore::maintainControlPointDistance (
                    p.position, cp, op.distance->value->getFloat (),
                    op.variableStrength->value->getFloat (), time.integration, constraintEnvelope (op, p)))
                p.position = *result;
        }
    };
}

OperatorFunc CParticle::createMaintainDistanceBetweenControlPointsOperator (const MaintainDistanceBetweenControlPointsOperator& op) {
    return [&op] (std::vector<ParticleInstance>& particles, uint32_t count,
                  const std::vector<ControlPointData>& cps, float, ParticleCore::MovementTime) {
        const auto start = constraintControlPointIndex (op.controlPointStart);
        const auto end = constraintControlPointIndex (op.controlPointEnd);
        if (start >= cps.size () || end >= cps.size ()) return;
        for (uint32_t i = 0; i < count; ++i) {
            auto& p = particles[i];
            p.position = ParticleCore::maintainBetweenControlPoints (
                p.position, constraintPoint (cps[start]), constraintPoint (cps[end]), constraintEnvelope (op, p));
        }
    };
}

OperatorFunc CParticle::createReduceMovementNearControlPointOperator (const ReduceMovementNearControlPointOperator& op) {
    return [&op] (std::vector<ParticleInstance>& particles, uint32_t count,
                  const std::vector<ControlPointData>& cps, float, ParticleCore::MovementTime time) {
        const auto index = constraintControlPointIndex (op.controlPoint);
        if (index >= cps.size ()) return;
        for (uint32_t i = 0; i < count; ++i) {
            auto& p = particles[i];
            p.velocity *= ParticleCore::movementNearControlPointMultiplier (
                p.position, cps[index].position, op.distanceInner->value->getFloat (),
                op.distanceOuter->value->getFloat (), op.reductionInner->value->getFloat (),
                op.reductionOuter->value->getFloat (), time.integration, constraintEnvelope (op, p));
        }
    };
}

OperatorFunc CParticle::createInheritValueFromEventOperator (const InheritValueFromEventOperator& op) {
    return [this, &op] (std::vector<ParticleInstance>& particles, uint32_t count,
                       const std::vector<ControlPointData>&, float, ParticleCore::MovementTime) {
        const auto parent = parentEventValues ();
        const auto envelope = operatorEnvelope (op.blendEnvelope ? &*op.blendEnvelope : nullptr);
        const bool envelopeActive = envelope && ParticleCore::usesBlendOpcode (*envelope);
        for (uint32_t i = 0; i < count; ++i) {
            auto& p = particles[i];
            auto values = eventValues (p);
            if (ParticleCore::applyEventInheritance (values, parent,
                    static_cast<ParticleCore::EventInheritanceMode> (op.mode), constraintEnvelope (op, p), true, envelopeActive))
                applyEventValues (p, values, false);
        }
    };
}

OperatorFunc CParticle::createMovementOperator (const MovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* gravityValue = op.gravity->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();

    return [dragValue, gravityValue, speedOverride, nativeSlots = m_slotStreams.has_value ()] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       ParticleCore::MovementTime time
	   ) {
	float speed = speedOverride->getFloat ();
	float drag = dragValue->getFloat ();
	glm::vec3 gravity = gravityValue->getVec3 ();
	// Flip gravity Y for centered space
	gravity.y = -gravity.y;
	const glm::vec3 packedGravity = gravity * speed;

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!nativeSlots && !p.alive) {
		continue;
	    }

	    for (int axis = 0; axis < 3; ++axis) {
		ParticleCore::integrateAxis (p.position[axis], p.velocity[axis],
		                             packedGravity[axis], drag, time);
	    }
	}
    };
}

OperatorFunc CParticle::createAngularMovementOperator (const AngularMovementOperator& op) {
    DynamicValue* dragValue = op.drag->value.get ();
    DynamicValue* forceValue = op.force->value.get ();

    return [dragValue, forceValue] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       ParticleCore::MovementTime time
	   ) {
	float drag = dragValue->getFloat ();
	glm::vec3 force = forceValue->getVec3 ();

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }

	    for (int axis = 0; axis < 3; ++axis) {
		ParticleCore::integrateAngularAxis (
		    p.rotation[axis], p.angularVelocity[axis], force[axis], drag, time);
	    }
	}
    };
}

OperatorFunc CParticle::createCapVelocityOperator (const CapVelocityOperator& op) {
    DynamicValue* maxSpeedValue = op.maxSpeed->value.get ();
    const auto& projection = getScene ().getScene ().camera.projection;
    const float sceneDefault = ParticleCore::capVelocityDefault (projection.isOrthogonal);
    const bool useSceneDefault = op.useSceneDefault;
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    return [maxSpeedValue, sceneDefault, useSceneDefault, blend, nativeSlots = m_slotStreams.has_value ()] (
	std::vector<ParticleInstance>& particles, uint32_t count,
	const std::vector<ControlPointData>&, float, ParticleCore::MovementTime) {
	const float maxSpeed = useSceneDefault ? sceneDefault : maxSpeedValue->getFloat ();
	const auto envelope = operatorEnvelope (blend);
	for (uint32_t i = 0; i < count; ++i) {
	    auto& p = particles[i];
	    if (!nativeSlots && !p.alive) continue;
	    float factor;
            if (nativeSlots) {
                factor = ParticleCore::nativeRuntimeCapVelocityFactor (
                    p.velocity.x, p.velocity.y, p.velocity.z, maxSpeed,
                    ParticleCore::nativeRuntimeLifetimeFraction (p.age, p.lifetime), envelope);
            } else factor = ParticleCore::capVelocityFactor (
		p.velocity.x, p.velocity.y, p.velocity.z, maxSpeed, p.getLifetimePos (), envelope);
	    p.velocity *= factor;
	}
    };
}

OperatorFunc CParticle::createScalarRemapValueOperator (const ScalarRemapValueOperator& op, bool birth) {
    const bool hasRotationRandom = std::any_of (
	m_particle.initializers.begin (), m_particle.initializers.end (),
	[] (const auto& initializer) {
	    return initializer && initializer->template is<RotationRandomInitializer> ();
	});
    const bool hasAngularMovement = std::any_of (
	m_particle.operators.begin (), m_particle.operators.end (),
	[] (const auto& particleOperator) {
	    return particleOperator && particleOperator->template is<AngularMovementOperator> ();
	});
    const bool hasAngularVelocityRandom = std::any_of (
        m_particle.initializers.begin (), m_particle.initializers.end (),
        [] (const auto& initializer) {
            return initializer && initializer->template is<AngularVelocityRandomInitializer> ();
        });
    return createScalarRemapOperator (
        op, birth, hasRotationRandom, hasAngularMovement, hasAngularVelocityRandom, m_slotStreams.has_value ());
}

OperatorFunc CParticle::createVectorRemapValueOperator (const VectorRemapValueOperator& op, bool birth) {
    const bool hasRotationRandom = std::any_of (
        m_particle.initializers.begin (), m_particle.initializers.end (),
        [] (const auto& init) { return init && init->template is<RotationRandomInitializer> (); });
    const bool hasAngularMovement = std::any_of (
        m_particle.operators.begin (), m_particle.operators.end (),
        [] (const auto& item) { return item && item->template is<AngularMovementOperator> (); });
    const bool hasAngularVelocityRandom = std::any_of (
        m_particle.initializers.begin (), m_particle.initializers.end (),
        [] (const auto& init) { return init && init->template is<AngularVelocityRandomInitializer> (); });
    return createVectorRemapOperator (
        op, birth, hasRotationRandom, hasAngularMovement, hasAngularVelocityRandom, m_slotStreams.has_value ());
}

OperatorFunc CParticle::createAlphaFadeOperator (const AlphaFadeOperator& op) {
    // Native opcode 3 scales the current alpha stream, preserving earlier
    // operators in this tick (14023fbc0); the stream is reset before the tick.
    DynamicValue* fadeInTimeValue = op.fadeInTime->value.get ();
    DynamicValue* fadeOutTimeValue = op.fadeOutTime->value.get ();

    return
	[fadeInTimeValue, fadeOutTimeValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float fadeInTime = fadeInTimeValue->getFloat ();
	    float fadeOutTime = fadeOutTimeValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();

		if (life < fadeInTime) {
		    float fade = WallpaperEngine::Maths::fadeValue (life, 0.0f, fadeInTime, 0.0f, 1.0f);
		    p.alpha *= fade;
		} else if (life > fadeOutTime) {
		    float fade = 1.0f - WallpaperEngine::Maths::fadeValue (life, fadeOutTime, 1.0f, 0.0f, 1.0f);
		    p.alpha *= fade;
		}

	    }
	};
}

OperatorFunc CParticle::createSizeChangeOperator (const SizeChangeOperator& op) {
    // Native opcode 4 scales current size, so ordered size changes compose.
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();
    DynamicValue* sizeOverride = sizeOverrideValue ();

    return
	[this, startTimeValue, endTimeValue, startValueValue, endValueValue, sizeOverride] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float startTime = startTimeValue->getFloat ();
	    float endTime = endTimeValue->getFloat ();
	    // 1401c5490 binds both coefficients to the scene instance's size
	    // descriptor (+0xcc), in addition to the sizerandom binding.
	    const float instanceSize = (m_particle.flags & 0x80u) != 0
	        ? 1.0f : sizeOverride->getFloat ();
	    float startValue = startValueValue->getFloat () * instanceSize;
	    float endValue = endValueValue->getFloat () * instanceSize;

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();
		float multiplier = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue, endValue);
		p.size *= multiplier;

	    }
	};
}

OperatorFunc CParticle::createAlphaChangeOperator (const AlphaChangeOperator& op) {
    // Native opcode 6 scales current alpha, including preceding fade results.
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    return
	[startTimeValue, endTimeValue, startValueValue, endValueValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float startTime = startTimeValue->getFloat ();
	    float endTime = endTimeValue->getFloat ();
	    float startValue = startValueValue->getFloat ();
	    float endValue = endValueValue->getFloat ();

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();
		float multiplier = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue, endValue);
		p.alpha *= multiplier;

	    }
	};
}

OperatorFunc CParticle::createColorChangeOperator (const ColorChangeOperator& op) {
    DynamicValue* startTimeValue = op.startTime->value.get ();
    DynamicValue* endTimeValue = op.endTime->value.get ();
    DynamicValue* startValueValue = op.startValue->value.get ();
    DynamicValue* endValueValue = op.endValue->value.get ();

    return
	[this, startTimeValue, endTimeValue, startValueValue, endValueValue] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	float startTime = startTimeValue->getFloat ();
	float endTime = endTimeValue->getFloat ();
	glm::vec3 startValue = instanceTintedColorEndpoint (startValueValue->getVec3 ());
	glm::vec3 endValue = instanceTintedColorEndpoint (endValueValue->getVec3 ());

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];
		if (!p.alive) {
		    continue;
		}

		float life = p.getLifetimePos ();

		glm::vec3 color;
		color.r = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.r, endValue.r);
		color.g = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.g, endValue.g);
		color.b = WallpaperEngine::Maths::fadeValue (life, startTime, endTime, startValue.b, endValue.b);

		p.color = p.initial.color * color;
	    }
	};
}

OperatorFunc CParticle::createTurbulenceOperator (const TurbulenceOperator& op) {
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* speedMinValue = op.speedMin->value.get ();
    DynamicValue* speedMaxValue = op.speedMax->value.get ();
    DynamicValue* timeScaleValue = op.timeScale->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    return [this, scaleValue, speedMinValue, speedMaxValue, timeScaleValue,
            maskValue, phaseMinValue, phaseMaxValue, speedOverride, blend,
            audioOperator = &op, reportedUnbounded = false] (
               std::vector<ParticleInstance>& particles, uint32_t count,
               const std::vector<ControlPointData>&, float,
               ParticleCore::MovementTime time
           ) mutable {
        const auto settings = audioSettings (*audioOperator);
        const float audioGain = settings.mode != 0 ? sampleParticleAudio (*this, settings) : 1.0f;
        const float speedGain = audioGain * speedOverride->getFloat ();
        const float speedMin = speedMinValue->getFloat () * speedGain;
        const float speedDelta = (speedMaxValue->getFloat () - speedMinValue->getFloat ()) * speedGain;
        const float phaseMin = phaseMinValue->getFloat ();
        const float phaseDelta = phaseMaxValue->getFloat () - phaseMin;
        const float scale = scaleValue->getFloat ();
        const float timeScale = timeScaleValue->getFloat ();
        const glm::vec3 mask = maskValue->getVec3 ();
        const float sceneTime = getScene ().getParticleSceneTime ();
        const auto envelope = operatorEnvelope (blend);

        for (uint32_t i = 0; i < count; ++i) {
            ParticleInstance& p = particles[i];
            if (!p.alive) continue;
            const float envelopeWeight = envelope && ParticleCore::usesBlendOpcode (*envelope)
                ? ParticleCore::blendWeight (p.getLifetimePos (), *envelope) : 1.0f;
            // The native SoA streams are authored XYZ; Linux simulates with
            // reflected Y. Convert before simplex and reflect the final delta.
            const auto delta = Utils::nativeTurbulenceVelocityDelta (
                ParticleCore::toAuthoredVector (p.position), p.oscillatorRandom,
                phaseMin, phaseDelta, speedMin, speedDelta, scale, timeScale,
                sceneTime, mask, time.damping, envelopeWeight);
            if (!delta) {
                if (!reportedUnbounded) {
                    sLog.error ("Particle turbulence coordinate outside bounded simplex domain: ",
                                m_particle.name);
                    reportedUnbounded = true;
                }
                continue;
            }
            if (traceParticleChildren (*this)) {
                std::ostringstream trace;
                trace << std::setprecision (9)
                      << "Particle turbulence: object=" << m_particle.id
                      << " birth=" << p.birthId << " px=" << p.position.x
                      << " py=" << p.position.y << " pz=" << p.position.z
                      << " random=" << p.oscillatorRandom << " scene=" << sceneTime
                      << " q=" << time.damping << " age=" << p.age
                      << " lifetime=" << p.lifetime << " envelope=" << envelopeWeight
                      << " audio=" << audioGain
                      << " dvx=" << delta->x << " dvy=" << delta->y
                      << " dvz=" << delta->z;
                sLog.out (trace.str ());
            }
            p.velocity += ParticleCore::toSimulationVector (*delta);
        }
    };
}

OperatorFunc CParticle::createVortexOperator (const VortexOperator& op) {
    const bool v2 = op.variant == VortexOperator::Variant::VortexV2;
    const auto& projection = getScene ().getScene ().camera.projection;
    const auto sceneDefaults = ParticleCore::vortexDefaults (projection.isOrthogonal);
    const auto useSceneDefaults = op.sceneDefaults;
    int controlPoint = ParticleCore::vortexControlPointIndex (op.controlPoint);
    int flags = op.flags;
    DynamicValue* axisValue = op.axis->value.get ();
    DynamicValue* offsetValue = op.offset->value.get ();
    DynamicValue* distanceInnerValue = op.distanceInner->value.get ();
    DynamicValue* distanceOuterValue = op.distanceOuter->value.get ();
    DynamicValue* speedInnerValue = op.speedInner->value.get ();
    DynamicValue* speedOuterValue = op.speedOuter->value.get ();
    DynamicValue* centerForceValue = op.centerForce->value.get ();
    DynamicValue* ringRadiusValue = op.ringRadius->value.get ();
    DynamicValue* ringWidthValue = op.ringWidth->value.get ();
    DynamicValue* ringPullDistanceValue = op.ringPullDistance->value.get ();
    DynamicValue* ringPullForceValue = op.ringPullForce->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();
    const auto* blend = v2 && op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    // Extract flag bits
    bool infiniteAxis = (flags & 1) != 0;
    bool maintainDistance = v2 && (flags & 2) != 0;
    bool ringShape = v2 && (flags & 4) != 0;

    return [v2, sceneDefaults, useSceneDefaults, controlPoint, axisValue, offsetValue,
	    distanceInnerValue, distanceOuterValue, speedInnerValue,
	    speedOuterValue, centerForceValue, ringRadiusValue, ringWidthValue, ringPullDistanceValue,
	    ringPullForceValue, audioOperator = &op, this, infiniteAxis, maintainDistance, ringShape,
	    speedOverride, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float, ParticleCore::MovementTime time
	   ) {
	// The native vortex records scale speedinner and (speedouter - speedinner)
	// by the same response. Scaling both authored endpoints is equivalent.
	const auto settings = audioSettings (*audioOperator);
	const float audioGain = settings.mode != 0 ? sampleParticleAudio (*this, settings) : 1.0f;

	glm::vec3 axis = axisValue->getVec3 ();
	glm::vec3 offset = offsetValue->getVec3 ();
	// Both factories normalize the authored axis (or fall back to +X) before
	// v2 transforms it by the active control-point basis. Do not renormalize
	// afterward: the native interpreter consumes the transformed components.
	axis = ParticleCore::normalizedVortexAxis (axis);
	axis = ParticleCore::toSimulationVector (axis);
	float distanceInner = useSceneDefaults.distanceInner
	    ? sceneDefaults.distanceInner : distanceInnerValue->getFloat ();
	float distanceOuter = useSceneDefaults.distanceOuter
	    ? sceneDefaults.distanceOuter : distanceOuterValue->getFloat ();
	float speedInner = (useSceneDefaults.speedInner
	    ? sceneDefaults.speedInner : speedInnerValue->getFloat ()) * audioGain;
	float speedOuter = speedOuterValue->getFloat () * audioGain;
	float centerForce = centerForceValue->getFloat ();
	float ringRadius = ringRadiusValue->getFloat ();
	float ringWidth = ringWidthValue->getFloat ();
	float ringPullDistance = ringPullDistanceValue->getFloat ();
	float ringPullForce = ringPullForceValue->getFloat ();
	const auto envelope = operatorEnvelope (blend);

	// Get vortex center from control point
	glm::vec3 center = glm::vec3 (0.0f);
	if (controlPoint >= 0 && controlPoint < static_cast<int> (controlPoints.size ())) {
	    const auto& cp = controlPoints[controlPoint];
	    center = ParticleCore::vortexCenter (cp.position, offset, v2);
	    axis = ParticleCore::vortexAxis (axis, cp.basis, v2);
	} else {
	    center = v2 ? glm::vec3 (0.0f) : offset;
	}

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) {
		continue;
	    }
	    const float envelopeWeight = envelope && ParticleCore::usesBlendOpcode (*envelope)
		? ParticleCore::blendWeight (p.getLifetimePos (), *envelope) : 1.0f;

	    // Calculate vector from center to particle
	    glm::vec3 toParticle = p.position - center;

	    // For infinite axis mode, project onto plane perpendicular to axis (cylinder shape)
	    // Otherwise use full 3D distance (sphere shape)
	    float axialDistance = 0.0f;
	    glm::vec3 radialVector = toParticle;
	    if (infiniteAxis) {
		// Project out the axis component
		axialDistance = glm::dot (toParticle, axis);
		radialVector = toParticle - axis * axialDistance;
	    }

	    float distance = glm::length (radialVector);
	    glm::vec3 v2CenterCorrection (0.0f);
	    glm::vec3 predictedRadial (0.0f);
	    if (v2) {
		predictedRadial = toParticle + p.velocity * time.integration;
		if (infiniteAxis) predictedRadial -= axis * glm::dot (predictedRadial, axis);
	    }
	    if (v2 && maintainDistance) {
		v2CenterCorrection = ParticleCore::vortexV2RadialCorrection (
		    radialVector, predictedRadial, centerForce, time);
	    }

	    // Compute tangent direction (perpendicular to both axis and radial vector)
	    glm::vec3 tangent = ParticleCore::vortexTangent (radialVector, axis);
	    // A zero tangent does not suppress the v2 radial correction or ring pull.

	    // Calculate spin speed and apply forces based on mode
	    float speed = 0.0f;
	    glm::vec3 radialForce = glm::vec3 (0.0f);

	    if (!v2) {
		// Native opcode 0x0f packs both speed endpoints with the damping
		// clock, then clamps the radial distance fraction.
		speed = ParticleCore::vortexRadialVelocityScale (
		    distance, distanceInner, distanceOuter, speedInner, speedOuter, time);
	    } else if (ringShape) {
		const auto influence = ParticleCore::vortexV2RingInfluence (
		    distance, ringRadius, ringWidth, ringPullDistance);
		speed = glm::mix (speedInner, speedOuter, influence.speedInterpolation);
		radialForce = predictedRadial * influence.signedPull * ringPullForce;
	    } else {
		// The v2 non-ring record uses the same packed clamped distance
		// interpolation as v1, with a separate center correction below.
		speed = ParticleCore::vortexRadialSpeed (
		    distance, distanceInner, distanceOuter, speedInner, speedOuter);
	    }

	    // Apply tangential velocity (spinning)
	    p.velocity += tangent * speed * (v2 ? time.damping : 1.0f)
		* speedOverride->getFloat () * envelopeWeight;

	    // Apply radial force (ring pull)
	    p.velocity += radialForce * time.integration * envelopeWeight;
	    p.velocity += v2CenterCorrection * envelopeWeight;
	}
    };
}

OperatorFunc CParticle::createControlPointAttractOperator (const ControlPointAttractOperator& op) {
    int controlPoint = op.controlPoint;
    const uint32_t flags = op.flags;
    DynamicValue* originValue = op.origin->value.get ();
    DynamicValue* scaleValue = op.scale->value.get ();
    DynamicValue* thresholdValue = op.threshold->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();

    return [controlPoint, flags, originValue, scaleValue, thresholdValue, speedOverride] (
	       std::vector<ParticleInstance>& particles, uint32_t count,
	       const std::vector<ControlPointData>& controlPoints, float,
	       ParticleCore::MovementTime time
	   ) {
	// Get dynamic values
	glm::vec3 origin = originValue->getVec3 ();
	float scale = scaleValue->getFloat ();
	float threshold = thresholdValue->getFloat ();

	// Get control point position
	if (controlPoint < 0 || controlPoint >= static_cast<int> (controlPoints.size ())) {
	    return;
	}

	glm::vec3 center = controlPoints[controlPoint].position + origin;

	// Native 14023fbc0 opcode 0x0a applies linear radial falloff, and its
	// default flag 2 caps an inward impulse before crossing the control point.
	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];
	    if (!p.alive) continue;
	    p.velocity += ParticleCore::controlPointAttractVelocityDelta (
		p.position, center, scale * speedOverride->getFloat (), threshold, time, flags);
	}
    };
}

OperatorFunc CParticle::createOscillateAlphaOperator (const OscillateAlphaOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    return
	[this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue, blend] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float freqMin = freqMinValue->getFloat ();
	    float freqMax = freqMaxValue->getFloat ();
	    float scaleMin = scaleMinValue->getFloat ();
	    float scaleMax = scaleMaxValue->getFloat ();
	    float phaseMin = phaseMinValue->getFloat ();
	    float phaseMax = phaseMaxValue->getFloat ();
	    const auto envelope = operatorEnvelope (blend);

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];

		const float multiplier = ParticleCore::oscillatorMultiplier (
		    p.age, p.oscillatorRandom, freqMin, freqMax, phaseMin, phaseMax,
		    scaleMin, scaleMax, p.getLifetimePos (), envelope);
		p.alpha *= multiplier;
	    }
	};
}

OperatorFunc CParticle::createOscillateSizeOperator (const OscillateSizeOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    return
	[this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue, blend] (
	    std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float, float
	) {
	    float freqMin = freqMinValue->getFloat ();
	    float freqMax = freqMaxValue->getFloat ();
	    float scaleMin = scaleMinValue->getFloat ();
	    float scaleMax = scaleMaxValue->getFloat ();
	    float phaseMin = phaseMinValue->getFloat ();
	    float phaseMax = phaseMaxValue->getFloat ();
	    const auto envelope = operatorEnvelope (blend);

	    for (uint32_t i = 0; i < count; i++) {
		auto& p = particles[i];

		const float multiplier = ParticleCore::oscillatorMultiplier (
		    p.age, p.oscillatorRandom, freqMin, freqMax, phaseMin, phaseMax,
		    scaleMin, scaleMax, p.getLifetimePos (), envelope);
		p.size *= multiplier;
	    }
	};
}

OperatorFunc CParticle::createOscillatePositionOperator (const OscillatePositionOperator& op) {
    DynamicValue* freqMinValue = op.frequencyMin->value.get ();
    DynamicValue* freqMaxValue = op.frequencyMax->value.get ();
    DynamicValue* scaleMinValue = op.scaleMin->value.get ();
    DynamicValue* scaleMaxValue = op.scaleMax->value.get ();
    DynamicValue* phaseMinValue = op.phaseMin->value.get ();
    DynamicValue* phaseMaxValue = op.phaseMax->value.get ();
    DynamicValue* maskValue = op.mask->value.get ();
    DynamicValue* speedOverride = speedOverrideValue ();
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;

    return [this, freqMinValue, freqMaxValue, scaleMinValue, scaleMaxValue, phaseMinValue, phaseMaxValue, maskValue,
	    speedOverride, blend] (
	       std::vector<ParticleInstance>& particles, uint32_t count, const std::vector<ControlPointData>&, float,
	       float dt
	   ) {
	float freqMin = freqMinValue->getFloat ();
	float freqMax = freqMaxValue->getFloat ();
	float scaleMin = scaleMinValue->getFloat ();
	float scaleMax = scaleMaxValue->getFloat ();
	float phaseMin = phaseMinValue->getFloat ();
	float phaseMax = phaseMaxValue->getFloat ();
	glm::vec3 mask = maskValue->getVec3 ();
	const auto envelope = operatorEnvelope (blend);

	for (uint32_t i = 0; i < count; i++) {
	    auto& p = particles[i];

	    const float move = ParticleCore::positionOscillationDelta (
		p.age, dt, p.oscillatorRandom, freqMin, freqMax, phaseMin, phaseMax,
		scaleMin, scaleMax, speedOverride->getFloat (),
		envelope && ParticleCore::usesBlendOpcode (*envelope)
		    ? ParticleCore::blendWeight (p.getLifetimePos (), *envelope) : 1.0f);
	    p.position += mask * move;
	}
    };
}

// ========== RENDERING ==========

void CParticle::setupPass () {
    if (!m_particle.material || !m_particle.material->material || m_particle.material->material->passes.empty ()) {
	if (m_particle.children.empty ())
	    sLog.error ("No valid material for particle ", m_particle.name);
	return;
    }

    // Force texture 0 to use the input (particle texture) rather than the shader's
    // default "util/white" annotation, which would override it in setupRenderTexture()
    m_passBinds = { { 0, "previous" } };

    // Any pass may sample the current scene for refraction.
    m_hasRefract = std::any_of (
	m_particle.material->material->passes.begin (), m_particle.material->material->passes.end (),
	[] (const auto& pass) {
	    const auto it = pass->combos.find ("REFRACT");
	    return it != pass->combos.end () && it->second != 0;
	});

    // Create the FBO provider for CPass
    m_passFBOProvider = std::make_shared<FBOProvider> (this);

    // For REFRACT: create a copy FBO that shadows _rt_FullFrameBuffer.
    // The REFRACT shader reads g_Texture3 (= _rt_FullFrameBuffer) while we render TO the active target.
    // Reading from the same FBO being rendered to is undefined behavior in OpenGL, causing
    // black reads on NVIDIA. By placing a copy FBO with the same name in our FBOProvider,
    // CPass resolves g_Texture3 to the copy instead. Resize/blit from the
    // active target before each refracting pass, including composition scopes.
    if (m_hasRefract) {
	auto sceneFBO = getScene ().getFBO ();
	float w = static_cast<float> (sceneFBO->getRealWidth ());
	float h = static_cast<float> (sceneFBO->getRealHeight ());
	m_refractFBO = m_passFBOProvider->create (
	    "_rt_FullFrameBuffer", TextureFormat_ARGB8888, TextureFlags_ClampUVs, 1.0f, { w, h }, { w, h }
	);
    }

    // All material passes draw the same particle vertex stream, in authored
    // order. Attribute locations belong to each linked program, so each pass
    // needs its own VAO even though the VBO and EBO are shared.
    GLint prevVAO = 0;
    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &prevVAO);
    glGenBuffers (1, &m_vbo);
    glGenBuffers (1, &m_ebo);

    const size_t rendererCount = m_particle.renderers.empty () ? 1
        : m_mixedSpriteRopeRenderer || m_spriteRendererCount > 1
            ? m_particle.renderers.size () : 1;
    for (size_t renderer = 0; renderer < rendererCount; ++renderer) {
    const bool ropePass = m_mixedSpriteRopeRenderer
        ? (m_particle.renderers[renderer].name == "rope"
           || m_particle.renderers[renderer].name == "ropetrail") : m_useRopeRenderer;
    const bool trailPass = m_particle.renderers.empty () ? m_useTrailRenderer
        : m_particle.renderers[renderer].name == "spritetrail"
            || m_particle.renderers[renderer].name == "ropetrail";
    auto override = std::make_unique<ImageEffectPassOverride> ();
    override->combos["THICKFORMAT"] = 1;
    if (ropePass) override->shaderOverride = "genericropeparticle";
    if (m_spritesheetFrames > 0 && !m_separatePageAnimation) {
        // Native sets both atlas combos from the particle node, even when the
        // material omits them. Random-frame mode and authored flag 2 select
        // discrete tiles; ordinary sequences interpolate adjacent tiles.
        override->combos["SPRITESHEET"] = 1;
        override->combos["SPRITESHEETBLEND"] = particleAtlasBlendEnabled (
            m_particle.flags, m_particle.animationMode) ? 1 : 0;
    }
    if (trailPass) override->combos["TRAILRENDERER"] = 1;
    if (ropePass) override->combos["LINUX_CPU_ROPE_UV"] = 1;
    if (ropePass && trailPass) {
        const auto& trailRecord = m_particle.renderers[renderer];
        if (m_ropeTrailUVScrolling) override->combos["TRAILSCROLLALPHA"] = 1;
        if (trailRecord.fadeAlpha) override->combos["TRAILFADEALPHA"] = 1;
        if (trailRecord.fadeSize) override->combos["TRAILFADESIZE"] = 1;
    }
    m_passOverrides.push_back (std::move (override));
    m_rendererPassRanges.push_back ({m_passes.size (), m_particle.material->material->passes.size ()});
    for (const auto& materialPass : m_particle.material->material->passes) {
	auto pass = std::make_unique<Effects::CPass> (
	    *this, m_passFBOProvider, *materialPass, *m_passOverrides.back (), m_passBinds, std::nullopt);
	pass->setDestination (getScene ().getFBO ());
	pass->setInput (getTexture ());
	pass->setModelViewProjectionMatrix (&m_mvpMatrix);
	pass->setModelViewProjectionMatrixInverse (&m_mvpMatrixInverse);
	pass->setModelMatrix (&m_modelMatrix);
	pass->setViewProjectionMatrix (&m_viewProjectionMatrix);
	GLuint vao = 0;
	glGenVertexArrays (1, &vao);
	glBindVertexArray (vao);
	glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
	glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
	const GLuint program = pass->getProgramID ();

    if (ropePass) {
	// Rope vertex layout: native 26 floats plus CPU UV and right endpoints.
	// a_PositionVec4(4) + a_TexCoordVec4(4) + a_TexCoordVec4C1(4) + a_TexCoordVec4C2(4)
	// + a_TexCoordVec4C3(4) + a_TexCoordC4(2) + a_Color(4) = 26
	const GLsizei stride = sizeof (float) * ROPE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_PositionVec4");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C2");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordVec4C3");
	const GLint loc5 = glGetAttribLocation (program, "a_TexCoordC4");
	const GLint loc6 = glGetAttribLocation (program, "a_Color");
	const GLint loc7 = glGetAttribLocation (program, "a_CpuRopeUV");
	const GLint loc8 = glGetAttribLocation (program, "a_CpuRopeRightStart");
	const GLint loc9 = glGetAttribLocation (program, "a_CpuRopeRightEnd");
	const GLint loc10 = glGetAttribLocation (program, "a_CpuRopeEyeDirection");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 4));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 8));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 12));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 16));
	}
	if (loc5 >= 0) {
	    glEnableVertexAttribArray (loc5);
	    glVertexAttribPointer (loc5, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 20));
	}
	if (loc6 >= 0) {
	    glEnableVertexAttribArray (loc6);
	    glVertexAttribPointer (loc6, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 22));
	}
	if (loc7 >= 0) {
	    glEnableVertexAttribArray (loc7);
	    glVertexAttribPointer (loc7, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 26));
	}
	if (loc8 >= 0) {
	    glEnableVertexAttribArray (loc8);
	    glVertexAttribPointer (loc8, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 28));
	}
	if (loc9 >= 0) {
	    glEnableVertexAttribArray (loc9);
	    glVertexAttribPointer (loc9, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 31));
	}
	if (loc10 >= 0) {
	    glEnableVertexAttribArray (loc10);
	    glVertexAttribPointer (loc10, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 34));
	}
    } else {
	// Sprite vertex layout: 5 attributes, 17 floats/vertex, stride=68 bytes
	// a_Position(3) + a_TexCoordVec4(4) + a_Color(4) + a_TexCoordVec4C1(4) + a_TexCoordC2(2) = 17
	const GLsizei stride = sizeof (float) * SPRITE_FLOATS_PER_VERTEX;

	const GLint loc0 = glGetAttribLocation (program, "a_Position");
	const GLint loc1 = glGetAttribLocation (program, "a_TexCoordVec4");
	const GLint loc2 = glGetAttribLocation (program, "a_Color");
	const GLint loc3 = glGetAttribLocation (program, "a_TexCoordVec4C1");
	const GLint loc4 = glGetAttribLocation (program, "a_TexCoordC2");

	if (loc0 >= 0) {
	    glEnableVertexAttribArray (loc0);
	    glVertexAttribPointer (loc0, 3, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 0));
	}
	if (loc1 >= 0) {
	    glEnableVertexAttribArray (loc1);
	    glVertexAttribPointer (loc1, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 3));
	}
	if (loc2 >= 0) {
	    glEnableVertexAttribArray (loc2);
	    glVertexAttribPointer (loc2, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 7));
	}
	if (loc3 >= 0) {
	    glEnableVertexAttribArray (loc3);
	    glVertexAttribPointer (loc3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 11));
	}
	if (loc4 >= 0) {
	    glEnableVertexAttribArray (loc4);
	    glVertexAttribPointer (loc4, 2, GL_FLOAT, GL_FALSE, stride, (void*)(sizeof (float) * 15));
	}
    }

        setupGeometryCallbacks (*pass, vao);
        setupParticleUniforms (*pass);
        m_vaos.push_back (vao);
        m_passes.push_back (std::move (pass));
    }
    }
    glBindVertexArray (prevVAO);
}

void CParticle::setupGeometryCallbacks (Effects::CPass& pass, GLuint vao) {
    pass.setGeometryCallback (
	// Setup attribs: save current VAO, bind particle VAO
	[this, vao] () {
	    glGetIntegerv (GL_VERTEX_ARRAY_BINDING, &m_prevVAO);
	    glBindVertexArray (vao);
	},
	// Draw geometry: indexed rendering
	[this] () { glDrawElements (GL_TRIANGLES, m_activeIndexCount, GL_UNSIGNED_INT,
	                          reinterpret_cast<const void*> (m_activeIndexOffset * sizeof (uint32_t))); },
	// Cleanup: restore previous VAO
	[this] () { glBindVertexArray (m_prevVAO); }
    );
}

void CParticle::setupParticleUniforms (Effects::CPass& pass) {
    // Add particle-specific uniforms from common_particles.h that CPass doesn't provide
    // These are pointer-based: CPass reads the current value each frame
    pass.addUniform ("g_ModelMatrixInverse", &m_modelMatrixInverse);
    pass.addUniform ("g_OrientationUp", &m_orientationUp);
    pass.addUniform ("g_OrientationRight", &m_orientationRight);
    pass.addUniform ("g_OrientationForward", &m_orientationForward);
    pass.addUniform ("g_ViewUp", &m_viewUp);
    pass.addUniform ("g_ViewRight", &m_viewRight);
    pass.addUniform ("g_EyePosition", &m_eyePosition);
    pass.addUniform ("g_RenderVar0", &m_renderVar0);
    pass.addUniform ("g_RenderVar1", &m_renderVar1);

    // CPass binds g_RefractAmount from the shader annotation and material
    // constants. Adding a fixed particle uniform here would overwrite the
    // authored value after CPass::setupShaderVariables().
}

Wallpapers::ResolvedSceneTransform CParticle::resolveTransform () const {
    return Wallpapers::resolveSceneTransform (m_particle, [this] (int parentId) -> const Object* {
        const auto* parent = getScene ().getObject (parentId);
        return parent ? &parent->getObject () : nullptr;
    }, [this] (const Object& parent, const std::string& name) {
        return getScene ().getPuppetAttachmentTransform (parent.id, name);
    });
}

void CParticle::updateOrdinaryControlPoints () {
    const bool presetWorld = (m_particle.flags & 1u) != 0;
    const CParticle* instanceOwner = instanceOverrideOwner ();
    for (size_t index = 0; index < m_controlPoints.size (); ++index) {
        auto& cp = m_controlPoints[index];
        if (cp.linkMouse || (cp.flags & 0x10000u) != 0) continue;
        glm::mat4 authored (1.0f);
        const bool instanceOverrideAllowed = (cp.flags & 0x10005u) == 0;
        const glm::vec3 overridePosition = instanceOwner->getInstanceControlPoint (index);
        const glm::vec3 overrideAngle = instanceOwner->getInstanceControlPointAngle (index);
        const glm::vec3 angles = instanceOverrideAllowed
            && overrideAngle.x != std::numeric_limits<float>::max () ? overrideAngle : cp.angles;
        const glm::mat3 authoredBasis = ParticleCore::localControlPointBasis (angles);
        for (int axis = 0; axis < 3; ++axis)
            authored[axis] = glm::vec4 (authoredBasis[axis], 0.0f);
        authored[3] = glm::vec4 (
            ParticleCore::instanceControlPointPosition (cp.offset, overridePosition, cp.flags), 1.0f);
        // 14022a070/22bd40: choose the current stack, its inverse, or the
        // authored CP matrix. CP0 has a native world/world special case.
        const glm::mat4 mapped = Wallpapers::particleControlPointMatrix (
            authored, m_simulationModelMatrix,
            m_controlPointTransformInvertible ? std::optional<glm::mat4> (m_controlPointInverse)
                                             : std::nullopt,
            presetWorld, cp.worldSpace, index == 0,
            getScene ().getCamera ().isOrthogonal (), glm::vec2 (
                static_cast<float> (getScene ().getWidth ()),
                static_cast<float> (getScene ().getHeight ())));
        cp.position = glm::vec3 (mapped[3]);
        cp.basis = glm::mat3 (mapped);
    }
}

void CParticle::setChildAnchor (const glm::mat4& parentStack,
                                const glm::vec3& particlePosition, bool staticChild) {
    m_childSceneParentMatrix = parentStack;
    m_childParentPosition = particlePosition;
    m_childParentMatrix = glm::mat4 (1.0f);
    m_childReplacesSceneStack = !staticChild && (m_particle.flags & 1u) != 0;
    if (staticChild) return;

    // 14022a360: event node +3a0 gets B*T(particle)*descriptor. The child
    // preset bit selects 140229760's replacement or multiplication of the
    // stack; the parent's preset bit selects the coordinate basis for B.
    const bool parentWorld = m_parentParticleRuntime
        && (m_parentParticleRuntime->m_particle.flags & 1u) != 0;
    if (parentWorld && !m_childReplacesSceneStack) {
        const auto inverse = Wallpapers::inverseFiniteTransform (parentStack);
        if (inverse) m_childParentMatrix = *inverse;
        else if (!m_reportedParentCPTransformFailure) {
            sLog.error ("Particle event child parent transform is singular: ", m_particle.name);
            m_reportedParentCPTransformFailure = true;
        }
    } else if (!parentWorld && m_childReplacesSceneStack) {
        m_childParentMatrix = parentStack;
    }
}

void CParticle::updateMatrices () {
    const auto transform = resolveTransform ();
    const float screenWidth = static_cast<float> (getScene ().getWidth ());
    const float screenHeight = static_cast<float> (getScene ().getHeight ());
    const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    const bool perspective = !getScene ().getCamera ().isOrthogonal ();
    // Emitters keep reflected local coordinates. Native perspective object
    // transforms project authored world XYZ; only orthographic scenes center
    // and Y-flip the authored canvas.
    const glm::mat4 local = perspective && !m_hasChildParentMatrix
        ? transform.authoredMatrix * flip : flip * transform.authoredMatrix * flip;
    if (m_hasChildParentMatrix) {
        const glm::mat4 node = m_childParentMatrix
            * glm::translate (glm::mat4 (1.0f), m_childParentPosition) * local;
        m_modelMatrix = m_childReplacesSceneStack
            ? node : m_childSceneParentMatrix * node;
    } else {
        m_modelMatrix = perspective ? local
            : glm::translate (glm::mat4 (1.0f),
                glm::vec3 (-screenWidth / 2.0f, screenHeight / 2.0f, 0.0f)) * local;
    }
    m_simulationModelMatrix = m_modelMatrix;
    m_transformedOrigin = glm::vec3 (m_simulationModelMatrix[3]);
    // Preserve the former pre-parallax control-point conversion until the
    // native simulation/render-space split is measured.
    const auto simulationInverse = Wallpapers::inverseFiniteTransform (m_simulationModelMatrix);
    m_controlPointTransformInvertible = simulationInverse.has_value ();
    if (simulationInverse) m_controlPointInverse = *simulationInverse;
    // 1402366f0 substitutes scene+af0 for the own renderer when preset bit
    // 1 is set, then restores node+3a0 before recursively rendering children.
    // Linux's active scene/composition projection already maps centered world
    // coordinates into the target, so its world draw base is identity.
    if ((m_particle.flags & 1u) != 0)
        m_modelMatrix = perspective ? flip : glm::mat4 (1.0f);
    if (!m_hasChildParentMatrix) this->applyParallaxToModelMatrix ();
    m_modelMatrixInverse = Wallpapers::inverseFiniteTransform (m_modelMatrix).value_or (glm::mat4 (1.0f));

    this->updateParticleViewProjection ();
    m_mvpMatrix = m_viewProjectionMatrix * m_modelMatrix;
    m_mvpMatrixInverse = Wallpapers::inverseFiniteTransform (m_mvpMatrix).value_or (glm::mat4 (1.0f));

    // Native's default sprite basis is authored Y-down. The ordinary
    // orthographic model above reflects Y into Linux's centered GL space, so
    // reflect the basis as well while retaining the authored rotation and UV.
    // Fixed/upright renderers replace this basis below; trails use velocity.
    m_orientationUp = getScene ().getCamera ().isOrthogonal ()
        && (m_particle.flags & 4u) == 0
        ? glm::vec3 (0.0f, -1.0f, 0.0f) : glm::vec3 (0.0f, 1.0f, 0.0f);
    m_orientationRight = glm::vec3 (1.0f, 0.0f, 0.0f);
    m_orientationForward = glm::vec3 (0.0f, 0.0f, 1.0f);
    m_viewUp = glm::vec3 (0.0f, 1.0f, 0.0f);
    m_viewRight = glm::vec3 (1.0f, 0.0f, 0.0f);

    this->updateParticleRenderVars ();
}

void CParticle::applyParallaxToModelMatrix () {
    if (!getScene ().getScene ().camera.parallax.enabled->value->getBool ()
	|| getScene ().getContext ().getApp ().getContext ().settings.mouse.disableparallax) {
	return;
    }

    const glm::vec3 parallaxOffset = Wallpapers::sceneParticleParallaxOffset (
        m_particle.origin->value->getVec3 (), getScene ().getCamera ().getEye (),
        m_particle.parallaxDepth->value->getVec2 (), *getScene ().getParallaxDisplacement (),
        static_cast<float> (getScene ().getWidth ()), static_cast<float> (getScene ().getHeight ()),
        getScene ().getScene ().camera.parallax.amount->value->getFloat (),
        getScene ().getCamera ().isOrthogonal ());
    // Native translates the scene stack before dispatching this object's
    // draw. An unparented emitter's rotation or negative/nonuniform scale
    // must not rotate or magnify the camera movement (2334035201 fog/smoke).
    // Preserve the old authored-parent path until root inheritance is mapped.
    m_modelMatrix = m_particle.parent
        ? glm::translate (m_modelMatrix, parallaxOffset)
        : glm::translate (glm::mat4 (1.0f), parallaxOffset) * m_modelMatrix;
}

void CParticle::updateParticleViewProjection () {
    const auto& sceneCamera = getScene ().getCamera ();
    const auto perspectiveViewProjection = [&] {
        return Wallpapers::particlePerspectiveViewProjection (
            sceneCamera.getProjection (), sceneCamera.getLookAt (), (m_particle.flags & 1u) != 0);
    };
    if ((m_particle.flags & 4) != 0) {
	const auto& camera = getScene ().getCamera ();
	if (camera.isOrthogonal ()) {
	    // Native 1401891a0 selects perspectiveoverridefov for orthographic
	    // scenes (default 95 degrees), separately from perspective fov (50).
	    // 1401e5b60 then derives the flag-4 eye distance from that selected
	    // FOV and the active orthographic projection's Y scale.
	    const float fov = camera.getPerspectiveOverrideFov ();
	    const float eyeZ = ParticleCore::flag4OrthographicEyeDistance (
	        fov, camera.getProjection ()[1][1]);
	    m_viewProjectionMatrix = glm::perspective (
	        glm::radians (fov), camera.getWidth () / camera.getHeight (),
	        camera.getNearZ (), camera.getFarZ ())
	        * glm::translate (glm::mat4 (1.0f), glm::vec3 (0.0f, 0.0f, -eyeZ));
	    m_eyePosition = glm::vec3 (0.0f, 0.0f, eyeZ);
	} else {
	    m_viewProjectionMatrix = perspectiveViewProjection ();
	    m_eyePosition = camera.getEye ();
	}
    } else {
	// Orthographic projection from scene camera
	m_viewProjectionMatrix = getScene ().isChildCompositionScope ()
	    ? getScene ().getActiveRenderProjection ()
	    : sceneCamera.isOrthogonal ()
	        ? sceneCamera.getProjection () * sceneCamera.getRenderLookAt ()
	        : perspectiveViewProjection ();
	// Native 1401891a0 writes orthographic particle eye Z=2000 in authored
	// screen coordinates. Linux centers the camera XY, so (0,0,2000) is the
	// equivalent eye used by ComputeParticleTrailTangents. The flag-4 branch
	// above derives a separate eye distance from perspectiveoverridefov.
	m_eyePosition = sceneCamera.isOrthogonal ()
	    ? glm::vec3 (0.0f, 0.0f, 2000.0f) : sceneCamera.getEye ();
    }
    if (!getScene ().isChildCompositionScope ())
        m_viewProjectionMatrix = getScene ().getRootRenderClipTransform () * m_viewProjectionMatrix;
}

void CParticle::updateParticleRenderVars () {
    m_renderVar0 = glm::vec4 (m_trailLength, m_trailMaxLength, m_trailMinLength, 0.0f);
    if (m_useRopeRenderer && m_useTrailRenderer) {
	// Native 1402366f0 writes the normalized countdown phase to +0xb0,
	// consumed as g_RenderVar0.z by genericropeparticle.vert.
	const float phase = m_ropeTrailInterval > 0.0f
	    ? 1.0f - std::max (m_ropeTrailCountdown, 0.0f) / m_ropeTrailInterval : 0.0f;
	const float maxCount = m_ropeUVScrolling
	    ? ParticleCore::ropeTrailScrollMaxCount (m_ropeSegments, m_ropeUVScale)
	    : static_cast<float> (m_ropeSegments) - 0.5f;
	m_renderVar0 = glm::vec4 (static_cast<float> (m_ropeSegments - 1), 0.0f,
	                         phase, maxCount);
    }

    if (m_spritesheetFrames > 0 && !m_separatePageAnimation
        && m_spritesheetCols > 0 && m_spritesheetRows > 0) {
	float frameWidth = 1.0f / static_cast<float> (m_spritesheetCols);
	float frameHeight = 1.0f / static_cast<float> (m_spritesheetRows);
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    // Use atlas dimensions (from resolution vec4) for textureRatio, NOT getRealWidth/Height
	    // which returns per-frame dimensions for animated textures. The shader needs the
	    // per-frame pixel aspect ratio: (atlasH * frameHeight) / (atlasW * frameWidth).
	    const glm::vec4* res = texture->getResolution ();
	    float w = res->x; // atlas/GL texture width
	    float h = res->y; // atlas/GL texture height
	    if (w > 0.0f) {
		textureRatio = (h * frameHeight) / (w * frameWidth);
	    }
	}
	m_renderVar1 = glm::vec4 (frameWidth, frameHeight, static_cast<float> (m_spritesheetFrames), textureRatio);
    } else {
	// No spritesheet - texture ratio is height/width
	float textureRatio = 1.0f;
	if (const auto texture = getTexture ()) {
	    float w = static_cast<float> (texture->getRealWidth ());
	    float h = static_cast<float> (texture->getRealHeight ());
	    if (w > 0.0f) {
		textureRatio = h / w;
	    }
	}
	m_renderVar1 = glm::vec4 (0.0f, 0.0f, 0.0f, textureRatio);
    }
}

void CParticle::renderSprites (uint32_t rendererIndex) {
    if (m_particleCount == 0 || m_passes.empty ()) {
	return;
    }

    // Count alive particles
    uint32_t aliveCount = 0;
    for (uint32_t i = 0; i < m_particleCount; i++) {
	if (m_particles[i].alive) {
	    aliveCount++;
	}
    }

    if (aliveCount == 0) {
	return;
    }

    // Build vertex data in WP shader layout:
    // a_Position(3) + a_TexCoordVec4(uv.x, uv.y, rotZ, size)(4) + a_Color(4)
    //   + a_TexCoordVec4C1(vel.x, vel.y, vel.z, lifetime)(4) + a_TexCoordC2(rotX, rotY)(2) = 17 floats
    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;
    std::vector<uint32_t> drawnParticleIndices;
    if (m_separatePageAnimation) drawnParticleIndices.reserve (m_particleCount);

    for (uint32_t i = 0; i < m_particleCount; i++) {
	const auto& p = m_particles[i];
	if (!p.alive) {
	    continue;
	}

	// Skip particles with invalid values
	if (!std::isfinite (p.position.x) || !std::isfinite (p.position.y) || !std::isfinite (p.position.z)
	    || !std::isfinite (p.size) || p.size <= 0.0f || p.size > 10000.0f) {
	    continue;
	}
	if (m_separatePageAnimation) drawnParticleIndices.push_back (i);

	// Compute the lifetime value for the WP shader's ComputeSpriteFrame.
	// The shader computes: floor(frac(lifetime) * numFrames) to get current frame,
	// and frac(lifetime * numFrames) for the blend factor between frames.
	// For timed animation, encode the CPU-computed frame. Native random-frame
	// mode passes the birth random stream directly instead.
	float lifetime = m_slotStreams ? p.sequenceFraction : p.getLifetimePos ();

	if (m_spritesheetFrames > 0 && m_particle.animationMode == "randomframe") {
	    lifetime = ParticleCore::randomFrameLifetime (p.oscillatorRandom);
	} else if (m_spritesheetFrames > 0 && p.frame >= 0.0f) {
		// Encode frame index + fractional blend: shader reconstructs via
		// floor(lifetime * numFrames) = current frame,
		// frac(lifetime * numFrames) = blend toward next frame
		lifetime = p.frame / static_cast<float> (m_spritesheetFrames);
	}

	auto addVertex = [&] (float u, float v) {
	    const uint32_t base = vertexIndex * SPRITE_FLOATS_PER_VERTEX;
	    // a_Position (vec3)
	    m_vertices[base + 0] = p.position.x;
	    m_vertices[base + 1] = p.position.y;
	    m_vertices[base + 2] = p.position.z;
	    // a_TexCoordVec4 (vec4: uv.x, uv.y, rotZ, size)
	    m_vertices[base + 3] = u;
	    m_vertices[base + 4] = v;
	    m_vertices[base + 5] = p.rotation.z;
	    m_vertices[base + 6] = p.size;
	    // a_Color (vec4: r, g, b, a)
	    m_vertices[base + 7] = p.color.r;
	    m_vertices[base + 8] = p.color.g;
	    m_vertices[base + 9] = p.color.b;
	    m_vertices[base + 10] = p.alpha;
	    // a_TexCoordVec4C1 (vec4: vel.x, vel.y, vel.z, lifetime)
	    m_vertices[base + 11] = p.velocity.x;
	    m_vertices[base + 12] = p.velocity.y;
	    m_vertices[base + 13] = p.velocity.z;
	    m_vertices[base + 14] = lifetime;
	    // a_TexCoordC2 (vec2: rotX, rotY)
	    m_vertices[base + 15] = p.rotation.x;
	    m_vertices[base + 16] = p.rotation.y;
	    vertexIndex++;
	};

	// 4 vertices for quad corners
	uint32_t baseVertex = vertexIndex;
	addVertex (0.0f, 1.0f); // 0: Bottom-left
	addVertex (1.0f, 1.0f); // 1: Bottom-right
	addVertex (1.0f, 0.0f); // 2: Top-right
	addVertex (0.0f, 0.0f); // 3: Top-left

	// Native perspective GSOut uses the opposite triangle facing from the
	// orthographic GL quad after the particle-specific projection Y sign.
	const bool perspective = !getScene ().getCamera ().isOrthogonal ();
	m_indices[indexOffset++] = baseVertex + 0;
	m_indices[indexOffset++] = baseVertex + (perspective ? 2 : 1);
	m_indices[indexOffset++] = baseVertex + (perspective ? 1 : 2);
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + (perspective ? 0 : 3);
	m_indices[indexOffset++] = baseVertex + (perspective ? 3 : 0);
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    m_activeIndexOffset = 0;
    if (m_activeIndexCount == 0) {
	return;
    }

#if !NDEBUG
    std::string str = "Particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    // Upload vertex and index data
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * SPRITE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    // Update matrices and uniform data
    if (rendererIndex < m_particle.renderers.size ()
        && m_particle.renderers[rendererIndex].name == "spritetrail") {
        const auto& trail = m_particle.renderers[rendererIndex];
        m_trailLength = trail.length;
        m_trailMaxLength = trail.maxLength;
        m_trailMinLength = trail.minLength;
    }
    updateMatrices ();
    applyRendererOrientation (rendererIndex);
    if (m_separatePageAnimation) {
        const auto& frames = getTexture ()->getFrames ();
        const auto range = m_rendererPassRanges.at (m_activeRendererIndex);
        const auto resetPageDraw = [&] {
            for (size_t pass = 0; pass < range.count; ++pass)
                m_passes[range.first + pass]->setTextureFrameOverride (std::nullopt);
            m_activeIndexOffset = 0;
            m_activeIndexCount = static_cast<GLsizei> (indexOffset);
        };
        try {
	    for (size_t drawn = 0; drawn < drawnParticleIndices.size (); ++drawn) {
		const auto& particle = m_particles[drawnParticleIndices[drawn]];
		const float coordinate = m_particle.animationMode == "randomframe"
		    ? particle.oscillatorRandom * static_cast<float> (frames.size ())
		    : particle.frame;
		const uint32_t ordinal = m_particle.animationMode == "randomframe"
		    ? (std::isfinite (coordinate) && coordinate > 0.0f
		        ? static_cast<uint32_t> (std::min<double> (
		            coordinate, static_cast<double> (frames.size () - 1))) : 0u)
		    : std::min<uint32_t> (particle.frameOrdinal, frames.size () - 1);
		m_activeIndexOffset = drawn * 6;
		m_activeIndexCount = 6;
		for (size_t pass = 0; pass < range.count; ++pass)
		    m_passes[range.first + pass]->setTextureFrameOverride (ordinal);
		if (traceParticleChildren (*this))
		    sLog.out ("Particle page draw: particle=", getId (),
		              " renderer=", m_activeRendererIndex,
		              " birth=", particle.birthId,
		              " ordinal=", ordinal,
		              " page=", frames[ordinal]->frameNumber,
		              " position=", particle.position.x, ",", particle.position.y,
		              " color=", particle.color.r, ",", particle.color.g, ",", particle.color.b,
		              " age=", particle.age);
		drawMaterialPasses ();
	    }
        } catch (...) {
            resetPageDraw ();
            throw;
        }
        resetPageDraw ();
    } else {
        drawMaterialPasses ();
    }

#if !NDEBUG
    glPopDebugGroup ();
#endif
}

void CParticle::applyRendererOrientation (size_t rendererIndex) {
    if (rendererIndex < m_particle.renderers.size ()) {
        const auto& renderer = m_particle.renderers[rendererIndex];
        if (renderer.orientation == "fixed" || renderer.orientation == "upright") {
            const auto& camera = getScene ().getCamera ();
            std::optional<ParticleCore::FixedRendererBasis> basis;
            if (!m_hasChildParentMatrix || (m_particle.flags & 1u) == 0
                || !getScene ().isChildCompositionScope ()) {
                // 1402366f0 replaces the current scene matrix with scene+af0
                // before invoking 1402298b0 for preset bit 0. The Linux
                // draw model is that base; local presets retain the full
                // simulation stack for orientation as well as positions.
                const auto linear = glm::mat3 ((m_particle.flags & 1u) != 0
                    ? m_modelMatrix : m_simulationModelMatrix);
                if (renderer.orientation == "fixed") {
                    basis = ParticleCore::fixedRendererLocalBasis (
                        renderer.axis, linear, (renderer.flags & 1u) != 0);
                } else {
                    const auto forward = camera.getCenter () - camera.getEye ();
                    const auto cameraRight = glm::cross (forward, camera.getUp ());
                    const float length = glm::length (cameraRight);
                    if (std::isfinite (length) && length > 0.0f) {
                        // Native mode 0 writes -g_ViewRight as screen-right.
                        // Calibrate that field to Linux's existing +camera-right
                        // screen basis before applying mode 1's cross products.
                        const auto right = cameraRight / length;
                        // Orthographic scene stacks are Y-reflected on both
                        // sides. Perspective root stacks map reflected local
                        // particles directly into authored camera space.
                        const glm::vec3 reflectedNativeViewRight = camera.isOrthogonal ()
                            ? glm::vec3 (-right.x, right.y, -right.z) : -right;
                        basis = ParticleCore::uprightRendererLocalBasis (
                            renderer.axis, linear, reflectedNativeViewRight,
                            (renderer.flags & 1u) != 0,
                            camera.isOrthogonal () ? glm::vec3 (0.0f, -1.0f, 0.0f)
                                                   : glm::vec3 (0.0f, 1.0f, 0.0f));
                    }
                }
            }
            if (basis) {
                m_orientationRight = basis->right;
                m_orientationUp = basis->up;
                // Reflection reverses cross handedness: cross(FR,FU)
                // equals F(up_native x right_native), the native forward.
                const auto forward = glm::cross (basis->right, basis->up);
                const float length = glm::length (forward);
                if (std::isfinite (length) && length > 0.0f)
                    m_orientationForward = forward / length;
            } else if (!m_warnedFixedRendererTransform) {
                sLog.error ("Particle renderer orientation needs supported child draw base or valid scene stack: ",
                    m_particle.name, " renderer index=", rendererIndex,
                    " orientation=", renderer.orientation);
                m_warnedFixedRendererTransform = true;
            }
        }
    }
    if (traceParticleChildren (*this))
        sLog.out ("Particle renderer orientation: particle=", getId (),
            " index=", rendererIndex,
            " right=", m_orientationRight.x, ",", m_orientationRight.y, ",", m_orientationRight.z,
            " up=", m_orientationUp.x, ",", m_orientationUp.y, ",", m_orientationUp.z);

}

void CParticle::renderRope () {
    std::vector<ParticleInstance> orderedPoints;
    orderedPoints.reserve (m_ropeBirthSlots.size ());
    // Native 1402308a0 indexes an insertion-order slot list. Expiry removes
    // its slot from the list; a reused low SoA slot is appended at the tail.
    for (uint32_t slot : m_ropeBirthSlots) {
        const auto end = m_particles.begin () + m_particleCount;
        const auto particle = std::lower_bound (m_particles.begin (), end, slot,
            [] (const ParticleInstance& value, uint32_t wanted) {
                return value.poolSlot < wanted;
            });
        if (particle != end && particle->poolSlot == slot) orderedPoints.push_back (*particle);
    }
    if (traceParticleChildren (*this)) {
        std::ostringstream ordered;
        ordered << std::setprecision (9);
        for (size_t i = 0; i < orderedPoints.size (); ++i) {
            const auto& point = orderedPoints[i];
            ordered << '[' << i << ':' << point.poolSlot << ':' << point.birthId
                    << ':' << point.age << ':' << point.position.x << ','
                    << point.position.y << ',' << point.position.z << ']';
        }
        sLog.out ("Particle rope live state: particle=", getId (),
                  " renderer=", m_activeRendererIndex, " scene=", getScene ().getParticleSceneTime (),
                  " alive=", m_particleCount, " ordered=", ordered.str ());
    }
    renderRopePoints (orderedPoints, static_cast<uint32_t> (orderedPoints.size ()));
}

void CParticle::renderRopeTrail () {
    if (m_passes.empty ()) return;
    if (traceParticleChildren (*this)) {
        const float phase = m_ropeTrailInterval > 0.0f
            ? 1.0f - std::max (m_ropeTrailCountdown, 0.0f) / m_ropeTrailInterval : 0.0f;
        std::ostringstream history;
        history << std::setprecision (9);
        for (uint32_t particle = 0; particle < m_particleCount; ++particle) {
            const auto& head = m_particles[particle];
            history << '[' << head.birthId << ':' << m_ropeTrailHistory.counts[particle]
                    << ':' << head.position.x << ',' << head.position.y << ',' << head.position.z;
            for (int sample = 0; sample < m_ropeSegments; ++sample) {
                const auto& point = m_ropeTrailHistory.at (particle, static_cast<uint32_t> (sample));
                history << ';' << point.x << ',' << point.y << ',' << point.z;
            }
            history << ']';
        }
        sLog.out ("Particle rope trail state: particle=", getId (),
                  " renderer=", m_activeRendererIndex,
                  " alive=", m_particleCount,
                  " phase=", phase,
                  " samples=", m_ropeTrailHistory.counts.empty () ? 0
                      : m_ropeTrailHistory.counts[0],
                  " head=", m_particleCount ? m_particles[0].position.x : 0.0f,
                  ",", m_particleCount ? m_particles[0].position.y : 0.0f,
                  ",", m_particleCount ? m_particles[0].position.z : 0.0f,
                  " oldest=", m_particleCount
                      ? m_ropeTrailHistory.at (0, static_cast<uint32_t> (m_ropeSegments - 1)).x : 0.0f,
                  ",", m_particleCount
                      ? m_ropeTrailHistory.at (0, static_cast<uint32_t> (m_ropeSegments - 1)).y : 0.0f,
                  ",", m_particleCount
                      ? m_ropeTrailHistory.at (0, static_cast<uint32_t> (m_ropeSegments - 1)).z : 0.0f,
                  " full=", history.str ());
    }
    std::vector<ParticleInstance> points (static_cast<size_t> (m_ropeSegments) + 1);
    for (uint32_t particle = 0; particle < m_particleCount; ++particle) {
	const auto& source = m_particles[particle];
	points[0] = source; // live head; history slots trail behind it
	for (int sample = 0; sample < m_ropeSegments; ++sample) {
	    auto& point = points[static_cast<size_t> (sample) + 1];
	    point = source;
	    point.position = m_ropeTrailHistory.at (particle, static_cast<uint32_t> (sample));
	}
	// Native 1402308a0 multiplies this particle's capped sample count by
	// the renderer's reciprocal UV scale for in_ParticleTrailLength.
	const float trailLength = ParticleCore::ropeTrailUVLength (
	    m_ropeTrailHistory.counts[particle], m_ropeUVScale);
	renderRopePoints (points, static_cast<uint32_t> (points.size ()), trailLength);
    }
}

void CParticle::renderRopePoints (const std::vector<ParticleInstance>& points,
                                  uint32_t aliveCount, float nativeTrailLength) {
    if (aliveCount < 2 || m_passes.empty ()) {
	return;
    }

    // The native geometry shader forms its width in the active renderer's
    // model basis. Refresh the same matrices/orientation used by this draw
    // before expanding its original segments on the CPU.
    updateMatrices ();
    applyRendererOrientation (m_activeRendererIndex);

    // Ordinary rope receives live particles in spawn order. Rope trail receives
    // one particle's live head and its own history, newest first.
    // The native geometry shader uses smoothstep cubic Bezier subdivisions.
    //
    // Rope vertex layout (37 floats per vertex, THICKFORMAT):
    // [0-3]   a_PositionVec4:   startPos.xyz, sizeStart
    // [4-7]   a_TexCoordVec4:   endPos.xyz, trailLength
    // [8-11]  a_TexCoordVec4C1: CP0.xyz, trailPosition
    // [12-15] a_TexCoordVec4C2: CP1.xyz, sizeEnd
    // [16-19] a_TexCoordVec4C3: colorEnd.rgba
    // [20-21] a_TexCoordC4:     uvs.xy
    // [22-25] a_Color:          colorStart.rgba
    // [26-27] a_CpuRopeUV:      native segment UV endpoints after subdivision
    // [28-33] a_CpuRopeRightStart/End: sized original-segment rights,
    //         smoothstep-interpolated without normalizing interior vertices
    // [34-36] a_CpuRopeEyeDirection: original-segment model-space eye vector

    const uint32_t numSegments = aliveCount - 1;
    const int subdivision = std::max (1, m_ropeSubdivision);
    const float phase = m_ropeTrailInterval > 0.0f
        ? 1.0f - std::max (m_ropeTrailCountdown, 0.0f) / m_ropeTrailInterval : 0.0f;
    const bool scrollFade = m_useTrailRenderer && m_ropeUVScrolling;

    // First pass: evaluate spline to get all interpolated points
    const uint32_t totalPoints = numSegments * subdivision + 1;
    // Store position, size, color (rgba) per point = 3 + 1 + 4 = 8 floats
    std::vector<glm::vec3> splinePositions (totalPoints);
    std::vector<float> splineSizes (totalPoints);
    std::vector<glm::vec4> splineColors (totalPoints); // rgba
    struct SegmentRights { glm::vec3 start; glm::vec3 end; glm::vec3 eyeDirection; };
    std::vector<SegmentRights> segmentRights (numSegments);
    const glm::vec3 eyeModel = glm::vec3 (m_modelMatrixInverse
        * glm::vec4 (m_eyePosition, 1.0f));
    const glm::vec3 fixedEyeDirection = glm::mat3 (m_modelMatrixInverse) * m_orientationForward;
    const bool screenOrientation = m_activeRendererIndex >= m_particle.renderers.size ()
        || m_particle.renderers[m_activeRendererIndex].orientation == "screen";

    for (uint32_t i = 0; i < numSegments; i++) {
	const auto& p1 = points[i];
	const auto& p2 = points[i + 1];
	const auto& p0 = (i > 0) ? points[i - 1] : p1;
	const auto& p3 = (i + 2 < aliveCount) ? points[i + 2] : p2;
	float sizeStart = p1.size;
	float sizeEnd = p2.size;
	glm::vec4 colorStart (p1.color, p1.alpha);
	glm::vec4 colorEnd (p2.color, p2.alpha);
	if (scrollFade) {
	    const float fadeStart = ParticleCore::ropeTrailFade (i, m_ropeSegments, phase, false);
	    const float fadeEnd = ParticleCore::ropeTrailFade (i, m_ropeSegments, phase, true);
	    if (m_ropeTrailFadeAlpha) {
		colorStart.a *= fadeStart;
		colorEnd.a *= fadeEnd;
	    }
	    if (m_ropeTrailFadeSize) {
		sizeStart *= fadeStart;
		sizeEnd *= fadeEnd;
	    }
	}
	const glm::vec3 eyeVector = screenOrientation
	    ? (p1.position + p2.position) * 0.5f - eyeModel : fixedEyeDirection;
	const float eyeSquared = glm::dot (eyeVector, eyeVector);
	const glm::vec3 eyeDirection = screenOrientation && std::isfinite (eyeSquared)
	    && eyeSquared > 0.0f ? eyeVector / std::sqrt (eyeSquared) : eyeVector;
	segmentRights[i] = {
	    ParticleCore::ropeReflectedSizedRight (eyeDirection, p2.position - p0.position, sizeStart),
	    ParticleCore::ropeReflectedSizedRight (eyeDirection, p3.position - p1.position, sizeEnd),
	    eyeDirection,
	};

	for (int k = 0; k < subdivision; k++) {
	    float t = static_cast<float> (k) / static_cast<float> (subdivision);
	    uint32_t idx = i * subdivision + k;

	    const float smooth = t * t * (3.0f - 2.0f * t);
	    splinePositions[idx] = ParticleCore::ropeBezierPosition (
		p0.position, p1.position, p2.position, p3.position, t);
	    splineSizes[idx] = glm::mix (sizeStart, sizeEnd, smooth);
	    splineColors[idx] = glm::mix (colorStart, colorEnd, smooth);
	}
    }
    // Last point is the final particle
    {
	const auto& pLast = points[aliveCount - 1];
	splinePositions[totalPoints - 1] = pLast.position;
	splineSizes[totalPoints - 1] = pLast.size;
	splineColors[totalPoints - 1] = glm::vec4 (pLast.color, pLast.alpha);
	if (scrollFade) {
	    const float fade = ParticleCore::ropeTrailFade (
		numSegments - 1, m_ropeSegments, phase, true);
	    if (m_ropeTrailFadeAlpha) splineColors[totalPoints - 1].a *= fade;
	    if (m_ropeTrailFadeSize) splineSizes[totalPoints - 1] *= fade;
	}
    }

    // Second pass: build quads from consecutive spline points. Native rope
    // geometry subdivides each original segment and smoothstep-interpolates
    // its endpoint UVs; generated vertices do not advance the native index.
    uint32_t vertexIndex = 0;
    uint32_t indexOffset = 0;
    const uint32_t totalSubSegments = totalPoints - 1;
    const float uvScale = (std::isfinite (m_ropeUVScale) && m_ropeUVScale != 0.0f)
        ? m_ropeUVScale : 1.0f;
    // 1401c5490 retains the first nonzero lifetime midpoint and emitter
    // rate in the definition. 14022c3c0 copies them to node +0x1bc/+0x1c0;
    // 1402308a0 scales them by the instance lifetime/count settings.
    float definitionLifetime = 0.0f;
    for (const auto& initializer : m_particle.initializers) {
        if (initializer && initializer->is<LifetimeRandomInitializer> ()) {
            const auto& lifetime = *initializer->as<LifetimeRandomInitializer> ();
            if (definitionLifetime == 0.0f) {
                const float minimum = lifetime.min->value->getFloat ();
                const float maximum = lifetime.max->value->getFloat ();
                if (std::isfinite (minimum) && std::isfinite (maximum))
                    definitionLifetime = minimum + (maximum - minimum) * 0.5f;
            }
        }
    }
    float definitionRate = 0.0f;
    for (const auto& emitter : m_particle.emitters)
        if (definitionRate == 0.0f && std::isfinite (emitter.rate)) definitionRate = emitter.rate;
    const float lifetimeMultiplier = lifetimeOverrideValue ()->getFloat ();
    const float countMultiplier = (m_particle.flags & 0x20u) != 0
        ? 1.0f : countOverrideValue ()->getFloat ();
    const float effectiveLifetime = definitionLifetime * lifetimeMultiplier;
    const float effectiveRate = definitionRate * countMultiplier;
    const bool nativeRateCase = !m_useTrailRenderer
        && std::isfinite (effectiveLifetime) && effectiveLifetime > 0.0f
        && std::isfinite (effectiveRate) && effectiveRate > 0.0f;
    const auto ordinaryUV = m_useTrailRenderer ? ParticleCore::RopeOrdinaryUV {}
        : ParticleCore::ropeOrdinaryUV (
            aliveCount, m_maxParticles, nativeRateCase ? effectiveLifetime : 0.0f,
            nativeRateCase ? effectiveRate : 0.0f,
            points.front ().age, uvScale,
            nativeRateCase && m_ropeUVScrolling,
            nativeRateCase && m_ropeUVSmoothing,
            m_ropeExpiredCount);
    const float trailLength = m_useTrailRenderer ? nativeTrailLength : ordinaryUV.trailLength;

    for (uint32_t s = 0; s < totalSubSegments; s++) {
	const glm::vec3& posStart = splinePositions[s];
	const glm::vec3& posEnd = splinePositions[s + 1];
	float sizeStart = splineSizes[s];
	float sizeEnd = splineSizes[s + 1];
	const glm::vec4& colorStart = splineColors[s];
	const glm::vec4& colorEnd = splineColors[s + 1];

	// Neighboring points for shader tangent computation (CP0/CP1)
	const glm::vec3& posPrev = (s > 0) ? splinePositions[s - 1] : posStart;
	const glm::vec3& posAfter = (s + 2 < totalPoints) ? splinePositions[s + 2] : posEnd;
	const uint32_t originalSegment = s / static_cast<uint32_t> (subdivision);
	const uint32_t subsegment = s % static_cast<uint32_t> (subdivision);
	const auto& nativeRights = segmentRights[originalSegment];
	const float fractionStart = static_cast<float> (subsegment) / static_cast<float> (subdivision);
	const float fractionEnd = static_cast<float> (subsegment + 1) / static_cast<float> (subdivision);
	const glm::vec3 rightStart = ParticleCore::ropeInterpolatedRight (
	    nativeRights.start, nativeRights.end, fractionStart);
	const glm::vec3 rightEnd = ParticleCore::ropeInterpolatedRight (
	    nativeRights.start, nativeRights.end, fractionEnd);

	const float trailPosition = m_useTrailRenderer ? static_cast<float> (originalSegment)
	    : static_cast<float> (originalSegment) + ordinaryUV.positionOffset;
	float cpuUVStart = 0.0f;
	float cpuUVEnd = 0.0f;
	if (m_useTrailRenderer) {
	    const auto uv = ParticleCore::ropeTrailSegmentUV (
		originalSegment, trailLength,
		m_ropeUVScrolling
		    ? ParticleCore::ropeTrailScrollMaxCount (m_ropeSegments, m_ropeUVScale)
		    : static_cast<float> (m_ropeSegments) - 0.5f,
		phase, m_ropeUVScrolling);
	    cpuUVStart = ParticleCore::ropeTrailSubsegmentUV (
		uv, static_cast<float> (subsegment) / static_cast<float> (subdivision));
	    cpuUVEnd = ParticleCore::ropeTrailSubsegmentUV (
		uv, static_cast<float> (subsegment + 1) / static_cast<float> (subdivision));
	} else {
	    const auto uv = ParticleCore::ropeOrdinarySegmentUV (originalSegment, ordinaryUV);
	    cpuUVStart = ParticleCore::ropeTrailSubsegmentUV (
	        uv, static_cast<float> (subsegment) / static_cast<float> (subdivision));
	    cpuUVEnd = ParticleCore::ropeTrailSubsegmentUV (
	        uv, static_cast<float> (subsegment + 1) / static_cast<float> (subdivision));
	}

	auto addRopeVertex = [&] (float uvX, float uvY) {
	    const uint32_t base = vertexIndex * ROPE_FLOATS_PER_VERTEX;

	    // a_PositionVec4: startPos.xyz, sizeStart
	    m_vertices[base + 0] = posStart.x;
	    m_vertices[base + 1] = posStart.y;
	    m_vertices[base + 2] = posStart.z;
	    m_vertices[base + 3] = sizeStart;

	    // a_TexCoordVec4: endPos.xyz, trailLength
	    m_vertices[base + 4] = posEnd.x;
	    m_vertices[base + 5] = posEnd.y;
	    m_vertices[base + 6] = posEnd.z;
	    m_vertices[base + 7] = trailLength;

	    // a_TexCoordVec4C1: CP0.xyz (neighbor before start), trailPosition
	    m_vertices[base + 8] = posPrev.x;
	    m_vertices[base + 9] = posPrev.y;
	    m_vertices[base + 10] = posPrev.z;
	    m_vertices[base + 11] = trailPosition;

	    // a_TexCoordVec4C2: CP1.xyz (neighbor after end), sizeEnd
	    m_vertices[base + 12] = posAfter.x;
	    m_vertices[base + 13] = posAfter.y;
	    m_vertices[base + 14] = posAfter.z;
	    m_vertices[base + 15] = sizeEnd;

	    // a_TexCoordVec4C3: colorEnd.rgba
	    m_vertices[base + 16] = colorEnd.r;
	    m_vertices[base + 17] = colorEnd.g;
	    m_vertices[base + 18] = colorEnd.b;
	    m_vertices[base + 19] = colorEnd.a;

	    // a_TexCoordC4: uvs.xy
	    m_vertices[base + 20] = uvX;
	    m_vertices[base + 21] = uvY;

	    // a_Color: colorStart.rgba
	    m_vertices[base + 22] = colorStart.r;
	    m_vertices[base + 23] = colorStart.g;
	    m_vertices[base + 24] = colorStart.b;
	    m_vertices[base + 25] = colorStart.a;
	    m_vertices[base + 26] = cpuUVStart;
	    m_vertices[base + 27] = cpuUVEnd;
	    m_vertices[base + 28] = rightStart.x;
	    m_vertices[base + 29] = rightStart.y;
	    m_vertices[base + 30] = rightStart.z;
	    m_vertices[base + 31] = rightEnd.x;
	    m_vertices[base + 32] = rightEnd.y;
	    m_vertices[base + 33] = rightEnd.z;
	    m_vertices[base + 34] = nativeRights.eyeDirection.x;
	    m_vertices[base + 35] = nativeRights.eyeDirection.y;
	    m_vertices[base + 36] = nativeRights.eyeDirection.z;

	    vertexIndex++;
	};

	// Quad: 4 vertices (left/right at start/end of segment)
	uint32_t baseVertex = vertexIndex;
	addRopeVertex (0.0f, 0.0f); // left at start
	addRopeVertex (1.0f, 0.0f); // right at start
	addRopeVertex (1.0f, 1.0f); // right at end
	addRopeVertex (0.0f, 1.0f); // left at end

	// 2 triangles
	m_indices[indexOffset++] = baseVertex + 0;
	m_indices[indexOffset++] = baseVertex + 1;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 2;
	m_indices[indexOffset++] = baseVertex + 3;
	m_indices[indexOffset++] = baseVertex + 0;
    }

    m_activeIndexCount = static_cast<GLsizei> (indexOffset);
    m_activeIndexOffset = 0;
    if (m_activeIndexCount == 0) {
	return;
    }

    if (traceParticleChildren (*this)) {
        // Hash the exact UV vertex fields sent to GL, independent of vertex
        // positions and of the material pass. The fixed debug step makes the
        // scrolling phase reproducible across isolated/mixed captures.
        uint64_t uvHash = 1469598103934665603ull;
        for (uint32_t vertex = 0; vertex < vertexIndex; ++vertex) {
            const size_t base = static_cast<size_t> (vertex) * ROPE_FLOATS_PER_VERTEX;
            for (size_t offset : {7u, 11u, 20u, 21u, 26u, 27u,
                                  28u, 29u, 30u, 31u, 32u, 33u,
                                  34u, 35u, 36u}) {
                uint32_t bits = 0;
                std::memcpy (&bits, &m_vertices[base + offset], sizeof (bits));
                uvHash = (uvHash ^ bits) * 1099511628211ull;
            }
        }
        const size_t last = (static_cast<size_t> (vertexIndex) - 1) * ROPE_FLOATS_PER_VERTEX;
        sLog.out ("Particle rope UV geometry: particle=", getId (),
                  " renderer=", m_activeRendererIndex,
                  " trail=", m_useTrailRenderer,
                  " vertices=", vertexIndex,
                  " hash=", uvHash,
                  " first=", m_vertices[7], ",", m_vertices[11], ",",
                  m_vertices[20], ",", m_vertices[21], ",",
                  m_vertices[26], ",", m_vertices[27],
                  " rightfirst=", m_vertices[28], ",", m_vertices[29], ",", m_vertices[30], ",",
                  m_vertices[31], ",", m_vertices[32], ",", m_vertices[33],
                  " eye=", m_vertices[34], ",", m_vertices[35], ",", m_vertices[36],
                  " last=", m_vertices[last + 7], ",", m_vertices[last + 11], ",",
                  m_vertices[last + 20], ",", m_vertices[last + 21], ",",
                  m_vertices[last + 26], ",", m_vertices[last + 27],
                  " rightlast=", m_vertices[last + 28], ",", m_vertices[last + 29], ",",
                  m_vertices[last + 30], ",", m_vertices[last + 31], ",",
                  m_vertices[last + 32], ",", m_vertices[last + 33],
                  " eyelast=", m_vertices[last + 34], ",", m_vertices[last + 35], ",",
                  m_vertices[last + 36]);
    }

#if !NDEBUG
    std::string str = "Rope particles ";
    str += this->getParticle ().name + " (" + std::to_string (this->getId ()) + ", " + this->getParticle ().particleFile
	+ ")";
    glPushDebugGroup (GL_DEBUG_SOURCE_APPLICATION, 0, -1, str.c_str ());
#endif

    // Upload vertex and index data
    glBindBuffer (GL_ARRAY_BUFFER, m_vbo);
    glBufferData (
	GL_ARRAY_BUFFER, static_cast<GLsizeiptr> (vertexIndex * ROPE_FLOATS_PER_VERTEX * sizeof (float)),
	m_vertices.data (), GL_DYNAMIC_DRAW
    );

    glBindBuffer (GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData (
	GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr> (indexOffset * sizeof (uint32_t)), m_indices.data (),
	GL_DYNAMIC_DRAW
    );

    drawMaterialPasses ();

#if !NDEBUG
    glPopDebugGroup ();
#endif
}

void CParticle::drawMaterialPasses () {
    // The corrected root local perspective VP adds a clip-Y reflection.
    // Compensate only that presentation change, keeping authored negative
    // scales and the existing world-preset facing intact. Restore GL state
    // before another renderer, child particle, or scene object draws.
    ParticlePresentationFrontFace frontFace (
        Wallpapers::particlePresentationReversesWinding (
            getScene ().getCamera ().isOrthogonal (), (m_particle.flags & 1u) != 0,
            getScene ().isChildCompositionScope (), (m_particle.flags & 4u) != 0));
    // Keep the current depth-clamp workaround for sprite and rope geometry.
    glEnable (GL_DEPTH_CLAMP);
    const auto& authoredPasses = m_particle.material->material->passes;
    const auto range = m_rendererPassRanges.at (m_activeRendererIndex);
    for (size_t index = 0; index < range.count; ++index) {
        const auto& authored = *authoredPasses[index];
        const auto refract = authored.combos.find ("REFRACT");
        if (m_refractFBO && refract != authored.combos.end () && refract->second != 0) {
            // A later refracting pass observes earlier material passes. Copy
            // just before it draws to avoid sampling the active target itself.
            const auto activeFBO = getScene ().getActiveRenderTarget ();
            const GLint w = static_cast<GLint> (activeFBO->getRealWidth ());
            const GLint h = static_cast<GLint> (activeFBO->getRealHeight ());
            m_refractFBO->resize (activeFBO->getRealWidth (), activeFBO->getRealHeight (),
                                  activeFBO->getTextureWidth (0), activeFBO->getTextureHeight (0));
            GLint previousRead = 0;
            GLint previousDraw = 0;
            glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &previousRead);
            glGetIntegerv (GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
            glBindFramebuffer (GL_READ_FRAMEBUFFER, activeFBO->getFramebuffer ());
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, m_refractFBO->getFramebuffer ());
            glBlitFramebuffer (0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer (GL_READ_FRAMEBUFFER, static_cast<GLuint> (previousRead));
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, static_cast<GLuint> (previousDraw));
            if (traceParticleChildren (*this))
                sLog.out ("Particle refraction source: particle=", getId (),
                          " renderer=", m_activeRendererIndex,
                          " pass=", index,
                          " active=", activeFBO->getFramebuffer (),
                          " root=", getScene ().getFBO ()->getFramebuffer (),
                          " size=", w, "x", h);
        }
        auto& pass = *m_passes[range.first + index];
        pass.setDestination (getScene ().getActiveRenderTarget ());
        pass.render ();
        if (traceParticleChildren (*this))
            sLog.out ("Particle renderer pass: particle=", getId (),
                " renderer=", m_activeRendererIndex,
                " type=", m_particle.renderers.empty () ? "sprite"
                    : m_particle.renderers[m_activeRendererIndex].name,
                " pass=", index, " indices=", m_activeIndexCount,
                " uvscale=", m_ropeUVScale,
                " uvscroll=", m_ropeUVScrolling,
                " uvsmooth=", m_ropeUVSmoothing);
    }
    glDisable (GL_DEPTH_CLAMP);
}
