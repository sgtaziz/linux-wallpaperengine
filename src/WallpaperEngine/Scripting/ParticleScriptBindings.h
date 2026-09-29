#pragma once

#include "WallpaperEngine/Data/Model/Object.h"

#include <string>

namespace WallpaperEngine::Scripting {

// Particle component settings are simulation inputs, not public thisLayer
// properties. Visit the retained UserSettings so their scripts can update the
// same DynamicValues that initializer births and operator ticks consume.
template <typename Visitor>
void forEachParticleScriptSetting (const Data::Model::Particle& particle, Visitor&& visit) {
    using namespace Data::Model;
    const auto setting = [&visit] (const std::string& key, const UserSettingUniquePtr& value) {
        if (value && value->value) visit (key, *value->value);
    };
    const auto& instance = particle.instanceOverride;
    setting ("instance_enabled", instance.enabled);
    setting ("instance_alpha", instance.alpha);
    setting ("instance_brightness", instance.brightness);
    setting ("instance_size", instance.size);
    setting ("instance_lifetime", instance.lifetime);
    setting ("instance_rate", instance.rate);
    setting ("instance_speed", instance.speed);
    setting ("instance_count", instance.count);
    setting ("instance_color", instance.color);
    setting ("instance_colorn", instance.colorn);
    for (size_t i = 0; i < instance.controlPoints.size (); ++i) {
        setting ("instance_controlpoint" + std::to_string (i), instance.controlPoints[i]);
        setting ("instance_controlpointangle" + std::to_string (i), instance.controlPointAngles[i]);
    }

    for (size_t i = 0; i < particle.initializers.size (); ++i) {
        const auto& initializer = particle.initializers[i];
        if (!initializer) continue;
        const std::string prefix = "initializer" + std::to_string (i) + "_";
        const auto field = [&] (const char* name, const UserSettingUniquePtr& value) {
            setting (prefix + name, value);
        };
        if (initializer->is<ColorRandomInitializer> ()) {
            const auto& value = *initializer->as<ColorRandomInitializer> ();
            field ("min", value.min); field ("max", value.max);
        } else if (initializer->is<SizeRandomInitializer> ()) {
            const auto& value = *initializer->as<SizeRandomInitializer> ();
            field ("min", value.min); field ("max", value.max); field ("exponent", value.exponent);
        } else if (initializer->is<AlphaRandomInitializer> ()) {
            const auto& value = *initializer->as<AlphaRandomInitializer> ();
            field ("min", value.min); field ("max", value.max); field ("exponent", value.exponent);
        } else if (initializer->is<LifetimeRandomInitializer> ()) {
            const auto& value = *initializer->as<LifetimeRandomInitializer> ();
            field ("min", value.min); field ("max", value.max);
        } else if (initializer->is<VelocityRandomInitializer> ()) {
            const auto& value = *initializer->as<VelocityRandomInitializer> ();
            field ("min", value.min); field ("max", value.max);
        } else if (initializer->is<RotationRandomInitializer> ()) {
            const auto& value = *initializer->as<RotationRandomInitializer> ();
            field ("min", value.min); field ("max", value.max);
        } else if (initializer->is<AngularVelocityRandomInitializer> ()) {
            const auto& value = *initializer->as<AngularVelocityRandomInitializer> ();
            field ("min", value.min); field ("max", value.max); field ("exponent", value.exponent);
        } else if (initializer->is<TurbulentVelocityRandomInitializer> ()) {
            const auto& value = *initializer->as<TurbulentVelocityRandomInitializer> ();
            field ("speedMin", value.speedMin); field ("speedMax", value.speedMax);
            field ("scale", value.scale); field ("offset", value.offset);
            field ("forward", value.forward); field ("timeScale", value.timeScale);
            field ("phaseMin", value.phaseMin); field ("phaseMax", value.phaseMax);
            field ("right", value.right);
        } else if (initializer->is<MapSequenceAroundControlPointInitializer> ()) {
            const auto& value = *initializer->as<MapSequenceAroundControlPointInitializer> ();
            field ("controlPoint", value.controlPoint); field ("count", value.count);
            field ("speedMin", value.speedMin); field ("speedMax", value.speedMax);
        }
    }

    for (size_t i = 0; i < particle.operators.size (); ++i) {
        const auto& op = particle.operators[i];
        if (!op) continue;
        const std::string prefix = "operator" + std::to_string (i) + "_";
        const auto field = [&] (const char* name, const UserSettingUniquePtr& value) {
            setting (prefix + name, value);
        };
        if (op->blendEnvelope) {
            field ("blendInStart", op->blendEnvelope->inStart);
            field ("blendInEnd", op->blendEnvelope->inEnd);
            field ("blendOutStart", op->blendEnvelope->outStart);
            field ("blendOutEnd", op->blendEnvelope->outEnd);
        }
        if (op->is<MovementOperator> ()) {
            const auto& value = *op->as<MovementOperator> ();
            field ("drag", value.drag); field ("gravity", value.gravity);
        } else if (op->is<AngularMovementOperator> ()) {
            const auto& value = *op->as<AngularMovementOperator> ();
            field ("drag", value.drag); field ("force", value.force);
        } else if (op->is<CapVelocityOperator> ()) {
            field ("maxSpeed", op->as<CapVelocityOperator> ()->maxSpeed);
        } else if (op->is<AlphaFadeOperator> ()) {
            const auto& value = *op->as<AlphaFadeOperator> ();
            field ("fadeInTime", value.fadeInTime); field ("fadeOutTime", value.fadeOutTime);
        } else if (op->is<SizeChangeOperator> ()) {
            const auto& value = *op->as<SizeChangeOperator> ();
            field ("startTime", value.startTime); field ("endTime", value.endTime);
            field ("startValue", value.startValue); field ("endValue", value.endValue);
        } else if (op->is<AlphaChangeOperator> ()) {
            const auto& value = *op->as<AlphaChangeOperator> ();
            field ("startTime", value.startTime); field ("endTime", value.endTime);
            field ("startValue", value.startValue); field ("endValue", value.endValue);
        } else if (op->is<ColorChangeOperator> ()) {
            const auto& value = *op->as<ColorChangeOperator> ();
            field ("startTime", value.startTime); field ("endTime", value.endTime);
            field ("startValue", value.startValue); field ("endValue", value.endValue);
        } else if (op->is<TurbulenceOperator> ()) {
            const auto& value = *op->as<TurbulenceOperator> ();
            field ("scale", value.scale); field ("speedMin", value.speedMin);
            field ("speedMax", value.speedMax); field ("timeScale", value.timeScale);
            field ("mask", value.mask); field ("phaseMin", value.phaseMin);
            field ("phaseMax", value.phaseMax);
            field ("audioMode", value.audioProcessingMode);
            field ("audioBounds", value.audioProcessingBounds);
            field ("audioExponent", value.audioProcessingExponent);
            field ("audioFrequencyStart", value.audioProcessingFrequencyStart);
            field ("audioFrequencyEnd", value.audioProcessingFrequencyEnd);
        } else if (op->is<VortexOperator> ()) {
            const auto& value = *op->as<VortexOperator> ();
            field ("axis", value.axis); field ("offset", value.offset);
            field ("distanceInner", value.distanceInner); field ("distanceOuter", value.distanceOuter);
            field ("speedInner", value.speedInner); field ("speedOuter", value.speedOuter);
            field ("centerForce", value.centerForce); field ("ringRadius", value.ringRadius);
            field ("ringWidth", value.ringWidth); field ("ringPullDistance", value.ringPullDistance);
            field ("ringPullForce", value.ringPullForce);
            field ("audioMode", value.audioProcessingMode);
            field ("audioBounds", value.audioProcessingBounds);
            field ("audioExponent", value.audioProcessingExponent);
            field ("audioFrequencyStart", value.audioProcessingFrequencyStart);
            field ("audioFrequencyEnd", value.audioProcessingFrequencyEnd);
        } else if (op->is<ControlPointAttractOperator> ()) {
            const auto& value = *op->as<ControlPointAttractOperator> ();
            field ("origin", value.origin); field ("scale", value.scale);
            field ("threshold", value.threshold);
        } else if (op->is<OscillateAlphaOperator> ()) {
            const auto& value = *op->as<OscillateAlphaOperator> ();
            field ("frequencyMin", value.frequencyMin); field ("frequencyMax", value.frequencyMax);
            field ("scaleMin", value.scaleMin); field ("scaleMax", value.scaleMax);
            field ("phaseMin", value.phaseMin); field ("phaseMax", value.phaseMax);
        } else if (op->is<OscillateSizeOperator> ()) {
            const auto& value = *op->as<OscillateSizeOperator> ();
            field ("frequencyMin", value.frequencyMin); field ("frequencyMax", value.frequencyMax);
            field ("scaleMin", value.scaleMin); field ("scaleMax", value.scaleMax);
            field ("phaseMin", value.phaseMin); field ("phaseMax", value.phaseMax);
        } else if (op->is<OscillatePositionOperator> ()) {
            const auto& value = *op->as<OscillatePositionOperator> ();
            field ("frequencyMin", value.frequencyMin); field ("frequencyMax", value.frequencyMax);
            field ("scaleMin", value.scaleMin); field ("scaleMax", value.scaleMax);
            field ("phaseMin", value.phaseMin); field ("phaseMax", value.phaseMax);
            field ("mask", value.mask);
        }
    }
}

} // namespace WallpaperEngine::Scripting
