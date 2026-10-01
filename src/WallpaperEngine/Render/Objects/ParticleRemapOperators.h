#pragma once

#include "CParticle.h"
#include "ParticleCore.h"

#include <bit>

namespace WallpaperEngine::Render::Objects {

inline bool isBirthControlPointInput (Data::Model::ScalarRemapValueOperator::Input input) {
    using Input = Data::Model::ScalarRemapValueOperator::Input;
    return input == Input::ControlPoint || input == Input::DeltaToControlPoint
        || input == Input::DirectionToControlPoint;
}

inline void checkOrdinaryBirthRemapSource (glm::vec3 source, bool checkedBirth) {
    if (checkedBirth && (!std::isfinite (source.x) || !std::isfinite (source.y) || !std::isfinite (source.z)))
        throw std::invalid_argument ("Runtime control-point output nonfinite ordinary birth remap is unsupported");
}

inline void checkOrdinaryBirthBetweenControlPoints (
    glm::vec3 position, glm::vec3 first, glm::vec3 second, bool checkedBirth) {
    if (!checkedBirth) return;
    checkOrdinaryBirthRemapSource (position, true);
    checkOrdinaryBirthRemapSource (first, true);
    checkOrdinaryBirthRemapSource (second, true);
    const auto axis = second - first;
    const auto offset = position - first;
    if (!std::isfinite (glm::dot (axis, axis)) || !std::isfinite (glm::dot (offset, axis)))
        throw std::invalid_argument ("Runtime control-point output nonfinite ordinary birth remap is unsupported");
}

inline glm::vec3 remapControlPointInput (
    glm::vec3 particlePosition, ControlPointData& controlPoint,
    ParticleCore::RemapControlPointVector selector, bool birth, bool nativeSlots = false) {
    if (!birth) {
        if (!nativeSlots) return ParticleCore::remapControlPointVector (particlePosition, controlPoint.position, selector);
        if (selector == ParticleCore::RemapControlPointVector::Position)
            return ParticleCore::toAuthoredVector (controlPoint.position);
        const auto delta = ParticleCore::toAuthoredVector (controlPoint.position - particlePosition);
        if (selector == ParticleCore::RemapControlPointVector::Delta) return delta;
        const float squaredLength = ParticleCore::nativeRuntimeSquaredLength (delta.x, delta.y, delta.z);
        return delta * (squaredLength > 0.0f ? ParticleCore::nativeRuntimeReciprocalSqrt (squaredLength) : 0.0f);
    }
    // Birth 14023b340 inputs 16..18 store zero into matrix column-three XYZ
    // before reduction/ranges/output gates. Basis and authored offset survive.
    controlPoint.position = ParticleCore::toSimulationVector (glm::vec3 (0.0f));
    if (selector == ParticleCore::RemapControlPointVector::Position) return glm::vec3 (0.0f);
    const glm::vec3 delta = glm::vec3 (0.0f) - ParticleCore::toAuthoredVector (particlePosition);
    if (selector == ParticleCore::RemapControlPointVector::Delta) return delta;
    // 14005eb80: ((x*x + y*y) + z*z), sqrt, reciprocal, multiply.
    // Zero/nonfinite vectors deliberately retain native IEEE outcomes.
    const float squaredLength = (delta.x * delta.x + delta.y * delta.y) + delta.z * delta.z;
    return delta * (1.0f / std::sqrt (squaredLength));
}

inline OperatorFunc createScalarRemapOperator (
    const Data::Model::ScalarRemapValueOperator& op, bool birth,
    bool hasRotationRandom, bool hasAngularMovement, bool hasAngularVelocityRandom,
    bool nativeSlots = false) {
    using Data::Model::ScalarRemapValueOperator;
    const auto input = op.input;
    const bool birthControlPointIEEE = birth && isBirthControlPointInput (input);
    const bool birthNoise = birth && (op.transform == decltype (op.transform)::SimplexNoise
        || op.transform == decltype (op.transform)::FBMNoise);
    const auto output = op.output;
    const int inputControlPoint = op.inputControlPoint0;
    const int inputControlPoint1 = op.inputControlPoint1;
    ParticleCore::RemapVectorComponent vectorComponent = ParticleCore::RemapVectorComponent::X;
    switch (op.inputComponent) {
    case ScalarRemapValueOperator::InputComponent::All:
        vectorComponent = ParticleCore::RemapVectorComponent::All; break;
    case ScalarRemapValueOperator::InputComponent::X:
	vectorComponent = ParticleCore::RemapVectorComponent::X; break;
    case ScalarRemapValueOperator::InputComponent::Y:
	vectorComponent = ParticleCore::RemapVectorComponent::Y; break;
    case ScalarRemapValueOperator::InputComponent::Z:
	vectorComponent = ParticleCore::RemapVectorComponent::Z; break;
    case ScalarRemapValueOperator::InputComponent::Sum:
	vectorComponent = ParticleCore::RemapVectorComponent::Sum; break;
    case ScalarRemapValueOperator::InputComponent::Average:
	vectorComponent = ParticleCore::RemapVectorComponent::Average; break;
    case ScalarRemapValueOperator::InputComponent::Max:
	vectorComponent = ParticleCore::RemapVectorComponent::Max; break;
    case ScalarRemapValueOperator::InputComponent::Min:
	vectorComponent = ParticleCore::RemapVectorComponent::Min; break;
    }
    ParticleCore::RemapOperation operation = ParticleCore::RemapOperation::Multiply;
    switch (op.operation) {
    case ScalarRemapValueOperator::Operation::Set:
	operation = ParticleCore::RemapOperation::Set; break;
    case ScalarRemapValueOperator::Operation::Multiply:
	operation = ParticleCore::RemapOperation::Multiply; break;
    case ScalarRemapValueOperator::Operation::Add:
	operation = ParticleCore::RemapOperation::Add; break;
    case ScalarRemapValueOperator::Operation::Subtract:
	operation = ParticleCore::RemapOperation::Subtract; break;
    }
    const ParticleCore::ScalarRemapRange range {
	op.inputMin, op.inputMax, op.outputMin, op.outputMax, op.flags,
	op.transform == ScalarRemapValueOperator::Transform::Sine
	    ? ParticleCore::RemapTransform::Sine
	    : op.transform == ScalarRemapValueOperator::Transform::Square
	        ? ParticleCore::RemapTransform::Square
	    : op.transform == ScalarRemapValueOperator::Transform::Saw
	        ? ParticleCore::RemapTransform::Saw
	    : op.transform == ScalarRemapValueOperator::Transform::Triangle
	        ? ParticleCore::RemapTransform::Triangle
	    : op.transform == ScalarRemapValueOperator::Transform::SimplexNoise
	        ? ParticleCore::RemapTransform::SimplexNoise
	    : op.transform == ScalarRemapValueOperator::Transform::FBMNoise
	        ? ParticleCore::RemapTransform::FBMNoise : ParticleCore::RemapTransform::Identity,
	op.transformScale };
    // Runtime fallback and native birth kernels consume the same authored count.
    auto noiseRange = range;
    noiseRange.noiseOctaves = op.transformOctaves;
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;
	    return [input, output, inputControlPoint, inputControlPoint1, operation, vectorComponent, noiseRange, blend,
	    hasRotationRandom, hasAngularMovement, hasAngularVelocityRandom, birth, birthControlPointIEEE, birthNoise, nativeSlots] (
	std::vector<ParticleInstance>& particles, uint32_t count,
	std::vector<ControlPointData>& controlPoints, float, ParticleCore::MovementTime) {
	const auto envelope = blend ? std::optional<ParticleCore::BlendEnvelope> ({
            blend->inStart->value->getFloat (), blend->inEnd->value->getFloat (),
            blend->outStart->value->getFloat (), blend->outEnd->value->getFloat ()
        }) : std::nullopt;
	for (uint32_t i = 0; i < count; ++i) {
	    auto& p = particles[i];
	    if (!nativeSlots && !p.alive) continue;
	    if (!birth && !nativeSlots && (!std::isfinite (p.lifetime) || p.lifetime <= 0.0f)) continue;
	    const float lifetimeFraction = birth ? (nativeSlots ? p.sequenceFraction : 0.0f)
                : nativeSlots ? ParticleCore::nativeRuntimeLifetimeFraction (p.age, p.lifetime) : p.age / p.lifetime;
	    const bool checkedBirthSource = nativeSlots && birth && !birthControlPointIEEE && !birthNoise;
	    if (input == ScalarRemapValueOperator::Input::PositionBetweenTwoControlPoints
                && inputControlPoint >= 0 && inputControlPoint1 >= 0
                && inputControlPoint < static_cast<int> (controlPoints.size ())
                && inputControlPoint1 < static_cast<int> (controlPoints.size ()))
                checkOrdinaryBirthBetweenControlPoints (p.position, controlPoints[inputControlPoint].position,
                    controlPoints[inputControlPoint1].position, checkedBirthSource);
	    float inputValue = lifetimeFraction;
	    switch (input) {
	    case ScalarRemapValueOperator::Input::LifetimeFraction: break;
	    case ScalarRemapValueOperator::Input::MaxLifetime: inputValue = p.lifetime; break;
	    case ScalarRemapValueOperator::Input::Size: inputValue = p.size; break;
	    case ScalarRemapValueOperator::Input::Opacity: inputValue = p.alpha; break;
	    case ScalarRemapValueOperator::Input::Speed: inputValue = nativeSlots && !birth
                ? ParticleCore::nativeRuntimeLength (p.velocity.x, p.velocity.y, p.velocity.z) : glm::length (p.velocity); break;
	    case ScalarRemapValueOperator::Input::Rotation: inputValue = p.rotation.z; break;
	    case ScalarRemapValueOperator::Input::AngularSpeed:
		inputValue = ParticleCore::gatedAngularSpeed (
		    p.angularVelocity.z, birth ? hasAngularVelocityRandom : hasRotationRandom, hasAngularMovement);
		break;
	case ScalarRemapValueOperator::Input::DistanceToControlPoint:
		inputValue = inputControlPoint >= 0
		    && inputControlPoint < static_cast<int> (controlPoints.size ())
		    ? (nativeSlots && !birth ? ParticleCore::nativeRuntimeLength (
                      p.position.x - controlPoints[inputControlPoint].position.x,
                      p.position.y - controlPoints[inputControlPoint].position.y,
                      p.position.z - controlPoints[inputControlPoint].position.z)
                    : ParticleCore::remapControlPointDistance (p.position, controlPoints[inputControlPoint].position)) : 0.0f;
		break;
	case ScalarRemapValueOperator::Input::PositionBetweenTwoControlPoints:
		inputValue = inputControlPoint >= 0 && inputControlPoint1 >= 0
		    && inputControlPoint < static_cast<int> (controlPoints.size ())
		    && inputControlPoint1 < static_cast<int> (controlPoints.size ())
		    ? (nativeSlots && !birth ? ParticleCore::nativeRuntimePositionBetweenControlPoints (
                      p.position, controlPoints[inputControlPoint].position, controlPoints[inputControlPoint1].position)
                    : ParticleCore::remapPositionBetweenControlPoints (
                      p.position, controlPoints[inputControlPoint].position, controlPoints[inputControlPoint1].position)) : 0.0f;
		break;
	case ScalarRemapValueOperator::Input::ControlPoint:
	case ScalarRemapValueOperator::Input::DeltaToControlPoint:
	case ScalarRemapValueOperator::Input::DirectionToControlPoint:
		if (inputControlPoint >= 0
		    && inputControlPoint < static_cast<int> (controlPoints.size ())) {
		    const auto selector = input == ScalarRemapValueOperator::Input::ControlPoint
		        ? ParticleCore::RemapControlPointVector::Position
		        : input == ScalarRemapValueOperator::Input::DeltaToControlPoint
		            ? ParticleCore::RemapControlPointVector::Delta
		            : ParticleCore::RemapControlPointVector::Direction;
		    const auto source = remapControlPointInput (
		        p.position, controlPoints[inputControlPoint], selector, birth, nativeSlots);
		    inputValue = (birthControlPointIEEE || birthNoise)
                        ? ParticleCore::reduceBirthRemapVector (source.x, source.y, source.z, vectorComponent)
                        : nativeSlots && !birth ? ParticleCore::reduceNativeRuntimeRemapVector (source.x, source.y, source.z, vectorComponent)
                        : ParticleCore::reduceRemapVector (source.x, source.y, source.z, vectorComponent);
		} else inputValue = 0.0f;
		break;
	    case ScalarRemapValueOperator::Input::Color:
		{
                const auto source = p.color;
                checkOrdinaryBirthRemapSource (source, checkedBirthSource);
                inputValue = birthNoise
                    ? ParticleCore::reduceBirthRemapVector (source.x, source.y, source.z, vectorComponent)
                    : nativeSlots && !birth ? ParticleCore::reduceNativeRuntimeRemapVector (source.x, source.y, source.z, vectorComponent)
                        : ParticleCore::reduceRemapVector (source.x, source.y, source.z, vectorComponent);
            }
		break;
	    case ScalarRemapValueOperator::Input::Position:
		{
                const auto source = ParticleCore::toAuthoredVector (p.position);
                checkOrdinaryBirthRemapSource (source, checkedBirthSource);
                inputValue = birthNoise
                    ? ParticleCore::reduceBirthRemapVector (source.x, source.y, source.z, vectorComponent)
                    : nativeSlots && !birth ? ParticleCore::reduceNativeRuntimeRemapVector (source.x, source.y, source.z, vectorComponent)
                        : ParticleCore::reduceRemapVector (source.x, source.y, source.z, vectorComponent);
            }
		break;
	    case ScalarRemapValueOperator::Input::Velocity:
		{
                const auto source = ParticleCore::toAuthoredVector (p.velocity);
                checkOrdinaryBirthRemapSource (source, checkedBirthSource);
                inputValue = birthNoise
                    ? ParticleCore::reduceBirthRemapVector (source.x, source.y, source.z, vectorComponent)
                    : nativeSlots && !birth ? ParticleCore::reduceNativeRuntimeRemapVector (source.x, source.y, source.z, vectorComponent)
                        : ParticleCore::reduceRemapVector (source.x, source.y, source.z, vectorComponent);
            }
		break;
	    }
	    auto particleRange = noiseRange;
	    particleRange.noiseSeedBits = std::bit_cast<uint32_t> (p.oscillatorRandom);
	    if (output == ScalarRemapValueOperator::Output::Size)
		p.size = ParticleCore::remapScalarValue (
		    p.size, inputValue, lifetimeFraction, operation, envelope, particleRange, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
	    else if (output == ScalarRemapValueOperator::Output::Opacity)
		p.alpha = ParticleCore::remapScalarValue (
		    p.alpha, inputValue, lifetimeFraction, operation, envelope, particleRange, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
	    else if (output == ScalarRemapValueOperator::Output::MaxLifetime)
	        p.lifetime = ParticleCore::remapScalarValue (
	            p.lifetime, inputValue, lifetimeFraction, operation, envelope, particleRange, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
	    else if (output == ScalarRemapValueOperator::Output::Rotation)
	        p.rotation.z = ParticleCore::remapScalarValue (
	            p.rotation.z, inputValue, lifetimeFraction, operation, envelope, particleRange, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
	    else if (output == ScalarRemapValueOperator::Output::AngularSpeed) {
	        p.angularVelocity.z = ParticleCore::remapBirthAngularSpeed (
                p.angularVelocity.z, inputValue, operation, particleRange,
                hasAngularVelocityRandom, hasAngularMovement, birthControlPointIEEE, birthNoise, nativeSlots && birth);
	    }
	    else {
		const float currentSpeed = nativeSlots && !birth
                ? ParticleCore::nativeRuntimeLength (p.velocity.x, p.velocity.y, p.velocity.z) : glm::length (p.velocity);
		const float mappedSpeed = ParticleCore::remapScalarValue (
		    currentSpeed, inputValue, lifetimeFraction, operation, envelope, particleRange, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
		if (nativeSlots && !birth) {
                const float factor = currentSpeed > 0.0f
                    ? ParticleCore::nativeRuntimeReciprocal (currentSpeed) * mappedSpeed : mappedSpeed;
                p.velocity *= factor;
            } else p.velocity = ParticleCore::remapSpeedOutput (p.velocity, mappedSpeed, birthControlPointIEEE || birthNoise);
	    }
	}
    };
}

// The same CPU stream writer serves live vector remaps and scalar-dispatched
// birth remaps. Retaining this factory also allows contract tests to execute
// the production closure without constructing a scene or graphics context.
inline OperatorFunc createVectorRemapOperator (
    const Data::Model::VectorRemapValueOperator& op, bool birth,
    bool hasRotationRandom, bool hasAngularMovement, bool hasAngularVelocityRandom,
    bool nativeSlots = false) {
    using Data::Model::VectorRemapValueOperator;
    // Runtime CP output consumes sparse SIMD lane-zero state, including
    // expired slots. Compact execution retains its existing feature guard.
    if (op.output == VectorRemapValueOperator::Output::ControlPoint && !birth && !nativeSlots) return {};
    const auto input = op.input;
    const bool birthControlPointIEEE = birth && isBirthControlPointInput (input);
    const bool birthNoise = birth && (op.transform == decltype (op.transform)::SimplexNoise
        || op.transform == decltype (op.transform)::FBMNoise);
    const auto output = op.output;
    const int outputControlPoint = op.outputControlPoint0;
    const int inputControlPoint = op.inputControlPoint0;
    const int inputControlPoint1 = op.inputControlPoint1;
    const auto outputComponent = static_cast<int> (op.outputComponent);
    const auto inputComponent = op.inputComponent;
    const auto inputMin = op.inputMin;
    const auto inputMax = op.inputMax;
    const auto outputMin = op.outputMin;
    const auto outputMax = op.outputMax;
    const int flags = op.flags;
    const auto transform = op.transform == VectorRemapValueOperator::Transform::Sine
        ? ParticleCore::RemapTransform::Sine
        : op.transform == VectorRemapValueOperator::Transform::Square
            ? ParticleCore::RemapTransform::Square
        : op.transform == VectorRemapValueOperator::Transform::Saw
            ? ParticleCore::RemapTransform::Saw
        : op.transform == VectorRemapValueOperator::Transform::Triangle
            ? ParticleCore::RemapTransform::Triangle
        : op.transform == VectorRemapValueOperator::Transform::SimplexNoise
            ? ParticleCore::RemapTransform::SimplexNoise
        : op.transform == VectorRemapValueOperator::Transform::FBMNoise
            ? ParticleCore::RemapTransform::FBMNoise : ParticleCore::RemapTransform::Identity;
    const float transformScale = op.transformScale;
    const int transformOctaves = op.transformOctaves;
    const auto* blend = op.blendEnvelope ? &*op.blendEnvelope : nullptr;
    ParticleCore::RemapOperation operation = ParticleCore::RemapOperation::Multiply;
    switch (op.operation) {
    case VectorRemapValueOperator::Operation::Set:
        operation = ParticleCore::RemapOperation::Set; break;
    case VectorRemapValueOperator::Operation::Multiply:
        operation = ParticleCore::RemapOperation::Multiply; break;
    case VectorRemapValueOperator::Operation::Add:
        operation = ParticleCore::RemapOperation::Add; break;
    case VectorRemapValueOperator::Operation::Subtract:
        operation = ParticleCore::RemapOperation::Subtract; break;
    }
    ParticleCore::RemapVectorComponent component = ParticleCore::RemapVectorComponent::All;
    switch (inputComponent) {
    case VectorRemapValueOperator::InputComponent::All: break;
    case VectorRemapValueOperator::InputComponent::X:
        component = ParticleCore::RemapVectorComponent::X; break;
    case VectorRemapValueOperator::InputComponent::Y:
        component = ParticleCore::RemapVectorComponent::Y; break;
    case VectorRemapValueOperator::InputComponent::Z:
        component = ParticleCore::RemapVectorComponent::Z; break;
    case VectorRemapValueOperator::InputComponent::Sum:
        component = ParticleCore::RemapVectorComponent::Sum; break;
    case VectorRemapValueOperator::InputComponent::Average:
        component = ParticleCore::RemapVectorComponent::Average; break;
    case VectorRemapValueOperator::InputComponent::Max:
        component = ParticleCore::RemapVectorComponent::Max; break;
    case VectorRemapValueOperator::InputComponent::Min:
        component = ParticleCore::RemapVectorComponent::Min; break;
    }
    return [input, output, outputControlPoint, inputControlPoint, inputControlPoint1, outputComponent, component, inputMin, inputMax,
            outputMin, outputMax, flags, transform, transformScale, transformOctaves,
            blend, operation,
            hasRotationRandom, hasAngularMovement, hasAngularVelocityRandom, birth, birthControlPointIEEE, birthNoise, nativeSlots] (
        std::vector<ParticleInstance>& particles, uint32_t count,
        std::vector<ControlPointData>& controlPoints, float, ParticleCore::MovementTime) {
        if (output == VectorRemapValueOperator::Output::ControlPoint
            && (outputControlPoint < 0 || outputControlPoint >= static_cast<int> (controlPoints.size ()))) return;
        const auto envelope = blend ? std::optional<ParticleCore::BlendEnvelope> ({
            blend->inStart->value->getFloat (), blend->inEnd->value->getFloat (),
            blend->outStart->value->getFloat (), blend->outEnd->value->getFloat ()
        }) : std::nullopt;
        const bool blockControlPointWriter = !birth && output == VectorRemapValueOperator::Output::ControlPoint;
        for (uint32_t i = 0; i < count; i += blockControlPointWriter ? 4u : 1u) {
            auto& p = particles[i];
            if (!nativeSlots && (!p.alive || (!birth && (!std::isfinite (p.lifetime) || p.lifetime <= 0.0f)))) continue;
            const float age = birth ? (nativeSlots ? p.sequenceFraction : 0.0f)
                : nativeSlots ? ParticleCore::nativeRuntimeLifetimeFraction (p.age, p.lifetime) : p.age / p.lifetime;
            const bool checkedBirthSource = nativeSlots && birth && !birthControlPointIEEE && !birthNoise;
            if (input == VectorRemapValueOperator::Input::PositionBetweenTwoControlPoints
                && inputControlPoint >= 0 && inputControlPoint1 >= 0
                && inputControlPoint < static_cast<int> (controlPoints.size ())
                && inputControlPoint1 < static_cast<int> (controlPoints.size ()))
                checkOrdinaryBirthBetweenControlPoints (p.position, controlPoints[inputControlPoint].position,
                    controlPoints[inputControlPoint1].position, checkedBirthSource);
            glm::vec3 source (age);
            bool vectorInput = false;
            switch (input) {
            case VectorRemapValueOperator::Input::LifetimeFraction: break;
            case VectorRemapValueOperator::Input::MaxLifetime: source = glm::vec3 (p.lifetime); break;
            case VectorRemapValueOperator::Input::Size: source = glm::vec3 (p.size); break;
            case VectorRemapValueOperator::Input::Opacity: source = glm::vec3 (p.alpha); break;
            case VectorRemapValueOperator::Input::Speed: source = glm::vec3 (nativeSlots && !birth
                ? ParticleCore::nativeRuntimeLength (p.velocity.x, p.velocity.y, p.velocity.z) : glm::length (p.velocity)); break;
            case VectorRemapValueOperator::Input::Rotation: source = glm::vec3 (p.rotation.z); break;
            case VectorRemapValueOperator::Input::AngularSpeed:
                source = glm::vec3 (ParticleCore::gatedAngularSpeed (
                    p.angularVelocity.z, birth ? hasAngularVelocityRandom : hasRotationRandom, hasAngularMovement));
                break;
            case VectorRemapValueOperator::Input::DistanceToControlPoint:
                source = glm::vec3 (inputControlPoint >= 0
                    && inputControlPoint < static_cast<int> (controlPoints.size ())
                    ? (nativeSlots && !birth ? ParticleCore::nativeRuntimeLength (
                          p.position.x - controlPoints[inputControlPoint].position.x,
                          p.position.y - controlPoints[inputControlPoint].position.y,
                          p.position.z - controlPoints[inputControlPoint].position.z)
                        : ParticleCore::remapControlPointDistance (p.position, controlPoints[inputControlPoint].position)) : 0.0f);
                break;
            case VectorRemapValueOperator::Input::PositionBetweenTwoControlPoints:
                source = glm::vec3 (inputControlPoint >= 0 && inputControlPoint1 >= 0
                    && inputControlPoint < static_cast<int> (controlPoints.size ())
                    && inputControlPoint1 < static_cast<int> (controlPoints.size ())
                    ? (nativeSlots && !birth ? ParticleCore::nativeRuntimePositionBetweenControlPoints (
                          p.position, controlPoints[inputControlPoint].position, controlPoints[inputControlPoint1].position)
                        : ParticleCore::remapPositionBetweenControlPoints (
                          p.position, controlPoints[inputControlPoint].position, controlPoints[inputControlPoint1].position)) : 0.0f);
                break;
            case VectorRemapValueOperator::Input::ControlPoint:
            case VectorRemapValueOperator::Input::DeltaToControlPoint:
            case VectorRemapValueOperator::Input::DirectionToControlPoint:
                if (inputControlPoint >= 0
                    && inputControlPoint < static_cast<int> (controlPoints.size ())) {
                    const auto selector = input == VectorRemapValueOperator::Input::ControlPoint
                        ? ParticleCore::RemapControlPointVector::Position
                        : input == VectorRemapValueOperator::Input::DeltaToControlPoint
                            ? ParticleCore::RemapControlPointVector::Delta
                            : ParticleCore::RemapControlPointVector::Direction;
                    source = remapControlPointInput (
                        p.position, controlPoints[inputControlPoint], selector, birth, nativeSlots);
                } else source = glm::vec3 (0.0f);
                vectorInput = true;
                break;
            case VectorRemapValueOperator::Input::Color:
                source = p.color; vectorInput = true; break;
            case VectorRemapValueOperator::Input::Position:
                source = ParticleCore::toAuthoredVector (p.position); vectorInput = true; break;
            case VectorRemapValueOperator::Input::Velocity:
                source = ParticleCore::toAuthoredVector (p.velocity); vectorInput = true; break;
            }
            if (vectorInput) checkOrdinaryBirthRemapSource (source, checkedBirthSource);
            if (vectorInput && component != ParticleCore::RemapVectorComponent::All) {
                source = glm::vec3 ((birthControlPointIEEE || birthNoise)
                    ? ParticleCore::reduceBirthRemapVector (source.x, source.y, source.z, component)
                    : nativeSlots && !birth ? ParticleCore::reduceNativeRuntimeRemapVector (source.x, source.y, source.z, component)
                        : ParticleCore::reduceRemapVector (source.x, source.y, source.z, component));
            }
            glm::vec3 current = output == VectorRemapValueOperator::Output::Color
                ? p.color : output == VectorRemapValueOperator::Output::Position
                    ? ParticleCore::toAuthoredVector (p.position)
                    : output == VectorRemapValueOperator::Output::Velocity
                        ? ParticleCore::toAuthoredVector (p.velocity)
                        : ParticleCore::toAuthoredVector (controlPoints[outputControlPoint].position);
            current = ParticleCore::remapVectorValue (current, source, age, operation,
                inputMin, inputMax, outputMin, outputMax, flags, envelope,
                outputComponent, transform, transformScale,
                std::bit_cast<uint32_t> (p.oscillatorRandom), vectorInput,
                transformOctaves, birthControlPointIEEE, birthNoise, nativeSlots && !birth, nativeSlots && birth);
            if (output == VectorRemapValueOperator::Output::Color) p.color = current;
            else if (output == VectorRemapValueOperator::Output::Position)
                p.position = ParticleCore::toSimulationVector (current);
            else if (output == VectorRemapValueOperator::Output::Velocity)
                p.velocity = ParticleCore::toSimulationVector (current);
            else {
                // Birth 14023b340 and runtime 140243dd0 output 16 write only
                // translation XYZ; subsequent births/blocks/records observe it.
                controlPoints[outputControlPoint].position = ParticleCore::toSimulationVector (current);
            }
        }
    };
}


} // namespace WallpaperEngine::Render::Objects
