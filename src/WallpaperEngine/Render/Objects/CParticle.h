#pragma once

#include "CRenderable.h"
#include "ParticleCore.h"
#include "ParticleEventInheritance.h"
#include "WallpaperEngine/Data/Model/Object.h"
#include "WallpaperEngine/Render/Objects/Effects/CPass.h"
#include "WallpaperEngine/Render/Wallpapers/CScene.h"
#include "WallpaperEngine/Render/Wallpapers/SceneTransform.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

#include <functional>
#include <array>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

using namespace WallpaperEngine;
using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Model;

namespace WallpaperEngine::Render::Objects {

/**
 * Runtime particle instance state
 */
struct ParticleInstance {
    // Position and movement
    glm::vec3 position { 0.0f };
    glm::vec3 velocity { 0.0f };
    glm::vec3 acceleration { 0.0f };

    // Rotation
    glm::vec3 rotation { 0.0f };
    glm::vec3 angularVelocity { 0.0f };
    glm::vec3 angularAcceleration { 0.0f };

    // Visual properties
    glm::vec3 color { 1.0f };
    float alpha { 1.0f };
    float size { 20.0f };
    float frame { 0.0f }; // Current animation frame
    uint32_t frameOrdinal { 0 }; // Discrete authored page; the frame fraction may equal 1 at a boundary.

    // Lifetime
    float lifetime { 1.0f }; // Total lifetime in seconds
    float age { 0.0f }; // Current age in seconds

    // Shared native random stream component consumed by oscillators.
    float oscillatorRandom { 0.0f };

    // Initial values for resets/multipliers
    struct {
	glm::vec3 color { 1.0f };
	float alpha { 1.0f };
	float size { 20.0f };
	float lifetime { 1.0f };
    } initial;

    bool alive { false };
    uint32_t birthId { 0 }; // stable across order-preserving compaction
    uint32_t poolSlot { 0 }; // native high-water SoA index survives Linux compaction

    // Get normalized lifetime position (0.0 to 1.0)
    float getLifetimePos () const { return lifetime > 0.0f ? (age / lifetime) : 1.0f; }

    bool isAlive () const { return ParticleCore::isAlive (alive, age, lifetime); }
};

/**
 * Control point runtime data
 */
struct ControlPointData {
    glm::vec3 previousPosition { 0.0f };
    glm::vec3 position { 0.0f };
    glm::vec3 offset { 0.0f };
    glm::vec3 angles { 0.0f };
    glm::mat3 basis { 1.0f };
    uint32_t flags { 0 };
    bool linkMouse { false };
    bool worldSpace { false };
};

/**
 * Particle emitter function
 */
using EmitterFunc = std::function<void (std::vector<ParticleInstance>&, uint32_t&, float)>;

/**
 * Particle initializer function
 */
using InitializerFunc = std::function<void (ParticleInstance&)>;

/**
 * Particle operator function
 */
using OperatorFunc = std::function<
    void (std::vector<ParticleInstance>&, uint32_t, const std::vector<ControlPointData>&, float,
          ParticleCore::MovementTime)>;

class CParticle final : public CRenderable, public Scripting::ScriptableObject {
    friend CObject;

public:
    CParticle (Wallpapers::CScene& scene, const Particle& particle, uint32_t childDepth = 0,
               std::vector<std::string> ancestry = {}, CParticle* parentRuntime = nullptr);
    ~CParticle ();

    void setup () override;
    void render () override;
    void update (ParticleCore::TickClock clock);
    void emitParticles (int32_t count);
    void play ();
    void pause ();
    void stop ();
    [[nodiscard]] bool isPlaying () const;

    [[nodiscard]] const Particle& getParticle () const;
    [[nodiscard]] glm::vec3 getInstanceControlPoint (size_t index) const;
    void setInstanceControlPoint (size_t index, const glm::vec3& position);
    [[nodiscard]] glm::vec3 getInstanceControlPointAngle (size_t index) const;
    void setInstanceControlPointAngle (size_t index, const glm::vec3& angle);

    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

protected:
    void setupEmitters ();
    void setupInitializers ();
    void setupOperators ();

    // Emitter creators
    EmitterFunc createBoxEmitter (const ParticleEmitter& emitter, size_t index);
    EmitterFunc createSphereEmitter (const ParticleEmitter& emitter, size_t index);
    EmitterFunc createImageEmitter (const ParticleEmitter& emitter, size_t index);

    // Initializer creators
    InitializerFunc createColorRandomInitializer (const ColorRandomInitializer& init);
    InitializerFunc createHsvColorRandomInitializer (const HsvColorRandomInitializer& init);
    InitializerFunc createColorListInitializer (const ColorListInitializer& init);
    InitializerFunc createPositionOffsetRandomInitializer (const PositionOffsetRandomInitializer& init);
    InitializerFunc createMapSequenceBetweenControlPointsInitializer (
        const MapSequenceBetweenControlPointsInitializer& init);
    InitializerFunc createRemapInitialValueInitializer (const RemapInitialValueInitializer& init);
    InitializerFunc createSizeRandomInitializer (const SizeRandomInitializer& init);
    InitializerFunc createAlphaRandomInitializer (const AlphaRandomInitializer& init);
    InitializerFunc createLifetimeRandomInitializer (const LifetimeRandomInitializer& init);
    InitializerFunc createVelocityRandomInitializer (const VelocityRandomInitializer& init);
    InitializerFunc createRotationRandomInitializer (const RotationRandomInitializer& init);
    InitializerFunc createAngularVelocityRandomInitializer (const AngularVelocityRandomInitializer& init);
    InitializerFunc createTurbulentVelocityRandomInitializer (const TurbulentVelocityRandomInitializer& init);
    InitializerFunc
    createMapSequenceAroundControlPointInitializer (const MapSequenceAroundControlPointInitializer& init);

    InitializerFunc createInheritInitialValueFromEventInitializer (const InheritInitialValueFromEventInitializer& init);
    InitializerFunc createInheritControlPointVelocityInitializer (const InheritControlPointVelocityInitializer& init);
    [[nodiscard]] ParticleCore::EventParticleValues eventValues (const ParticleInstance& particle) const;
    [[nodiscard]] ParticleCore::EventParticleValues parentEventValues () const;
    void applyEventValues (ParticleInstance& particle, const ParticleCore::EventParticleValues& values, bool birth);
    OperatorFunc createInheritValueFromEventOperator (const InheritValueFromEventOperator& op);

    // Operator creators
    OperatorFunc createMaintainDistanceToControlPointOperator (const MaintainDistanceToControlPointOperator& op);
    OperatorFunc createMaintainDistanceBetweenControlPointsOperator (const MaintainDistanceBetweenControlPointsOperator& op);
    OperatorFunc createReduceMovementNearControlPointOperator (const ReduceMovementNearControlPointOperator& op);
    OperatorFunc createMovementOperator (const MovementOperator& op);
    OperatorFunc createAngularMovementOperator (const AngularMovementOperator& op);
    OperatorFunc createCapVelocityOperator (const CapVelocityOperator& op);
    OperatorFunc createScalarRemapValueOperator (const ScalarRemapValueOperator& op, bool birth = false);
    OperatorFunc createVectorRemapValueOperator (const VectorRemapValueOperator& op, bool birth = false);
    OperatorFunc createAlphaFadeOperator (const AlphaFadeOperator& op);
    OperatorFunc createSizeChangeOperator (const SizeChangeOperator& op);
    OperatorFunc createAlphaChangeOperator (const AlphaChangeOperator& op);
    OperatorFunc createColorChangeOperator (const ColorChangeOperator& op);
    OperatorFunc createTurbulenceOperator (const TurbulenceOperator& op);
    OperatorFunc createVortexOperator (const VortexOperator& op);
    OperatorFunc createControlPointAttractOperator (const ControlPointAttractOperator& op);
    OperatorFunc createOscillateAlphaOperator (const OscillateAlphaOperator& op);
    OperatorFunc createOscillateSizeOperator (const OscillateSizeOperator& op);
    OperatorFunc createOscillatePositionOperator (const OscillatePositionOperator& op);

    // Rendering
    void renderSprites (uint32_t rendererIndex);
    void applyRendererOrientation (size_t rendererIndex);
    void renderRope ();
    void renderRopeTrail ();
    void renderRopePoints (const std::vector<ParticleInstance>& points, uint32_t count,
                           float nativeTrailLength = 0.0f);
    void spawnChild (size_t descriptor, const ParticleInstance* parent, bool follow);
    void processChildEvents (const std::vector<ParticleInstance>& expired,
                             const std::vector<ParticleInstance>& born);
    void renderChildren ();
    void disableStaticEmittersRecursively ();
    [[nodiscard]] bool isFinishedForEvent () const;
    void inheritControlPointsFromParent (const CParticle& parent, const ParticleChild& descriptor);
    void setupPass ();
    void setupGeometryCallbacks (Effects::CPass& pass, GLuint vao);
    void setupParticleUniforms (Effects::CPass& pass);
    void drawMaterialPasses ();
    void updateMatrices ();
    void emitNewParticles (float dt);
    void resetPeriodicChildren ();
    void resetStaticEmitterTree ();
    void resetSequenceCounters (bool periodicOnly);
    void updateOrdinaryControlPoints ();
    void setChildAnchor (const glm::mat4& parentStack, const glm::vec3& particlePosition,
                         bool staticChild);
    [[nodiscard]] Wallpapers::ResolvedSceneTransform resolveTransform () const;
    void applyParallaxToModelMatrix ();
    void updateParticleViewProjection ();
    void updateParticleRenderVars ();

private:
    [[nodiscard]] const CParticle* instanceOverrideOwner () const;
    [[nodiscard]] DynamicValue* sharedInstanceOverrideValue (
        UserSettingUniquePtr ParticleInstanceOverride::* field) const;
    [[nodiscard]] DynamicValue* lifetimeOverrideValue () const;
    [[nodiscard]] DynamicValue* sizeOverrideValue () const;
    [[nodiscard]] DynamicValue* countOverrideValue () const;
    [[nodiscard]] DynamicValue* alphaOverrideValue () const;
    [[nodiscard]] DynamicValue* speedOverrideValue () const;
    [[nodiscard]] glm::vec3 instanceBirthRgbGain () const;
    [[nodiscard]] glm::vec3 instanceTintedColorEndpoint (const glm::vec3& authored) const;
    const Particle& m_particle;

    std::vector<ParticleInstance> m_particles;
    // Native ordinary-rope draw list (node+0x218): live SoA slots in birth order.
    std::vector<uint32_t> m_ropeBirthSlots;
    uint32_t m_ropeExpiredCount { 0 };
    bool m_hasOrdinaryRopeRenderer { false };
    uint32_t m_nextParticleBirthId { 1 };
    ParticleCore::NativeSlotAllocation m_nativeSlots;
    uint32_t m_childDepth { 0 };
    std::vector<std::string> m_childAncestry;
    CParticle* m_parentParticleRuntime { nullptr };
    // Native event consumers address the parent's allocated SoA slot, including
    // later slot reuse. Spatial event-follow remains tied to its birth identity.
    std::optional<uint32_t> m_eventParentSlot;
    std::vector<ParticleCore::EventParticleValues> m_eventSlotValues;
    bool m_emissionEnabled { true };
    bool m_paused { false };
    uint32_t m_forcedEmitCount { 0 };
    uint32_t m_pendingEmitCount { 0 };
    bool m_preTickedByParent { false };
    bool m_hasChildParentMatrix { false };
    // Native child node +3a0 and the scene matrix-stack entry below it are
    // distinct. Event children may replace the latter; static children do not.
    glm::mat4 m_childParentMatrix { 1.0f };
    glm::mat4 m_childSceneParentMatrix { 1.0f };
    bool m_childReplacesSceneStack { false };
    glm::vec3 m_childParentPosition { 0.0f };
    glm::mat4 m_simulationModelMatrix { 1.0f };
    struct ChildNode {
        ChildNode () = default;
        ChildNode (ChildNode&&) = default;
        ChildNode& operator= (ChildNode&& other) noexcept {
            if (this == &other) return *this;
            // A runtime's destroy hook may read thisLayer properties backed
            // by model. vector::erase/insert use move assignment, so release
            // the old runtime before replacing its model.
            runtime.reset ();
            model.reset ();
            descriptor = other.descriptor;
            parentBirthId = other.parentBirthId;
            follow = other.follow;
            detached = other.detached;
            model = std::move (other.model);
            runtime = std::move (other.runtime);
            return *this;
        }
        ChildNode (const ChildNode&) = delete;
        ChildNode& operator= (const ChildNode&) = delete;
        size_t descriptor { 0 };
        uint32_t parentBirthId { 0 };
        bool follow { false };
        bool detached { false };
        std::unique_ptr<Particle> model;
        std::unique_ptr<CParticle> runtime;
    };
    std::vector<ChildNode> m_childNodes;
    uint32_t m_particleCount { 0 };
    uint32_t m_maxParticles { 0 };

    std::vector<EmitterFunc> m_emitters;
    std::vector<uint8_t> m_emitterCanProduce;
    std::vector<InitializerFunc> m_initializers;
    glm::mat3 m_birthInitializerBasis { 1.0f };
    struct SequenceCounter {
        float phase { 0.0f };
        float step { 1.0f };
        uint32_t flags { 0 };
        DynamicValue* count { nullptr };
    };
    std::vector<std::shared_ptr<SequenceCounter>> m_sequenceCounters;
    std::vector<OperatorFunc> m_operators;
    bool m_resetAlphaEachPass { false };
    bool m_resetColorEachPass { false };

    std::vector<ControlPointData> m_controlPoints;
    // Native node overrides use FLT_MAX in X to leave an authored CP intact.
    std::array<glm::vec3, 8> m_instanceControlPointOverrides {};
    std::array<glm::vec3, 8> m_instanceControlPointAngleOverrides {};
    std::array<std::optional<glm::vec3>, 8> m_inheritedControlPointPositions {};
    bool m_reportedInheritedCPBasisMismatch { false };
    bool m_reportedParentCPTransformFailure { false };

    std::vector<float> m_vertices;
    std::vector<uint32_t> m_indices;

    double m_time { 0.0 };

    // CPass-based rendering
    std::vector<std::unique_ptr<Effects::CPass>> m_passes;
    std::vector<std::unique_ptr<ImageEffectPassOverride>> m_passOverrides;
    struct RendererPassRange { size_t first { 0 }; size_t count { 0 }; };
    std::vector<RendererPassRange> m_rendererPassRanges;
    size_t m_activeRendererIndex { 0 };
    std::shared_ptr<FBOProvider> m_passFBOProvider;
    TextureMap m_passBinds;
    GLsizei m_activeIndexCount { 0 };
    size_t m_activeIndexOffset { 0 };

    // REFRACT support: copy of scene FBO to avoid read-write conflict
    bool m_hasRefract { false };
    std::shared_ptr<CFBO> m_refractFBO;

    // OpenGL buffers
    std::vector<GLuint> m_vaos;
    GLuint m_vbo { 0 };
    GLuint m_ebo { 0 };
    GLint m_prevVAO { 0 };

    // Particle-specific uniform data (stored here, pointed to by CPass)
    glm::mat4 m_modelMatrix { 1.0f };
    glm::mat4 m_modelMatrixInverse { 1.0f };
    glm::mat4 m_controlPointInverse { 1.0f };
    bool m_controlPointTransformInvertible = true;
    glm::mat4 m_mvpMatrix { 1.0f };
    glm::mat4 m_mvpMatrixInverse { 1.0f };
    glm::mat4 m_viewProjectionMatrix { 1.0f };
    glm::vec3 m_orientationUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_orientationRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_orientationForward { 0.0f, 0.0f, 1.0f };
    glm::vec3 m_viewUp { 0.0f, 1.0f, 0.0f };
    glm::vec3 m_viewRight { 1.0f, 0.0f, 0.0f };
    glm::vec3 m_eyePosition { 0.0f, 0.0f, 1000.0f };
    glm::vec4 m_renderVar0 { 0.0f };
    glm::vec4 m_renderVar1 { 0.0f };

    // Spritesheet animation data
    int m_spritesheetCols { 0 };
    int m_spritesheetRows { 0 };
    int m_spritesheetFrames { 0 };
    int m_animationFrameCount { 0 };
    bool m_separatePageAnimation { false };
    float m_spritesheetDuration { 1.0f };
    bool m_authoredFrameTimeline { false };

    // Material shader constants
    float m_overbright { 1.0f };

    // Renderer configuration
    bool m_useTrailRenderer { false };
    bool m_hasRopeTrailHistory { false };
    float m_trailLength { 0.05f };
    float m_trailMaxLength { 10.0f };
    float m_trailMinLength { 0.0f };
    // Rope renderer (rope + ropetrail both use genericropeparticle shader)
    bool m_useRopeRenderer { false };
    uint32_t m_spriteRendererCount { 1 };
    bool m_mixedSpriteRopeRenderer { false };
    size_t m_ropeRendererIndex { 0 };
    size_t m_ropeTrailRendererIndex { 0 };
    std::vector<int> m_ropeRendererSubdivisions;
    bool m_warnedFixedRendererTransform { false };
    int m_ropeSubdivision { 4 }; // Catmull-Rom subdivisions between points (smoothing)
    int m_ropeSegments { 4 }; // ropetrail: historical position snapshots per particle
    ParticleCore::RopeTrailHistory m_ropeTrailHistory;
    float m_ropeTrailCountdown { 0.0f };
    float m_ropeTrailInterval { 0.0f };
    bool m_ropeTrailFadeAlpha { false };
    bool m_ropeTrailFadeSize { false };
    float m_ropeUVScale { 1.0f };
    bool m_ropeUVScrolling { false };
    bool m_ordinaryRopeUVScrolling { false };
    bool m_ordinaryRopeUVSmoothing { false };
    bool m_ropeTrailUVScrolling { false };
    bool m_ropeUVSmoothing { true }; // rope only

    // Per-vertex float counts for different renderer types
    static constexpr int SPRITE_FLOATS_PER_VERTEX = 17;
    static constexpr int ROPE_FLOATS_PER_VERTEX = 37;

    // Transformed origin (screen space to centered space conversion)
    glm::vec3 m_transformedOrigin { 0.0f };

    // Last known resolution for detecting changes
    float m_lastScreenWidth { 0.0f };
    float m_lastScreenHeight { 0.0f };

    // Random number generator
    std::mt19937& m_rng;

    bool m_initialized { false };
};
} // namespace WallpaperEngine::Render::Objects
