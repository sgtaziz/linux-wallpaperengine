#include "WallpaperEngine/Assets/AssetLocator.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Render/Camera.h"
#include "WallpaperEngine/Render/Objects/ParticleCore.h"
#include "WallpaperEngine/Render/Shaders/GLSLContext.h"
#include "WallpaperEngine/Render/Shaders/ParticleRefractionShader.h"
#include "WallpaperEngine/Render/Shaders/ShaderUnit.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {
using WallpaperEngine::Render::Shaders::particleTopDownRefractionSource;
namespace ParticleCore = WallpaperEngine::Render::Objects::ParticleCore;

// Refraction block of the shipped assets/shaders/genericparticle.frag (2.8.42,
// CRLF line endings as installed).
const std::string kShippedFragment =
    "#if REFRACT\r\n"
    "\tvec2 screenRefractionOffset = CAST2(0.0);\r\n"
    "\r\n"
    "#if NORMALMAP\r\n"
    "\tscreenRefractionOffset = v_ScreenTangents.xy * normal.x + v_ScreenTangents.zw * normal.y;\r\n"
    "#ifndef HLSL\r\n"
    "\tscreenRefractionOffset.y = -screenRefractionOffset.y;\r\n"
    "#endif\r\n"
    "\tscreenRefractionOffset *= normal.a * v_Color.a;\r\n"
    "#endif\r\n";

bool negatesOffsetY (const std::string& fragment) {
    return fragment.find ("screenRefractionOffset.y = -screenRefractionOffset.y;") != std::string::npos;
}

struct Tangents {
    glm::vec3 right, up;
};

// common_particles.h ComputeParticleTangents. glm and GLSL mat3 constructors
// are both column-major and Linux defines mul(x, y) as ((y) * (x)).
Tangents particleTangents (const glm::vec3& rotation, const glm::vec3& orientationRight,
                           const glm::vec3& orientationUp, const glm::vec3& orientationForward) {
    const glm::vec3 c = glm::cos (rotation), s = glm::sin (rotation);
    glm::mat3 m = glm::mat3 (1, 0, 0, 0, c.x, -s.x, 0, s.x, c.x) * glm::mat3 (c.z, -s.z, 0, s.z, c.z, 0, 0, 0, 1);
    m = glm::mat3 (c.y, 0, s.y, 0, 1, 0, -s.y, 0, c.y) * m;
    m = glm::mat3 (orientationRight, orientationUp, orientationForward) * m;
    return {m * glm::vec3 (1, 0, 0), m * glm::vec3 (0, 1, 0)};
}

// ComputeParticlePosition for one texel of a square sprite (texture ratio 1).
glm::vec3 spritePoint (const glm::vec3& centre, float size, const Tangents& t, glm::vec2 uv) {
    return centre + size * t.right * (uv.x - 0.5f) - size * t.up * (uv.y - 0.5f);
}

// ComputeScreenRefractionTangents followed by the fragment's normal-map
// offset, before the HLSL/GLSL-specific Y handling.
glm::vec2 refractionOffset (const Tangents& t, const glm::vec3& viewRight, const glm::vec3& viewUp,
                            glm::vec2 normal) {
    const glm::vec3 right = glm::normalize (t.right), up = glm::normalize (t.up);
    const glm::vec2 xy (glm::dot (right, viewRight), glm::dot (up, viewRight));
    const glm::vec2 zw (glm::dot (right, viewUp), glm::dot (up, viewUp));
    return xy * normal.x + zw * normal.y;
}

constexpr float kWidth = 3840.0f, kHeight = 2160.0f;
constexpr float kSize = 650.0f;

struct ScreenResult {
    glm::vec2 texel;  // texel displacement from the sprite centre, screen pixels, Y toward the bottom
    glm::vec2 sample; // refraction sample displacement, texcoord units, Y toward the bottom
    float winding;    // signed area of the sprite's (0,0),(1,0),(0,1) corners on screen
};

// Native D3D11: g_ModelViewProjectionMatrix of object 194's REFRACT draw from
// the 2.8.42 RenderDoc capture (positive Y scale: Y-up particle space, clip
// +Y at the top of the target). HLSL negates v_ScreenCoord.y and adds the
// offset to V unchanged; V grows toward the bottom.
ScreenResult nativeScreen (float theta, glm::vec2 uv, glm::vec2 normal) {
    glm::mat4 mvp (1.0f);
    mvp[0][0] = 0.00576f;
    mvp[3][0] = 0.02269f;
    mvp[1][1] = 0.01024f;
    mvp[3][1] = -0.06797f;
    mvp[2][2] = 0.002765f;
    mvp[3][2] = 0.5f;
    const glm::vec3 centre (40.0f, -20.0f, 0.0f);
    const Tangents t = particleTangents ({0, 0, theta}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1});
    const auto screen = [&] (glm::vec2 corner) {
        const glm::vec4 clip = mvp * glm::vec4 (spritePoint (centre, kSize, t, corner), 1.0f);
        return glm::vec2 ((clip.x / clip.w + 1.0f) * 0.5f * kWidth, (1.0f - clip.y / clip.w) * 0.5f * kHeight);
    };
    const glm::vec2 offset = refractionOffset (t, {1, 0, 0}, {0, 1, 0}, normal);
    const glm::vec2 a = screen ({0, 0}), b = screen ({1, 0}), c = screen ({0, 1});
    return {screen (uv) - screen ({0.5f, 0.5f}), offset,
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)};
}

// Linux GL: the ordinary orthographic particle stack is
// T(-w/2, h/2) * F * authored * F with F = diag(1,-1,1) over Y-reflected
// particle streams, projected by the scene's centred orthographic camera.
// The scene target is top-down: the final blit inverts it, so clip -Y and
// texcoord 0 are the top of the screen. The fragment adds the offset to the
// texcoord after its own (possibly patched) GLSL Y handling.
ScreenResult linuxScreen (float theta, glm::vec2 uv, glm::vec2 normal, const std::string& fragment,
                          const glm::vec3& viewUp) {
    const glm::mat4 flip = glm::scale (glm::mat4 (1.0f), glm::vec3 (1.0f, -1.0f, 1.0f));
    const glm::mat4 authored = glm::scale (glm::translate (glm::mat4 (1.0f), {1963.6f, 1006.6f, 0.0f}),
                                           glm::vec3 (11.06f));
    const glm::mat4 model = glm::translate (glm::mat4 (1.0f), {-kWidth / 2.0f, kHeight / 2.0f, 0.0f})
        * flip * authored * flip;
    const glm::mat4 projection = WallpaperEngine::Render::Camera::makeOrthogonalProjectionForScene (
        kWidth, kHeight, 0.01f, 10000.0f, glm::vec3 (0.0f));
    const glm::mat4 mvp = projection * model;
    const auto basis = ParticleCore::screenRendererBasis (true, 0u, !negatesOffsetY (fragment));
    const glm::vec3 centre (40.0f, 20.0f, 0.0f); // the native centre, Y-reflected
    const Tangents t = particleTangents ({0, 0, theta}, basis.right, basis.up, basis.forward);
    const auto screen = [&] (glm::vec2 corner) {
        const glm::vec4 clip = mvp * glm::vec4 (spritePoint (centre, kSize, t, corner), 1.0f);
        return glm::vec2 ((clip.x / clip.w + 1.0f) * 0.5f * kWidth, (clip.y / clip.w + 1.0f) * 0.5f * kHeight);
    };
    glm::vec2 offset = refractionOffset (t, basis.viewRight, viewUp, normal);
    if (negatesOffsetY (fragment)) offset.y = -offset.y;
    const glm::vec2 a = screen ({0, 0}), b = screen ({1, 0}), c = screen ({0, 1});
    return {screen (uv) - screen ({0.5f, 0.5f}), offset,
            (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)};
}

bool near (glm::vec2 a, glm::vec2 b, float tolerance) {
    return glm::length (a - b) <= tolerance * std::max (1.0f, glm::length (b));
}
} // namespace

TEST_CASE ("Top-down refraction drops only the shipped GL offset negation", "[particle][refract][shader]") {
    const auto patched = particleTopDownRefractionSource (kShippedFragment);
    REQUIRE (patched);
    REQUIRE_FALSE (negatesOffsetY (*patched));
    REQUIRE (patched->find (
        "screenRefractionOffset = v_ScreenTangents.xy * normal.x + v_ScreenTangents.zw * normal.y;")
        != std::string::npos);
    REQUIRE (patched->find ("screenRefractionOffset *= normal.a * v_Color.a;") != std::string::npos);
    REQUIRE (patched->size () + std::string ("screenRefractionOffset.y = -screenRefractionOffset.y;").size ()
             == kShippedFragment.size ());

    // Only the exact GLSL-only branch is accepted.
    const std::string unguarded = "\tscreenRefractionOffset.y = -screenRefractionOffset.y;\n";
    REQUIRE_FALSE (particleTopDownRefractionSource (unguarded));
    REQUIRE_FALSE (particleTopDownRefractionSource ("#ifdef HLSL\n" + unguarded + "#endif\n"));
    REQUIRE_FALSE (particleTopDownRefractionSource (kShippedFragment + kShippedFragment));
    REQUIRE_FALSE (particleTopDownRefractionSource ("void main() {}"));
}

TEST_CASE ("Only Y-reflected screen sprites pair with the top-down refraction fragment", "[particle][refract]") {
    // Native GS g_bufDynamic: up (0,1,0), view-up (0,1,0); Linux keeps F of them.
    const auto unpatched = ParticleCore::screenRendererBasis (true, 0u, false);
    REQUIRE (unpatched.up == glm::vec3 (0, -1, 0));
    REQUIRE (unpatched.viewUp == glm::vec3 (0, 1, 0));
    const auto patched = ParticleCore::screenRendererBasis (true, 0u, true);
    REQUIRE (patched.right == glm::vec3 (1, 0, 0));
    REQUIRE (patched.up == glm::vec3 (0, -1, 0));
    REQUIRE (patched.forward == glm::vec3 (0, 0, 1));
    REQUIRE (patched.viewRight == glm::vec3 (1, 0, 0));
    REQUIRE (patched.viewUp == glm::vec3 (0, -1, 0));
    // Perspective and flag-4 draws keep the unreflected basis unchanged.
    REQUIRE (ParticleCore::screenRendererBasis (false, 0u, true).viewUp == glm::vec3 (0, 1, 0));
    REQUIRE (ParticleCore::screenRendererBasis (true, 4u, true).up == glm::vec3 (0, 1, 0));
    REQUIRE (ParticleCore::screenRendererBasis (true, 4u, true).viewUp == glm::vec3 (0, 1, 0));

    REQUIRE (ParticleCore::topDownRefractionRenderer (true, 0u, false, false, false));
    REQUIRE (ParticleCore::topDownRefractionRenderer (true, 1u, false, false, false));
    REQUIRE_FALSE (ParticleCore::topDownRefractionRenderer (false, 0u, false, false, false));
    REQUIRE_FALSE (ParticleCore::topDownRefractionRenderer (true, 4u, false, false, false));
    REQUIRE_FALSE (ParticleCore::topDownRefractionRenderer (true, 0u, true, false, false));
    REQUIRE_FALSE (ParticleCore::topDownRefractionRenderer (true, 0u, false, true, false));
    REQUIRE_FALSE (ParticleCore::topDownRefractionRenderer (true, 0u, false, false, true));
}

TEST_CASE ("Rotating orthographic sprites refract like native on a top-down target", "[particle][refract]") {
    const auto patched = particleTopDownRefractionSource (kShippedFragment);
    REQUIRE (patched);
    const glm::vec3 patchedViewUp = ParticleCore::screenRendererBasis (true, 0u, true).viewUp;
    const glm::vec3 glViewUp = ParticleCore::screenRendererBasis (true, 0u, false).viewUp;
    const float pi = glm::pi<float> ();
    for (const float theta : {0.0f, pi / 4.0f, -pi / 4.0f, pi / 2.0f, -pi / 2.0f, pi}) {
        for (int step = 0; step < 8; ++step) {
            // A radial normal field: each texel's normal points along its
            // texture-space direction from the sprite centre (V down).
            const float phi = static_cast<float> (step) * pi / 4.0f;
            const glm::vec2 direction (std::cos (phi), std::sin (phi));
            const glm::vec2 uv = glm::vec2 (0.5f) + 0.3f * direction;
            const auto native = nativeScreen (theta, uv, direction);
            const auto gl = linuxScreen (theta, uv, direction, *patched, patchedViewUp);
            INFO ("theta " << theta << " phi " << phi);
            // On-screen sprite orientation, rotation sense and winding match.
            REQUIRE (near (gl.texel, native.texel, 1e-3f));
            REQUIRE ((gl.winding > 0.0f) == (native.winding > 0.0f));
            // Native keeps the refraction field radial at every rotation (UV offsets
            // against the isotropically scaled texel displacement).
            const glm::vec2 radial = glm::normalize (native.texel);
            const glm::vec2 nativeSample = glm::normalize (native.sample);
            REQUIRE (glm::dot (nativeSample, radial) > 0.999f);
            REQUIRE (near (gl.sample, native.sample, 1e-3f));
        }
    }

    // The unpatched GL fragment with the unreflected view-up matches native
    // only while the sprite is unrotated; at a quarter turn the field flips.
    const glm::vec2 direction (1.0f, 0.0f);
    const glm::vec2 uv (0.8f, 0.5f);
    const auto upright = linuxScreen (0.0f, uv, direction, kShippedFragment, glViewUp);
    REQUIRE (near (upright.sample, nativeScreen (0.0f, uv, direction).sample, 1e-3f));
    const auto quarter = linuxScreen (-pi / 2.0f, uv, direction, kShippedFragment, glViewUp);
    const auto nativeQuarter = nativeScreen (-pi / 2.0f, uv, direction);
    REQUIRE (glm::dot (glm::normalize (quarter.sample), glm::normalize (nativeQuarter.sample)) < -0.999f);
}

TEST_CASE ("Shipped genericparticle accepts the top-down refraction fragment", "[particle][refract][shader][shipped]") {
    using WallpaperEngine::Render::Shaders::GLSLContext;
    using WallpaperEngine::Render::Shaders::ShaderUnit;
    const char* root = std::getenv ("WPE_SHIPPED_ASSETS");
    if (!root) SKIP ("WPE_SHIPPED_ASSETS is not set; shipped genericparticle refraction not compiled");
    auto files = std::make_unique<WallpaperEngine::FileSystem::Container> ();
    files->mount (std::filesystem::path (root), "/");
    WallpaperEngine::Assets::AssetLocator assets (std::move (files));
    const std::string vertex = assets.vertexShader ("genericparticle");
    const std::string fragment = assets.fragmentShader ("genericparticle");
    REQUIRE (negatesOffsetY (fragment));
    const WallpaperEngine::Data::Model::ShaderConstantMap constants;
    const WallpaperEngine::Data::Model::TextureMap textures;
    const WallpaperEngine::Data::Model::ComboMap pass {{"REFRACT", 1}, {"NORMALMAP", 1}};
    for (const int topDown : {0, 1}) {
        WallpaperEngine::Data::Model::ComboMap overrides {{"THICKFORMAT", 1}};
        if (topDown) overrides["LINUX_TOPDOWN_REFRACTION"] = 1;
        ShaderUnit vertexUnit (GLSLContext::UnitType_Vertex, "genericparticle", vertex,
                               assets, constants, textures, textures, pass, overrides);
        ShaderUnit fragmentUnit (GLSLContext::UnitType_Fragment, "genericparticle", fragment,
                                 assets, constants, textures, textures, pass, overrides);
        INFO ("topDown " << topDown);
        REQUIRE (vertexUnit.usesTopDownRefraction () == false);
        REQUIRE (fragmentUnit.usesTopDownRefraction () == (topDown == 1));
        const auto translated = GLSLContext::get ().toGlsl (vertexUnit.compile (), fragmentUnit.compile ());
        REQUIRE_FALSE (translated.first.empty ());
        REQUIRE_FALSE (translated.second.empty ());
    }
}
