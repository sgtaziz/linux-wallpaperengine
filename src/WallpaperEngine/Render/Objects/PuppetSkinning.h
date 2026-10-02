#pragma once

#include "PuppetMeshParser.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace WallpaperEngine::Render::Objects {

struct PuppetPoseSample {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
};

struct PuppetFrameSelection {
    size_t first = 0;
    size_t next = 0;
    float weight = 0.0f;
};

inline PuppetFrameSelection puppetSelectFrames (const PuppetClipHeader& clip, float playbackSeconds) {
    // Native 1401a8c10 constructs frameDuration = 1/rate; 140170580
    // consumes already-normalized playback time and keeps the terminal sample.
    if (!std::isfinite (clip.rawRate) || clip.rawRate <= 0.0f ||
	clip.rawFrameCount == 0 || !std::isfinite (playbackSeconds) || playbackSeconds < 0.0f)
	throw std::runtime_error ("Invalid puppet clip time or rate");
    const float frameDuration = 1.0f / clip.rawRate;
    const float rawFrame = playbackSeconds / frameDuration;
    const size_t first = rawFrame >= float (clip.rawFrameCount) ?
	                 size_t (clip.rawFrameCount - 1) : size_t (rawFrame);
    const float weight = std::fmod (playbackSeconds, frameDuration) / frameDuration;
    return {first, first + 1, weight};
}

inline float puppetScalarAtFrames (const PuppetClipHeader::ScalarTrack& track,
                                   const PuppetFrameSelection& frames) {
    if (frames.first >= track.rawSamples.size () || frames.next >= track.rawSamples.size ())
        throw std::runtime_error ("Puppet scalar sample index out of range");
    return std::lerp (track.rawSamples[frames.first], track.rawSamples[frames.next], frames.weight);
}

inline float puppetApplyScalarLayer (float previous, float sampled, float weight, bool additive) {
    // 1401fdf90 writes ordinary scalar lanes after a frame and layer lerp;
    // the additive path adds sampled*weight to the retained lane.
    return additive ? previous + sampled * weight : std::lerp (previous, sampled, weight);
}

struct PuppetPlaybackState {
    float time = 0.0f;
    bool reverse = false;
    bool stopped = false;
};

inline void puppetAdvancePlayback (
    const PuppetClipHeader& clip, PuppetPlaybackState& state, float deltaSeconds, float layerRate
) {
    // Native 1401a9f60 updates a stateful clock. Mirror reverses at most once
    // per update; single sets a terminal stop flag. A caller can pause by not
    // advancing the state.
    if (!std::isfinite (deltaSeconds) || !std::isfinite (layerRate) ||
	!std::isfinite (state.time) || !std::isfinite (clip.rawRate) ||
	clip.rawRate <= 0.0f || clip.rawFrameCount == 0)
	throw std::runtime_error ("Invalid puppet playback state or rate");
    if (state.stopped) return;
    const float duration = float (clip.rawFrameCount) / clip.rawRate;
    const float delta = deltaSeconds * layerRate * (state.reverse ? -1.0f : 1.0f);
    state.time += delta;
    if (clip.mode == "single") {
	if (state.time >= duration) {
	    state.time = duration;
	    state.stopped = true;
	}
    } else if (clip.mode == "mirror") {
	if (!state.reverse && state.time >= duration) {
	    state.time = duration - std::fmod (state.time, duration);
	    state.reverse = true;
	} else if (state.reverse && state.time <= 0.0f) {
	    state.time = -std::fmod (state.time, duration);
	    state.reverse = false;
	}
    } else {
	if (state.time >= duration) state.time = std::fmod (state.time, duration);
	else if (state.time < 0.0f) state.time = std::fmod (state.time + duration, duration);
    }
}

inline float puppetEffectiveLayerWeight (
    const PuppetClipHeader& clip, const PuppetPlaybackState& playback,
    float baseWeight, float blendTime, bool& blendInActive, bool blendOutActive
) {
    // Native 14026c8b0 applies independent entry/exit ramps around the
    // shorter of half the clip duration and authored blend time.
    if (!std::isfinite (baseWeight) || !std::isfinite (blendTime) ||
	!std::isfinite (playback.time) || !std::isfinite (clip.rawRate) || clip.rawRate <= 0.0f)
	throw std::runtime_error ("Invalid puppet layer blend state");
    const float duration = float (clip.rawFrameCount) / clip.rawRate;
    const float window = std::min (duration * 0.5f, blendTime);
    const bool rampAvailable = std::min (duration, blendTime) > 1.1920929e-07f;
    float weight = baseWeight;
    if (blendInActive) {
	const float factor = rampAvailable ? std::min (1.0f, playback.time / window) : 1.0f;
	weight *= factor;
	if (factor >= 1.0f && clip.mode != "single") blendInActive = false;
    }
    if (blendOutActive && rampAvailable)
	weight *= std::min (1.0f, (duration - playback.time) / window);
    return weight;
}

inline PuppetPoseSample puppetRawPoseSample (const std::array<float, 9>& sample) {
    // Native 140264188-1f1 halves Euler XYZ, then builds qz*qy*qx from
    // cos/sin; the nine payload floats are T.xyz, Euler.xyz, S.xyz.
    const float cx = std::cos (sample[3] * 0.5f), sx = std::sin (sample[3] * 0.5f);
    const float cy = std::cos (sample[4] * 0.5f), sy = std::sin (sample[4] * 0.5f);
    const float cz = std::cos (sample[5] * 0.5f), sz = std::sin (sample[5] * 0.5f);
    const glm::quat rotation (
	cz * cy * cx + sz * sy * sx,
	cz * cy * sx - sz * sy * cx,
	cz * sy * cx + sz * cy * sx,
	sz * cy * cx - cz * sy * sx);
    return {{sample[0], sample[1], sample[2]}, rotation, {sample[6], sample[7], sample[8]}};
}

inline std::vector<PuppetPoseSample> puppetReferencePoseSamples (const PuppetSkeletonData& skeleton) {
    if (!skeleton.optionalRestMatrices.empty () &&
	 skeleton.optionalRestMatrices.size () != skeleton.bones.size ())
	throw std::runtime_error ("Puppet optional rest matrix count mismatch");
    std::vector<PuppetPoseSample> result;
    result.reserve (skeleton.bones.size ());
    for (size_t i = 0; i < skeleton.bones.size (); ++i) {
	const auto& raw = skeleton.optionalRestMatrices.empty () ? skeleton.bones[i].matrix :
	                  skeleton.optionalRestMatrices[i];
	const glm::mat4 matrix = glm::make_mat4 (raw.data ());
	glm::mat3 rotation;
	glm::vec3 scale;
	for (size_t column = 0; column < 3; ++column) {
	    const glm::vec3 axis = glm::vec3 (matrix[column]);
	    scale[column] = glm::length (axis);
	    if (!std::isfinite (scale[column]) || scale[column] == 0.0f)
		throw std::runtime_error ("Invalid puppet rest axis");
	    rotation[column] = axis / scale[column];
	}
	result.push_back ({glm::vec3 (matrix[3]), glm::normalize (glm::quat_cast (rotation)), scale});
    }
    return result;
}

inline glm::mat4 puppetPoseMatrix (const PuppetPoseSample& pose) {
    return glm::translate (glm::mat4 (1.0f), pose.position) *
	   glm::mat4_cast (pose.rotation) * glm::scale (glm::mat4 (1.0f), pose.scale);
}

inline std::vector<glm::mat4> puppetPoseMatrices (const std::vector<PuppetPoseSample>& pose) {
    std::vector<glm::mat4> matrices;
    matrices.reserve (pose.size ());
    for (const auto& bone : pose) matrices.push_back (puppetPoseMatrix (bone));
    return matrices;
}

inline glm::mat4 puppetRawSampleMatrix (const std::array<float, 9>& sample) {
    return puppetPoseMatrix (puppetRawPoseSample (sample));
}

inline glm::quat puppetShortestNlerp (glm::quat first, glm::quat second, float weight) {
    if (!std::isfinite (weight))
	throw std::runtime_error ("Invalid puppet interpolation weight");
    if (glm::dot (first, second) < 0.0f) second = -second;
    return glm::normalize (first * (1.0f - weight) + second * weight);
}

inline PuppetPoseSample puppetBlendPoseSamples (
    const PuppetPoseSample& previous, const PuppetPoseSample& next, float weight
) {
    return {glm::mix (previous.position, next.position, weight),
	    puppetShortestNlerp (previous.rotation, next.rotation, weight),
	    glm::mix (previous.scale, next.scale, weight)};
}

inline PuppetPoseSample puppetAdditivePoseSample (
    const PuppetPoseSample& previous, const PuppetPoseSample& next,
    const PuppetPoseSample& reference, float weight
) {
    // Native 1401f9820 applies reference-relative translation and scale
    // deltas, and composes previousQ * nlerp(identity, inverseRefQ * nextQ).
    return {previous.position + (next.position - reference.position) * weight,
	    previous.rotation * puppetShortestNlerp (
		glm::quat (1.0f, 0.0f, 0.0f, 0.0f),
		glm::conjugate (reference.rotation) * next.rotation, weight),
	    previous.scale + (next.scale - reference.scale) * weight};
}

inline std::vector<PuppetPoseSample> puppetPoseAtFrames (
    const PuppetClipHeader& clip, size_t firstFrame, size_t nextFrame, float frameWeight
) {
    std::vector<PuppetPoseSample> result;
    result.reserve (clip.tracks.size ());
    for (const auto& track : clip.tracks) {
	if (firstFrame >= track.rawSamples.size () || nextFrame >= track.rawSamples.size ())
	    throw std::runtime_error ("Puppet sample index out of range");
	result.push_back (puppetBlendPoseSamples (
	    puppetRawPoseSample (track.rawSamples[firstFrame]),
	    puppetRawPoseSample (track.rawSamples[nextFrame]), frameWeight));
    }
    return result;
}

inline std::vector<PuppetPoseSample> puppetApplyClipLayer (
    const PuppetSkeletonData& skeleton, const PuppetClipHeader& clip,
    const std::vector<PuppetPoseSample>& previous,
    const std::vector<PuppetPoseSample>& reference,
    const PuppetFrameSelection& frames, float layerWeight, bool additive
) {
    if (clip.tracks.size () != skeleton.bones.size () || previous.size () != clip.tracks.size () ||
	 reference.size () != clip.tracks.size ())
	throw std::runtime_error ("Puppet layer bone count mismatch");
    const auto sampled = puppetPoseAtFrames (clip, frames.first, frames.next, frames.weight);
    std::vector<PuppetPoseSample> result;
    result.reserve (sampled.size ());
    for (size_t bone = 0; bone < sampled.size (); ++bone) {
	if ((skeleton.bones[bone].rawFlags & 2) != 0) {
	    if (!skeleton.boneMappingKnown || skeleton.mappingRecordByBone.size () != sampled.size ())
		throw std::runtime_error ("Unresolved puppet flag-2 bone mapping table");
	    if (skeleton.mappingRecordByBone[bone] >= 0)
		throw std::runtime_error ("Unsupported mapped puppet flag-2 bone constraints");
	    // Native 140261880:1905-1914 forces track bit 0 for an unmapped
	    // flag-2 bone, preserving its previous pose for this layer.
	    result.push_back (previous[bone]);
	    continue;
	}
	if ((clip.tracks[bone].rawFlags & 1) != 0) {
	    result.push_back (previous[bone]);
	    continue;
	}
	result.push_back (additive ? puppetAdditivePoseSample (previous[bone], sampled[bone], reference[bone], layerWeight) :
	                           puppetBlendPoseSamples (previous[bone], sampled[bone], layerWeight));
    }
    return result;
}

inline std::vector<glm::mat4> puppetLocalPoseAtSample (const PuppetClipHeader& clip, size_t sampleIndex) {
    std::vector<glm::mat4> local;
    local.reserve (clip.tracks.size ());
    for (const auto& track : clip.tracks) {
	if (sampleIndex >= track.rawSamples.size ())
	    throw std::runtime_error ("Puppet sample index out of range");
	local.push_back (puppetRawSampleMatrix (track.rawSamples[sampleIndex]));
    }
    return local;
}

inline std::vector<glm::mat4> puppetLocalBindMatrices (const PuppetSkeletonData& skeleton) {
    std::vector<glm::mat4> local;
    local.reserve (skeleton.bones.size ());
    for (const auto& bone : skeleton.bones)
        local.push_back (glm::make_mat4 (bone.matrix.data ()));
    return local;
}

inline std::vector<glm::mat4> puppetUnanimatedLocalMatrices (const PuppetSkeletonData& skeleton) {
    if (skeleton.optionalRestMatrices.empty ()) return puppetLocalBindMatrices (skeleton);
    if (skeleton.optionalRestMatrices.size () != skeleton.bones.size ())
	throw std::runtime_error ("Puppet optional rest matrix count mismatch");
    std::vector<glm::mat4> local;
    local.reserve (skeleton.optionalRestMatrices.size ());
    for (const auto& matrix : skeleton.optionalRestMatrices)
	local.push_back (glm::make_mat4 (matrix.data ()));
    return local;
}

inline std::vector<glm::mat4> puppetGlobalMatrices (
    const PuppetSkeletonData& skeleton, const std::vector<glm::mat4>& local
) {
    if (local.size () != skeleton.bones.size ()) throw std::runtime_error ("Puppet pose bone count mismatch");
    std::vector<glm::mat4> global (local.size ());
    for (size_t i = 0; i < local.size (); ++i) {
        const int32_t parent = skeleton.bones[i].parentIndex;
        if (parent < -1 || parent >= static_cast<int32_t> (i))
            throw std::runtime_error ("Puppet bones must be parent-first and acyclic");
        global[i] = parent < 0 ? local[i] : global[size_t (parent)] * local[i];
    }
    return global;
}

inline std::vector<glm::mat4> puppetInverseBindMatrices (const PuppetSkeletonData& skeleton) {
    const auto bind = puppetGlobalMatrices (skeleton, puppetLocalBindMatrices (skeleton));
    std::vector<glm::mat4> inverses;
    inverses.reserve (bind.size ());
    for (size_t i = 0; i < bind.size (); ++i) {
        const float determinant = glm::determinant (bind[i]);
        if (!std::isfinite (determinant) || determinant == 0.0f)
            throw std::runtime_error ("Noninvertible puppet bind matrix");
        const glm::mat4 inverse = glm::inverse (bind[i]);
        for (size_t element = 0; element < 16; ++element) {
            if (!std::isfinite (glm::value_ptr (inverse)[element]))
                throw std::runtime_error ("Nonfinite inverse puppet bind matrix");
        }
        inverses.push_back (inverse);
    }
    return inverses;
}

inline std::vector<glm::mat4> puppetSkinPalette (
    const PuppetSkeletonData& skeleton, const std::vector<glm::mat4>& currentLocal,
    const std::vector<glm::mat4>& inverseBind
) {
    const auto current = puppetGlobalMatrices (skeleton, currentLocal);
    if (inverseBind.size () != current.size ())
	throw std::runtime_error ("Puppet inverse bind count mismatch");
    std::vector<glm::mat4> palette;
    palette.reserve (current.size ());
    for (size_t i = 0; i < current.size (); ++i) palette.push_back (current[i] * inverseBind[i]);
    return palette;
}

/** Native image-emission sample byte 3 selects one bone; 0xff keeps the
 * caller's existing transform. This matrix is separate from MDAT offsets. */
inline std::optional<glm::mat4> puppetEmissionBoneMatrix (
    const std::vector<glm::mat4>& currentGlobals,
    const std::vector<glm::mat4>& inverseBind, uint8_t boneIndex
) {
    if (boneIndex == 0xff) return std::nullopt;
    if (boneIndex >= inverseBind.size ())
        return std::nullopt;
    // Native 1d4360/1d4400 substitute identity when the pose is unavailable.
    return (boneIndex < currentGlobals.size () ? currentGlobals[boneIndex] : glm::mat4 (1))
        * inverseBind[boneIndex];
}

inline std::vector<glm::mat4> puppetEmissionBindGlobals (const PuppetSkeletonData& skeleton) {
    // 1d6d30 uses optional rest matrices for emission without changing the
    // bind matrices used by the render skin palette.
    return puppetGlobalMatrices (skeleton, puppetUnanimatedLocalMatrices (skeleton));
}

inline std::vector<glm::mat4> puppetEmissionInverseBindMatrices (const PuppetSkeletonData& skeleton) {
    const auto bind = puppetEmissionBindGlobals (skeleton);
    std::vector<glm::mat4> inverse;
    inverse.reserve (bind.size ());
    for (const auto& matrix : bind) {
        const float determinant = glm::determinant (matrix);
        if (!std::isfinite (determinant) || determinant == 0)
            throw std::runtime_error ("Noninvertible puppet emission bind matrix");
        inverse.push_back (glm::inverse (matrix));
    }
    return inverse;
}

struct PuppetEmissionWorldHistory {
    std::vector<glm::mat4> current;
    std::vector<glm::mat4> previous;

    // 1fdf90 swaps scene-owned world-bone buffers each frame independently
    // of positive emission attempts. A skeleton resize discards history.
    void advance (const glm::mat4& world, std::span<const glm::mat4> localGlobals) {
        if (current.size () != localGlobals.size ()) {
            current.clear ();
            previous.clear ();
        }
        current.swap (previous);
        current.resize (localGlobals.size ());
        for (size_t bone = 0; bone < localGlobals.size (); ++bone)
            current[bone] = world * localGlobals[bone];
    }
};

inline std::vector<glm::mat4> puppetSkinPalette (
    const PuppetSkeletonData& skeleton, const std::vector<glm::mat4>& currentLocal
) {
    return puppetSkinPalette (skeleton, currentLocal, puppetInverseBindMatrices (skeleton));
}

inline glm::vec3 puppetSkinnedPosition (
    const PuppetMeshData& mesh, size_t vertex, const std::vector<glm::mat4>& palette
) {
    if (vertex >= mesh.positions.size () || vertex >= mesh.blendIndices.size () ||
        vertex >= mesh.blendWeights.size ()) throw std::runtime_error ("Invalid puppet vertex");
    const auto& p = mesh.positions[vertex];
    const glm::vec4 position (p[0], p[1], p[2], 1.0f);
    if ((mesh.vertexMask & 0x01000000) == 0) return glm::vec3 (position);
    glm::vec3 result (0.0f);
    for (size_t lane = 0; lane < 4; ++lane) {
        const float weight = mesh.blendWeights[vertex][lane];
        if (weight == 0.0f) continue;
        const uint32_t bone = mesh.blendIndices[vertex][lane];
        if (bone >= palette.size ()) throw std::runtime_error ("Puppet weighted bone index out of range");
        result += glm::vec3 (palette[bone] * position) * weight;
    }
    return result;
}

} // namespace WallpaperEngine::Render::Objects
