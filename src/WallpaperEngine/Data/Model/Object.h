#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "DynamicValue.h"
#include "DynamicModelData.h"
#include "Effect.h"
#include "Material.h"
#include "Model.h"
#include "Types.h"
#include "UserSetting.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"
#include <memory>

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct ObjectDependency {
    int id;
    int index;
    std::string type;
    std::string mask;
};

struct ObjectData {
    int id;
    std::string name;
    std::vector<int> dependencies;
    /** Typed links used by emitter-image/collision integrations; order IDs remain above. */
    std::vector<ObjectDependency> typedDependencies;
    std::optional<int> parent;
    /** Named attachment on the parent puppet, when authored. */
    std::optional<std::string> attachment;
    /** Authored hit-test participation for SceneScript cursor events. */
    bool solid = true;
    /** Authored stop flag for overlapping SceneScript cursor targets. */
    bool disablePropagation = false;
    /** The point of origin of the object */
    UserSettingUniquePtr origin;
    /** Transform fields for generic scene/group objects. Typed objects keep their own transform fields. */
    UserSettingUniquePtr groupScale;
    UserSettingUniquePtr groupAngles;
    UserSettingUniquePtr groupVisible;
    /** Detached authored configuration for SceneScript cloning; excludes only the layer ID. */
    std::string initialConfiguration;
};

/**
 * Base class for all objects, represents a single object in the scene
 *
 * @see Image
 * @see Sound
 * @see Particle
 * @see Text
 * @see Light
 */
class Object : public TypeCaster, public ObjectData {
public:
    explicit Object (ObjectData data) noexcept : TypeCaster (), ObjectData (std::move (data)) { };
    ~Object () override = default;
};

/**
 * Overrides effect's passes configuration
 *
 * @see ImageEffect
 * @see EffectPass
 */
struct ImageEffectPassOverride {
    int id;
    ComboMap combos;
    ShaderConstantMap constants;
    TextureMap textures;
    UserTextureMap usertextures;
    std::optional<std::string> shaderOverride; // Overrides MaterialPass::shader when set
};

/**
 * Override information for an specific effect
 *
 * @see ImageEffect
 * @see Effect
 * @see EffectPass
 * @see ImageEffectPass
 */
struct ImageEffect {
    /** Not sure what it's used for */
    int id;
    /** Effect's name for the editor */
    std::string name;
    /** If this effect is visible or not */
    UserSettingUniquePtr visible;
    /** Pass overrides to apply to the effect's passes */
    std::vector<ImageEffectPassOverrideUniquePtr> passOverrides;
    /** The effect definition */
    EffectUniquePtr effect;
};

/**
 * Animation layers for the puppet warp
 */
struct ImageAnimationLayer {
    int id;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr visible;
    UserSettingUniquePtr blend;
    UserSettingUniquePtr animation;
    bool additive = false;
    bool blendIn = false;
    bool blendOut = false;
    float blendTime = 0.5f;
};

struct ImageData {
    /** The scale of the image */
    UserSettingUniquePtr scale;
    /** The rotation of the image */
    UserSettingUniquePtr angles;
    /** If the image is visible or not */
    UserSettingUniquePtr visible;
    /** The alpha of the image */
    UserSettingUniquePtr alpha;
    /** The color of the image */
    UserSettingUniquePtr color;
    /** Native child-composition blend setting; may be scripted or user-bound. */
    UserSettingUniquePtr copyBackground;
    // TODO: WRITE A COUPLE OF ENUMS FOR THIS
    /** The alignment of the image */
    std::string alignment;
    /** Image dimensions, including live user and script updates. */
    UserSettingUniquePtr size;
    /** Parallax depth used for parallax scrolling */
    UserSettingUniquePtr parallaxDepth;
    /** The color blending mode for this image */
    UserSettingUniquePtr colorBlendMode;
    /** The brightness of the image */
    UserSettingUniquePtr brightness;
    /** The material in use for this image */
    ModelUniquePtr model;
    /** The effects applied to this image after the material is rendered */
    std::vector<ImageEffectUniquePtr> effects;
    /** The animation layers used in the puppet warp */
    std::vector<ImageAnimationLayerUniquePtr> animationLayers;
};

class Image : public Object, public ImageData {
public:
    explicit Image (ObjectData data, ImageData imageData) noexcept :
	Object (std::move (data)), ImageData (std::move (imageData)) { };
    ~Image () override = default;
};

/** A scene model references an MDLV mesh resource, unlike an image model JSON. */
struct SceneModelData {
    std::string path;
    int skin = 0;
    std::shared_ptr<DynamicModelData> dynamic;
};

class SceneModel : public Object, public SceneModelData {
public:
    explicit SceneModel (ObjectData data, SceneModelData modelData) noexcept :
        Object (std::move (data)), SceneModelData (std::move (modelData)) { };
    ~SceneModel () override = default;
};

/** Camera scene object; its pose and FOV can override the root scene camera. */
struct SceneCameraData {
    std::string mode;
    std::string path;
    UserSettingUniquePtr fov;
    UserSettingUniquePtr zoom;
};

class SceneCamera : public Object, public SceneCameraData {
public:
    explicit SceneCamera (ObjectData data, SceneCameraData cameraData) noexcept :
        Object (std::move (data)), SceneCameraData (std::move (cameraData)) { };
    ~SceneCamera () override = default;
};

/** The bounded scene-light route currently supports unshadowed point lights. */
struct ScenePointLightData {
    bool lightingV1 = false;
    UserSettingUniquePtr color;
    UserSettingUniquePtr intensity;
    UserSettingUniquePtr radius;
    UserSettingUniquePtr exponent;
};

class ScenePointLight : public Object, public ScenePointLightData {
public:
    explicit ScenePointLight (ObjectData data, ScenePointLightData lightData) noexcept :
        Object (std::move (data)), ScenePointLightData (std::move (lightData)) { };
    ~ScenePointLight () override = default;
};

/** Native unshadowed modern spot light (cookie/shadow fields remain represented). */
struct SceneSpotLightData {
    UserSettingUniquePtr color;
    UserSettingUniquePtr intensity;
    UserSettingUniquePtr radius;
    UserSettingUniquePtr exponent;
    UserSettingUniquePtr innerCone;
    UserSettingUniquePtr outerCone;
    UserSettingUniquePtr controlPoint;
    bool castShadow = false;
    bool useCookie = false;
};

class SceneSpotLight : public Object, public SceneSpotLightData {
public:
    explicit SceneSpotLight (ObjectData data, SceneSpotLightData lightData) noexcept :
        Object (std::move (data)), SceneSpotLightData (std::move (lightData)) { }
    ~SceneSpotLight () override = default;
};

struct SoundData {
    /** Native modes are pool, random, single; loop is a legacy Linux alias. */
    std::optional<std::string> playbackmode;
    std::vector<std::string> sounds;
    UserSettingUniquePtr volume;
    float minTime = 0.0f;
    float maxTime = 0.0f;
    bool startSilent = false;
    bool spatialization = false;
    float attenuation = 1.0f;
    float minDistance = 0.0f;
};

class Sound : public Object, public SoundData {
public:
    explicit Sound (ObjectData data, SoundData soundData) noexcept :
	Object (std::move (data)), SoundData (std::move (soundData)) { };
    ~Sound () override = default;
};

/**
 * Particle control points for forces and positions
 */
struct ParticleControlPoint {
    int id;
    uint32_t flags;
    int parentControlPoint;
    glm::vec3 offset;
    glm::vec3 angles;
    bool lockToPointer;
};

/**
 * Particle emitter configuration
 */
struct ParticleEmitter {
    int id;
    std::string name;
    glm::vec3 directions;
    glm::vec3 distanceMin;
    glm::vec3 distanceMax;
    bool distanceMaxAuthored;
    glm::vec3 origin;
    glm::vec3 offsetMin;
    glm::vec3 offsetMax;
    glm::ivec3 sign;
    uint32_t instantaneous;
    float speedMin;
    float speedMax;
    float rate;
    int controlPoint;
    uint32_t flags;
    float cone;
    float delay;
    float duration;
    glm::vec2 audioProcessingBounds;
    float audioProcessingExponent;
    int audioProcessingFrequencyStart;
    int audioProcessingFrequencyEnd;
    int audioProcessingMode;
    float minPeriodicDelay;
    float maxPeriodicDelay;
    float minPeriodicDuration;
    float maxPeriodicDuration;
    uint32_t maxToEmitPerPeriod;
};

/**
 * Particle initializer base and implementations
 */
class ParticleInitializerBase : public TypeCaster {
public:
    virtual ~ParticleInitializerBase () = default;
};

class InheritControlPointVelocityInitializer : public ParticleInitializerBase {
public:
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class InheritInitialValueFromEventInitializer : public ParticleInitializerBase {
public:
    uint32_t mode { 0 };
};

class ColorRandomInitializer : public ParticleInitializerBase {
public:
    ColorRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class HsvColorRandomInitializer : public ParticleInitializerBase {
public:
    UserSettingUniquePtr hueMin;
    UserSettingUniquePtr hueMax;
    UserSettingUniquePtr hueSteps;
    UserSettingUniquePtr saturationMin;
    UserSettingUniquePtr saturationMax;
    UserSettingUniquePtr valueMin;
    UserSettingUniquePtr valueMax;
};

class ColorListInitializer : public ParticleInitializerBase {
public:
    std::vector<UserSettingUniquePtr> colors;
    UserSettingUniquePtr hueNoise;
    UserSettingUniquePtr saturationNoise;
    UserSettingUniquePtr valueNoise;
};

class PositionOffsetRandomInitializer : public ParticleInitializerBase {
public:
    UserSettingUniquePtr scale;
    UserSettingUniquePtr distance;
    UserSettingUniquePtr timeScale;
    glm::vec3 directions { 1.0f };
    glm::vec3 sign { 0.0f };
    int octaves { 6 };
};

class MapSequenceBetweenControlPointsInitializer : public ParticleInitializerBase {
public:
    UserSettingUniquePtr controlPointStart;
    UserSettingUniquePtr controlPointEnd;
    UserSettingUniquePtr count;
    glm::vec2 bounds { 0.0f, 1.0f };
    std::string limitBehavior { "repeat" };
    uint32_t flags { 0 };
    float arcAmount { 0.3f };
    glm::vec3 arcDirection { 0.0f, 1.0f, 0.0f };
    float sizeReduction { 0.9f };
};

class SizeRandomInitializer : public ParticleInitializerBase {
public:
    SizeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class AlphaRandomInitializer : public ParticleInitializerBase {
public:
    AlphaRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent) :
	min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class LifetimeRandomInitializer : public ParticleInitializerBase {
public:
    LifetimeRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class VelocityRandomInitializer : public ParticleInitializerBase {
public:
    VelocityRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class RotationRandomInitializer : public ParticleInitializerBase {
public:
    RotationRandomInitializer (UserSettingUniquePtr min, UserSettingUniquePtr max) :
	min (std::move (min)), max (std::move (max)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
};

class AngularVelocityRandomInitializer : public ParticleInitializerBase {
public:
    AngularVelocityRandomInitializer (
	UserSettingUniquePtr min, UserSettingUniquePtr max, UserSettingUniquePtr exponent
    ) : min (std::move (min)), max (std::move (max)), exponent (std::move (exponent)) { }
    UserSettingUniquePtr min;
    UserSettingUniquePtr max;
    UserSettingUniquePtr exponent;
};

class TurbulentVelocityRandomInitializer : public ParticleInitializerBase {
public:
    TurbulentVelocityRandomInitializer (
	UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax, UserSettingUniquePtr scale,
	UserSettingUniquePtr offset, UserSettingUniquePtr forward, UserSettingUniquePtr timeScale,
	UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax, UserSettingUniquePtr right,
	UserSettingUniquePtr audioProcessingMode, UserSettingUniquePtr audioProcessingBounds,
	UserSettingUniquePtr audioProcessingExponent, UserSettingUniquePtr audioProcessingFrequencyStart,
	UserSettingUniquePtr audioProcessingFrequencyEnd, bool speedMinDefault, bool speedMaxDefault
    ) :
	speedMin (std::move (speedMin)), speedMax (std::move (speedMax)), scale (std::move (scale)),
	offset (std::move (offset)), forward (std::move (forward)), timeScale (std::move (timeScale)),
	phaseMin (std::move (phaseMin)), phaseMax (std::move (phaseMax)), right (std::move (right)),
	audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)),
	speedMinDefault (speedMinDefault), speedMaxDefault (speedMaxDefault) { }
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr forward;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr right;
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
    bool speedMinDefault;
    bool speedMaxDefault;
};

class MapSequenceAroundControlPointInitializer : public ParticleInitializerBase {
public:
    MapSequenceAroundControlPointInitializer (
        UserSettingUniquePtr controlPoint, UserSettingUniquePtr count, UserSettingUniquePtr speedMin,
        UserSettingUniquePtr speedMax, glm::vec2 bounds, glm::vec3 axis,
        std::string limitBehavior, uint32_t flags
    ) :
        controlPoint (std::move (controlPoint)), count (std::move (count)), speedMin (std::move (speedMin)),
        speedMax (std::move (speedMax)), bounds (bounds), axis (axis),
        limitBehavior (std::move (limitBehavior)), flags (flags) { }
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr count;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    glm::vec2 bounds;
    glm::vec3 axis;
    std::string limitBehavior;
    uint32_t flags;
};

using ParticleInitializerUniquePtr = std::unique_ptr<ParticleInitializerBase>;

/**
 * Particle operator base and implementations
 */
class ParticleOperatorBase : public TypeCaster {
public:
    struct BlendEnvelope {
	UserSettingUniquePtr inStart;
	UserSettingUniquePtr inEnd;
	UserSettingUniquePtr outStart;
	UserSettingUniquePtr outEnd;
    };

    // Native 2.8.42 packs this common 16-float envelope into eligible
    // operator records. An absent envelope selects the ordinary opcode.
    std::optional<BlendEnvelope> blendEnvelope;
    virtual ~ParticleOperatorBase () = default;
};

class InheritValueFromEventOperator : public ParticleOperatorBase {
public:
    uint32_t mode { 4 };
};

class MaintainDistanceToControlPointOperator : public ParticleOperatorBase {
public:
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr distance;
    UserSettingUniquePtr variableStrength;
};

class MaintainDistanceBetweenControlPointsOperator : public ParticleOperatorBase {
public:
    UserSettingUniquePtr controlPointStart;
    UserSettingUniquePtr controlPointEnd;
};

class ReduceMovementNearControlPointOperator : public ParticleOperatorBase {
public:
    UserSettingUniquePtr controlPoint;
    UserSettingUniquePtr distanceInner;
    UserSettingUniquePtr distanceOuter;
    UserSettingUniquePtr reductionInner;
    UserSettingUniquePtr reductionOuter;
};

class MovementOperator : public ParticleOperatorBase {
public:
    MovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr gravity) :
	drag (std::move (drag)), gravity (std::move (gravity)) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr gravity;
};

class AngularMovementOperator : public ParticleOperatorBase {
public:
    AngularMovementOperator (UserSettingUniquePtr drag, UserSettingUniquePtr force) :
	drag (std::move (drag)), force (std::move (force)) { }
    UserSettingUniquePtr drag;
    UserSettingUniquePtr force;
};

class CapVelocityOperator : public ParticleOperatorBase {
public:
    CapVelocityOperator (UserSettingUniquePtr maxSpeed, bool useSceneDefault) :
	maxSpeed (std::move (maxSpeed)), useSceneDefault (useSceneDefault) { }
    UserSettingUniquePtr maxSpeed;
    bool useSceneDefault;
};

// Native remapvalue opcode 0x13 has many selector/transform branches. This
// typed record represents scalar lifetimefraction→size/opacity multiply;
// vector selectors and transform functions still require separate paths.
class ScalarRemapValueOperator : public ParticleOperatorBase {
public:
    enum class Input { LifetimeFraction, MaxLifetime, Size, Opacity, Speed,
                       Rotation, AngularSpeed, DistanceToControlPoint,
                       PositionBetweenTwoControlPoints,
                       ControlPoint, DeltaToControlPoint, DirectionToControlPoint,
                       Color, Position, Velocity };
    enum class InputComponent { All, X, Y, Z, Sum, Average, Max, Min };
    enum class Output { Size, Opacity, Speed, MaxLifetime, Rotation, AngularSpeed };
    enum class Operation { Set, Multiply, Add, Subtract };
    enum class Transform { Identity, Sine, Square, Saw, Triangle, SimplexNoise, FBMNoise };
    ScalarRemapValueOperator (Input input, InputComponent inputComponent,
        Output output, Operation operation,
        int flags, float inputMin,
        float inputMax, float outputMin, float outputMax,
        Transform transform = Transform::Identity, float transformScale = 2.0f,
        int inputControlPoint0 = 0, int transformOctaves = 3,
        int inputControlPoint1 = 1) :
        input (input), inputComponent (inputComponent), output (output),
        operation (operation), flags (flags),
        inputMin (inputMin), inputMax (inputMax),
        outputMin (outputMin), outputMax (outputMax),
        transform (transform), transformScale (transformScale),
        inputControlPoint0 (inputControlPoint0), transformOctaves (transformOctaves),
        inputControlPoint1 (inputControlPoint1) { }
    Input input;
    InputComponent inputComponent;
    Output output;
    Operation operation;
    int flags;
    float inputMin;
    float inputMax;
    float outputMin;
    float outputMax;
    Transform transform;
    float transformScale;
    int inputControlPoint0;
    int transformOctaves;
    int inputControlPoint1;
};

class VectorRemapValueOperator : public ParticleOperatorBase {
public:
    using Input = ScalarRemapValueOperator::Input;
    using InputComponent = ScalarRemapValueOperator::InputComponent;
    using Operation = ScalarRemapValueOperator::Operation;
    using Transform = ScalarRemapValueOperator::Transform;
    enum class Output { Color, Position, Velocity };
    enum class OutputComponent { All, X, Y, Z };

    VectorRemapValueOperator (Input input, InputComponent inputComponent,
        Output output, OutputComponent outputComponent, Operation operation,
        int flags, glm::vec3 inputMin, glm::vec3 inputMax,
        glm::vec3 outputMin, glm::vec3 outputMax,
        Transform transform = Transform::Identity, float transformScale = 2.0f,
        int inputControlPoint0 = 0, int transformOctaves = 3,
        int inputControlPoint1 = 1) :
        input (input), inputComponent (inputComponent), output (output),
        outputComponent (outputComponent), operation (operation), flags (flags),
        inputMin (inputMin), inputMax (inputMax), outputMin (outputMin),
        outputMax (outputMax), transform (transform), transformScale (transformScale),
        inputControlPoint0 (inputControlPoint0), transformOctaves (transformOctaves),
        inputControlPoint1 (inputControlPoint1) { }
    Input input;
    InputComponent inputComponent;
    Output output;
    OutputComponent outputComponent;
    Operation operation;
    int flags;
    glm::vec3 inputMin;
    glm::vec3 inputMax;
    glm::vec3 outputMin;
    glm::vec3 outputMax;
    Transform transform;
    float transformScale;
    int inputControlPoint0;
    int transformOctaves;
    int inputControlPoint1;
};

// Birth opcode 0x0f shares the remap record/selectors with operator 0x13,
// but executes once without the operator's age envelope.
class RemapInitialValueInitializer : public ParticleInitializerBase {
public:
    explicit RemapInitialValueInitializer (std::unique_ptr<ParticleOperatorBase> remap) :
        remap (std::move (remap)) { }
    std::unique_ptr<ParticleOperatorBase> remap;
};

class AlphaFadeOperator : public ParticleOperatorBase {
public:
    AlphaFadeOperator (UserSettingUniquePtr fadeInTime, UserSettingUniquePtr fadeOutTime) :
	fadeInTime (std::move (fadeInTime)), fadeOutTime (std::move (fadeOutTime)) { }
    UserSettingUniquePtr fadeInTime;
    UserSettingUniquePtr fadeOutTime;
};

class SizeChangeOperator : public ParticleOperatorBase {
public:
    SizeChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class AlphaChangeOperator : public ParticleOperatorBase {
public:
    AlphaChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class ColorChangeOperator : public ParticleOperatorBase {
public:
    ColorChangeOperator (
	UserSettingUniquePtr startTime, UserSettingUniquePtr endTime, UserSettingUniquePtr startValue,
	UserSettingUniquePtr endValue
    ) :
	startTime (std::move (startTime)), endTime (std::move (endTime)), startValue (std::move (startValue)),
	endValue (std::move (endValue)) { }
    UserSettingUniquePtr startTime;
    UserSettingUniquePtr endTime;
    UserSettingUniquePtr startValue;
    UserSettingUniquePtr endValue;
};

class TurbulenceOperator : public ParticleOperatorBase {
public:
    TurbulenceOperator (
	UserSettingUniquePtr scale, UserSettingUniquePtr speedMin, UserSettingUniquePtr speedMax,
	UserSettingUniquePtr timeScale, UserSettingUniquePtr mask, UserSettingUniquePtr phaseMin,
	UserSettingUniquePtr phaseMax, UserSettingUniquePtr audioProcessingMode,
	UserSettingUniquePtr audioProcessingBounds, UserSettingUniquePtr audioProcessingExponent,
	UserSettingUniquePtr audioProcessingFrequencyStart, UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	scale (std::move (scale)), speedMin (std::move (speedMin)), speedMax (std::move (speedMax)),
	timeScale (std::move (timeScale)), mask (std::move (mask)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    UserSettingUniquePtr scale;
    UserSettingUniquePtr speedMin;
    UserSettingUniquePtr speedMax;
    UserSettingUniquePtr timeScale;
    UserSettingUniquePtr mask;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class VortexOperator : public ParticleOperatorBase {
public:
    enum class Variant { Vortex, VortexV2 };
    struct SceneDefaults {
	bool distanceInner;
	bool distanceOuter;
	bool speedInner;
    };
    VortexOperator (
	Variant variant, SceneDefaults sceneDefaults, int controlPoint, int flags,
	UserSettingUniquePtr axis, UserSettingUniquePtr offset,
	UserSettingUniquePtr distanceInner, UserSettingUniquePtr distanceOuter, UserSettingUniquePtr speedInner,
	UserSettingUniquePtr speedOuter, UserSettingUniquePtr centerForce, UserSettingUniquePtr ringRadius,
	UserSettingUniquePtr ringWidth, UserSettingUniquePtr ringPullDistance, UserSettingUniquePtr ringPullForce,
	UserSettingUniquePtr audioProcessingMode, UserSettingUniquePtr audioProcessingBounds,
	UserSettingUniquePtr audioProcessingExponent, UserSettingUniquePtr audioProcessingFrequencyStart,
	UserSettingUniquePtr audioProcessingFrequencyEnd
    ) :
	variant (variant), sceneDefaults (sceneDefaults), controlPoint (controlPoint), flags (flags),
	axis (std::move (axis)), offset (std::move (offset)),
	distanceInner (std::move (distanceInner)), distanceOuter (std::move (distanceOuter)),
	speedInner (std::move (speedInner)), speedOuter (std::move (speedOuter)), centerForce (std::move (centerForce)),
	ringRadius (std::move (ringRadius)), ringWidth (std::move (ringWidth)),
	ringPullDistance (std::move (ringPullDistance)), ringPullForce (std::move (ringPullForce)),
	audioProcessingMode (std::move (audioProcessingMode)),
	audioProcessingBounds (std::move (audioProcessingBounds)),
	audioProcessingExponent (std::move (audioProcessingExponent)),
	audioProcessingFrequencyStart (std::move (audioProcessingFrequencyStart)),
	audioProcessingFrequencyEnd (std::move (audioProcessingFrequencyEnd)) { }
    Variant variant;
    SceneDefaults sceneDefaults;
    int controlPoint;
    int flags; // 1 = infinite axis, 2 = maintain distance to center, 4 = ring shape
    UserSettingUniquePtr axis;
    UserSettingUniquePtr offset;
    UserSettingUniquePtr distanceInner; // Standard vortex inner radius
    UserSettingUniquePtr distanceOuter; // Standard vortex outer radius
    UserSettingUniquePtr speedInner;
    UserSettingUniquePtr speedOuter;
    UserSettingUniquePtr centerForce; // Strength to pull particles toward center
    UserSettingUniquePtr ringRadius; // Ring mode: radius of the ring
    UserSettingUniquePtr ringWidth; // Ring mode: width of the ring
    UserSettingUniquePtr ringPullDistance; // Ring mode: distance at which ring attracts particles
    UserSettingUniquePtr ringPullForce; // Ring mode: strength of ring attraction
    UserSettingUniquePtr audioProcessingMode;
    UserSettingUniquePtr audioProcessingBounds;
    UserSettingUniquePtr audioProcessingExponent;
    UserSettingUniquePtr audioProcessingFrequencyStart;
    UserSettingUniquePtr audioProcessingFrequencyEnd;
};

class ControlPointAttractOperator : public ParticleOperatorBase {
public:
    ControlPointAttractOperator (
	int controlPoint, UserSettingUniquePtr origin, UserSettingUniquePtr scale, UserSettingUniquePtr threshold,
	uint32_t flags
    ) :
	controlPoint (controlPoint), origin (std::move (origin)), scale (std::move (scale)),
	threshold (std::move (threshold)), flags (flags) { }
    int controlPoint;
    UserSettingUniquePtr origin;
    UserSettingUniquePtr scale;
    UserSettingUniquePtr threshold;
    uint32_t flags;
};

class OscillateAlphaOperator : public ParticleOperatorBase {
public:
    OscillateAlphaOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillateSizeOperator : public ParticleOperatorBase {
public:
    OscillateSizeOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
};

class OscillatePositionOperator : public ParticleOperatorBase {
public:
    OscillatePositionOperator (
	UserSettingUniquePtr frequencyMin, UserSettingUniquePtr frequencyMax, UserSettingUniquePtr scaleMin,
	UserSettingUniquePtr scaleMax, UserSettingUniquePtr phaseMin, UserSettingUniquePtr phaseMax,
	UserSettingUniquePtr mask
    ) :
	frequencyMin (std::move (frequencyMin)), frequencyMax (std::move (frequencyMax)),
	scaleMin (std::move (scaleMin)), scaleMax (std::move (scaleMax)), phaseMin (std::move (phaseMin)),
	phaseMax (std::move (phaseMax)), mask (std::move (mask)) { }
    UserSettingUniquePtr frequencyMin;
    UserSettingUniquePtr frequencyMax;
    UserSettingUniquePtr scaleMin;
    UserSettingUniquePtr scaleMax;
    UserSettingUniquePtr phaseMin;
    UserSettingUniquePtr phaseMax;
    UserSettingUniquePtr mask;
};

using ParticleOperatorUniquePtr = std::unique_ptr<ParticleOperatorBase>;

/**
 * Particle renderer configuration
 */
struct ParticleRenderer {
    std::string name;
    std::string orientation; // screen, upright, fixed
    glm::vec3 axis; // authored axis; native renderer factory normalizes it
    uint8_t flags;
    float length;
    float maxLength;
    float minLength;
    float subdivision;
    float segments; // ropetrail: number of history segments per particle
    float uvScale;
    bool uvScrolling;
    bool uvSmoothing; // rope only: reduces flickering when lifetimes are identical
    bool fadeAlpha; // ropetrail: fade alpha along trail
    bool fadeSize; // ropetrail: fade size along trail
};

/**
 * Child particle system
 */
struct ParticleChild {
    std::string type;
    std::string name;
    uint32_t flags;
    int maxCount;
    int controlPointStartIndex;
    float probability;
    glm::vec3 angles;
    glm::vec3 origin;
    glm::vec3 scale;
    std::string particleFile;
};

/**
 * Instance override values
 */
struct ParticleInstanceOverride {
    UserSettingUniquePtr enabled;
    UserSettingUniquePtr alpha;
    UserSettingUniquePtr brightness;
    UserSettingUniquePtr size;
    UserSettingUniquePtr lifetime;
    UserSettingUniquePtr rate;
    UserSettingUniquePtr speed;
    UserSettingUniquePtr count;
    UserSettingUniquePtr color; // Replaces particle color
    UserSettingUniquePtr colorn; // Multiplies particle color
    std::array<UserSettingUniquePtr, 8> controlPoints;
    std::array<UserSettingUniquePtr, 8> controlPointAngles;
};

struct ParticleData {
    /** Position and transformation */
    UserSettingUniquePtr scale;
    UserSettingUniquePtr angles;
    UserSettingUniquePtr visible;

    /** Parallax depth */
    UserSettingUniquePtr parallaxDepth;

    /** Reference to particle definition file */
    std::string particleFile;

    /** Particle system configuration */
    std::string animationMode;
    float sequenceMultiplier;
    uint32_t maxCount;
    /** Authored pre-simulation duration in seconds, including fractional values. */
    float startTime;
    uint32_t flags;
    /** Native preset color comparison/hascolor gate for shared instance RGB. */
    glm::vec3 presetColorN { 1.0f };
    bool presetHasColor { false };
    bool presetTintCompiled { true };

    /** Material for rendering */
    ModelUniquePtr material;

    /** Emitters, initializers, operators, renderers */
    std::vector<ParticleEmitter> emitters;
    std::vector<ParticleInitializerUniquePtr> initializers;
    std::vector<ParticleOperatorUniquePtr> operators;
    std::vector<ParticleRenderer> renderers;
    std::vector<ParticleControlPoint> controlPoints;
    std::vector<ParticleChild> children;

    /** Instance override */
    ParticleInstanceOverride instanceOverride;
};

class Particle : public Object, public ParticleData {
public:
    explicit Particle (ObjectData data, ParticleData particleData) noexcept :
	Object (std::move (data)), ParticleData (std::move (particleData)) { };
    ~Particle () override = default;
};

/**
 * Text object data. Phase 1 of text support covers only static text;
 * dynamic (script-driven) text captures the script source for a future
 * pass but renders whatever initial value the scene provides.
 */
struct TextData {
    /** Initial text content to render (for scripted text, this is the `value` placeholder) */
    UserSettingUniquePtr text;
    /** Font reference from scene (e.g. "fonts/VCR_OSD_MONO.ttf" or "systemfont_arial") */
    std::string font;
    /** Font size in points, optionally bound to a user setting or script */
    UserSettingUniquePtr pointSize;
    /** Additional glyph advance and row spacing in scene-text pixel units */
    UserSettingUniquePtr spacing;
    /** Native gate for maxRows; absent/false leaves row count unlimited */
    UserSettingUniquePtr limitRows;
    /** Maximum laid-out row count when limitRows is enabled; zero disables it */
    UserSettingUniquePtr maxRows;
    /** Native gate for maxWidth; absent/false leaves width unlimited */
    UserSettingUniquePtr limitWidth;
    /** Positive width for wrapping when limitWidth is enabled */
    UserSettingUniquePtr maxWidth;
    /** Append an ellipsis when maxRows discards later rows */
    UserSettingUniquePtr limitUseEllipsis;
    /** Draw the native text-background rectangle behind the glyphs */
    UserSettingUniquePtr opaqueBackground;
    /** Authored RGB for the opaque text background */
    UserSettingUniquePtr backgroundColor;
    /** Native HDR-only background RGB multiplier (independent of alpha) */
    UserSettingUniquePtr backgroundBrightness;
    /** Material effects applied to the text target before scene composition */
    std::vector<ImageEffectUniquePtr> effects;
    /** Bounding box size */
    glm::vec2 size;
    /** Scale (x, y, z) */
    UserSettingUniquePtr scale;
    /** Text color as linear-space RGB */
    UserSettingUniquePtr color;
    /** Native HDR-only monochrome fill RGB multiplier */
    UserSettingUniquePtr brightness;
    /** Alpha multiplier */
    UserSettingUniquePtr alpha;
    /** Whether the text is visible */
    UserSettingUniquePtr visible;
    /** Authored per-axis camera parallax depth */
    UserSettingUniquePtr parallaxDepth;
    /** Horizontal alignment: "left", "center", "right" */
    std::string alignment;
    /** Vertical alignment: "top", "center", "bottom" */
    std::string verticalalign;
    /** Horizontal and vertical effect/background target padding */
    UserSettingUniquePtr padding;
};

class Text : public Object, public TextData {
public:
    explicit Text (ObjectData data, TextData textData) noexcept :
	Object (std::move (data)), TextData (std::move (textData)) { };
    ~Text () override = default;
};
} // namespace WallpaperEngine::Data::Model
