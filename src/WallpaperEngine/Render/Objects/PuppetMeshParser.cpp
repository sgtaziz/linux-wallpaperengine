#include "PuppetMeshParser.h"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string_view>

using namespace WallpaperEngine::Render::Objects;

namespace {
class Cursor {
public:
    explicit Cursor (std::span<const uint8_t> bytes, size_t offset = 0) : m_bytes (bytes), m_offset (offset) {
	if (offset > bytes.size ()) throw std::runtime_error ("Invalid MDLV cursor offset");
    }

    size_t offset () const { return m_offset; }
    size_t remaining () const { return m_bytes.size () - m_offset; }

    std::span<const uint8_t> take (size_t count) {
	if (count > remaining ()) throw std::runtime_error ("Truncated MDLV mesh data");
	const auto result = m_bytes.subspan (m_offset, count);
	m_offset += count;
	return result;
    }

    uint32_t u32 () {
	const auto bytes = take (4);
	return uint32_t (bytes[0]) | (uint32_t (bytes[1]) << 8) |
	       (uint32_t (bytes[2]) << 16) | (uint32_t (bytes[3]) << 24);
    }

    uint64_t u64 () {
	const uint64_t low = u32 ();
	return low | (uint64_t (u32 ()) << 32);
    }

    uint8_t u8 () { return take (1)[0]; }

    uint16_t u16 () {
	const auto bytes = take (2);
	return uint16_t (bytes[0]) | (uint16_t (bytes[1]) << 8);
    }

    std::string cstring () {
	const size_t start = m_offset;
	while (m_offset < m_bytes.size () && m_bytes[m_offset] != 0) ++m_offset;
	if (m_offset == m_bytes.size ()) throw std::runtime_error ("Unterminated MDLV string");
	const auto length = m_offset++ - start;
	return std::string (reinterpret_cast<const char*> (m_bytes.data () + start), length);
    }

private:
    std::span<const uint8_t> m_bytes;
    size_t m_offset = 0;
};

uint32_t readU32 (std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t (bytes[offset]) | (uint32_t (bytes[offset + 1]) << 8) |
	   (uint32_t (bytes[offset + 2]) << 16) | (uint32_t (bytes[offset + 3]) << 24);
}

float readFloat (std::span<const uint8_t> bytes, size_t offset) {
    return std::bit_cast<float> (readU32 (bytes, offset));
}
PuppetMeshData parseMesh (Cursor& cursor, int version, uint32_t headerFlags, uint32_t materialCount) {
    PuppetMeshData result;
    result.version = version;
    for (uint32_t i = 0; i < materialCount; ++i) {
	const std::string material = cursor.cstring ();
	if (i == 0) result.material = material;
	result.materials.push_back (material);
    }
    // Native 140261880 reads this per-mesh field at v4+, an extra uint32 when
    // bit 1 is set, then six floats at v17+ before the attribute mask.
    const uint32_t meshFlags = cursor.u32 ();
    result.meshFlags = meshFlags;
    if (meshFlags & 2) result.blendRowCount = cursor.u32 ();
    if (version >= 17) cursor.take (6 * sizeof (float));
    result.vertexMask = version >= 15 ? cursor.u32 () : headerFlags;
    // Native 1400d7f90's attribute table orders these six semantics:
    // position12, optional normal12/tangent16, optional indices16/weights16,
    // then TEXCOORD float2 (mask8) or float4 (mask0x20).
    constexpr uint32_t knownMask = 0x0180002f;
    const uint32_t texcoordMask = result.vertexMask & 0x28;
    if ((result.vertexMask & 1) == 0 || (texcoordMask != 8 && texcoordMask != 0x20) ||
        (result.vertexMask & ~knownMask) != 0)
	throw std::runtime_error ("Unsupported MDLV vertex attribute mask " +
	                          std::to_string (result.vertexMask));
    const bool hasBlendIndices = (result.vertexMask & 0x00800000) != 0;
    const bool hasBlendWeights = (result.vertexMask & 0x01000000) != 0;
    const size_t blendIndexOffset = 12 + ((result.vertexMask & 2) ? 12 : 0) +
	                            ((result.vertexMask & 4) ? 16 : 0);
    const size_t blendWeightOffset = blendIndexOffset + (hasBlendIndices ? 16 : 0);
    const size_t uvOffset = blendWeightOffset + (hasBlendWeights ? 16 : 0);
    const size_t texcoordComponents = texcoordMask == 8 ? 2 : 4;
    const size_t stride = uvOffset + texcoordComponents * sizeof (float);

    const uint32_t vertexBytes = cursor.u32 ();
    if (vertexBytes == 0 || vertexBytes % stride != 0)
	throw std::runtime_error ("Invalid MDLV vertex byte count");
    const auto vertices = cursor.take (vertexBytes);
    const size_t vertexCount = vertexBytes / stride;
    if (vertexCount > UINT16_MAX + size_t (1))
	throw std::runtime_error ("MDLV mesh exceeds 16-bit index range");
    result.positions.reserve (vertexCount);
    result.normals.reserve (vertexCount);
    result.tangents.reserve (vertexCount);
    result.blendIndices.reserve (vertexCount);
    result.blendWeights.reserve (vertexCount);
    result.texcoords.reserve (vertexCount);
    result.texcoordsFull.reserve (vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
	const size_t offset = i * stride;
	const std::array<float, 3> position {
	    readFloat (vertices, offset), readFloat (vertices, offset + 4), readFloat (vertices, offset + 8)
	};
	std::array<float, 3> normal {};
	if (result.vertexMask & 2)
	    for (size_t lane = 0; lane < 3; ++lane)
		normal[lane] = readFloat (vertices, offset + 12 + lane * 4);
	std::array<float, 4> tangent {};
	if (result.vertexMask & 4)
	    for (size_t lane = 0; lane < 4; ++lane)
		tangent[lane] = readFloat (vertices, offset + 12 + ((result.vertexMask & 2) ? 12 : 0)
		                               + lane * 4);
	std::array<float, 4> uvFull {};
	for (size_t lane = 0; lane < texcoordComponents; ++lane)
	    uvFull[lane] = readFloat (vertices, offset + uvOffset + lane * 4);
	const std::array<float, 2> uv {uvFull[0], uvFull[1]};
	std::array<uint32_t, 4> blendIndices {};
	std::array<float, 4> blendWeights {};
	for (size_t lane = 0; lane < 4; ++lane) {
	    if (hasBlendIndices)
		blendIndices[lane] = readU32 (vertices, offset + blendIndexOffset + lane * 4);
	    if (hasBlendWeights) {
		blendWeights[lane] = readFloat (vertices, offset + blendWeightOffset + lane * 4);
		if (!std::isfinite (blendWeights[lane])) throw std::runtime_error ("Nonfinite MDLV blend weight");
	    }
	}
	if (!std::isfinite (position[0]) || !std::isfinite (position[1]) || !std::isfinite (position[2]) ||
	    !std::isfinite (normal[0]) || !std::isfinite (normal[1]) || !std::isfinite (normal[2]) ||
	    !std::isfinite (tangent[0]) || !std::isfinite (tangent[1]) ||
	    !std::isfinite (tangent[2]) || !std::isfinite (tangent[3]) ||
	    !std::isfinite (uvFull[0]) || !std::isfinite (uvFull[1]) ||
	    !std::isfinite (uvFull[2]) || !std::isfinite (uvFull[3]))
	    throw std::runtime_error ("Nonfinite MDLV position or UV");
	result.positions.push_back (position);
	result.normals.push_back (normal);
	result.tangents.push_back (tangent);
	result.blendIndices.push_back (blendIndices);
	result.blendWeights.push_back (blendWeights);
	result.texcoords.push_back (uv);
	result.texcoordsFull.push_back (uvFull);
    }

    const uint32_t indexBytes = cursor.u32 ();
    if (indexBytes == 0 || indexBytes % (sizeof (uint16_t) * 3) != 0)
	throw std::runtime_error ("Invalid MDLV triangle index byte count");
    const auto indices = cursor.take (indexBytes);
    result.indices.reserve (indexBytes / 2);
    for (size_t i = 0; i < indices.size (); i += 2) {
	const uint16_t index = uint16_t (indices[i]) | (uint16_t (indices[i + 1]) << 8);
	if (index >= vertexCount) throw std::runtime_error ("MDLV triangle index out of range");
	result.indices.push_back (index);
    }
    result.payloadEndOffset = cursor.offset ();
    return result;
}

void parseMeshTail (Cursor& cursor, PuppetMeshData& mesh) {
    if (mesh.version >= 21) {
        if (cursor.u8 () != 0) {
            mesh.optionalPositionStreamCount = cursor.u32 ();
            const uint32_t length = cursor.u32 ();
            const size_t vertexCount = mesh.positions.size ();
            if (mesh.optionalPositionStreamCount != 1 ||
                vertexCount > UINT32_MAX / (3 * sizeof (float)) ||
                length != vertexCount * 3 * sizeof (float))
                throw std::runtime_error ("Unsupported MDLV optional position stream layout");
            const auto payload = cursor.take (length);
            mesh.optionalPositions.reserve (vertexCount);
            for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
                std::array<float, 3> position {};
                for (size_t lane = 0; lane < 3; ++lane) {
                    position[lane] = readFloat (payload, (vertex * 3 + lane) * sizeof (float));
                    if (!std::isfinite (position[lane]))
                        throw std::runtime_error ("Nonfinite MDLV optional position");
                }
                mesh.optionalPositions.push_back (position);
            }
        }
        if (cursor.u8 () != 0) {
            const uint32_t length = cursor.u32 ();
            if (length % 16 != 0)
                throw std::runtime_error ("Unsupported MDLV bone-range record layout");
            const auto payload = cursor.take (length);
            mesh.boneRanges.reserve (length / 16);
            for (size_t offset = 0; offset < length; offset += 16) {
                const PuppetMeshData::BoneRange range {
                    readU32 (payload, offset), readU32 (payload, offset + 4),
                    readU32 (payload, offset + 8), readU32 (payload, offset + 12)
                };
                if (range.firstIndex > mesh.indices.size () ||
                    range.indexCount > mesh.indices.size () - range.firstIndex ||
                    range.firstIndex % 3 != 0 || range.indexCount % 3 != 0)
                    throw std::runtime_error ("Invalid MDLV bone triangle range");
                mesh.boneRanges.push_back (range);
            }
        }
    }
    if (mesh.version >= 23 && cursor.u32 () != 0)
        throw std::runtime_error ("Unsupported MDLV v23 tail records");
}

PuppetSkeletonData parseSkeletonAt (std::span<const uint8_t> bytes, size_t offset,
                                   std::span<const PuppetMeshData> meshes) {
    Cursor tail (bytes, offset);
    const std::string magic = tail.cstring ();
    if (magic != "MDLS0001" && magic != "MDLS0002" && magic != "MDLS0003" && magic != "MDLS0004")
	throw std::runtime_error ("Unsupported MDLS version");
    PuppetSkeletonData result;
    result.version = magic[7] - '0';
    result.sectionEndOffset = tail.u32 ();
    if (result.sectionEndOffset < tail.offset () || result.sectionEndOffset > bytes.size ())
	throw std::runtime_error ("Invalid MDLS section bound");
    Cursor section (bytes.first (result.sectionEndOffset), tail.offset ());
    const uint32_t boneCount = section.u32 ();
    if (boneCount > 128) throw std::runtime_error ("MDLS bone count exceeds native bound");
    result.bones.reserve (boneCount);
    for (uint32_t i = 0; i < boneCount; ++i) {
	PuppetBoneRecord bone;
	bone.name = section.cstring ();
	bone.rawFlags = section.u32 ();
	bone.parentIndex = std::bit_cast<int32_t> (section.u32 ());
	const uint32_t payloadLength = section.u32 ();
	if (payloadLength != 64) throw std::runtime_error ("Unsupported MDLS bone matrix length");
	const auto payload = section.take (payloadLength);
	for (size_t j = 0; j < bone.matrix.size (); ++j) {
	    bone.matrix[j] = readFloat (payload, j * sizeof (float));
	    if (!std::isfinite (bone.matrix[j])) throw std::runtime_error ("Nonfinite MDLS bone matrix");
	}
	if (bone.parentIndex < -1 || bone.parentIndex >= static_cast<int32_t> (boneCount))
	    throw std::runtime_error ("MDLS parent index out of range");
	bone.metadata = section.cstring ();
	result.bones.push_back (std::move (bone));
    }
    result.boneRecordsEndOffset = section.offset ();
    if (result.version >= 2) {
	// Native 140261880:801-980 walks these post-bone records before MDLA.
	result.auxiliaryTransformCount = section.u16 ();
	if (result.auxiliaryTransformCount > section.remaining () / (1 + 4 + 4 + 64))
	    throw std::runtime_error ("Truncated MDLS auxiliary transforms");
	for (uint16_t i = 0; i < result.auxiliaryTransformCount; ++i) {
	    section.cstring ();
	    section.u32 ();
	    section.u32 ();
	    section.take (64);
	}
	if (section.u8 () != 0) {
	    if (result.bones.size () > section.remaining () / 64)
		throw std::runtime_error ("Truncated MDLS extra bone matrices");
	    result.optionalRestMatrices.reserve (result.bones.size ());
	    for (size_t i = 0; i < result.bones.size (); ++i) {
		std::array<float, 16> matrix {};
		const auto payload = section.take (64);
		for (size_t j = 0; j < matrix.size (); ++j) {
		    matrix[j] = readFloat (payload, j * sizeof (float));
		    if (!std::isfinite (matrix[j]))
			throw std::runtime_error ("Nonfinite MDLS optional rest matrix");
		}
		result.optionalRestMatrices.push_back (matrix);
	    }
	    if (result.auxiliaryTransformCount > section.remaining () / 64)
		throw std::runtime_error ("Truncated MDLS extra auxiliary matrices");
	    section.take (size_t (result.auxiliaryTransformCount) * 64);
	}
	result.scalarTrackCount = section.u32 ();
	if (result.scalarTrackCount == 0) {
	    // Native 140261880:1080-1220 reads a group table followed by the
	    // mapping records that assign each bone's +d4 field. The nonempty
	    // group/constraint payload remains outside this bounded subset.
	    const uint16_t groupCount = section.u16 ();
	    if (groupCount == 0) {
		const uint16_t mappingCount = section.u16 ();
		result.mappingRecordByBone.assign (boneCount, -1);
		for (uint16_t i = 0; i < mappingCount; ++i) {
		    const uint32_t boneIndex = section.u32 ();
		    if (boneIndex >= boneCount)
			throw std::runtime_error ("MDLS mapping bone index out of range");
		    const uint32_t auxiliaryCount = section.u32 ();
		    if (auxiliaryCount > section.remaining () / sizeof (uint32_t))
			throw std::runtime_error ("Truncated MDLS mapping auxiliary indices");
		    const auto auxiliaryIndices = section.take (size_t (auxiliaryCount) * sizeof (uint32_t));
		    for (uint32_t j = 0; j < auxiliaryCount; ++j)
			if (readU32 (auxiliaryIndices, size_t (j) * sizeof (uint32_t)) >=
			    result.auxiliaryTransformCount)
			    throw std::runtime_error ("MDLS mapping auxiliary index out of range");
		    if (section.u16 () != 0)
			throw std::runtime_error ("Unsupported MDLS nested mapping constraints");
		    result.mappingRecordByBone[boneIndex] = i;
		}
		result.boneMappingKnown = true;
		// 140261880:1580 reads one extent+matrix record per bone after
		// the mapping tail. Unknown scalar/group tails remain undecoded.
		if (section.remaining () != 0 && section.u8 () != 0) {
		    if (boneCount > section.remaining () / 76)
		        throw std::runtime_error ("Truncated MDLS emission bounds");
		    result.emissionBounds.resize (boneCount);
		    for (auto& bound : result.emissionBounds) {
		        const auto payload = section.take (76);
		        for (size_t lane = 0; lane < bound.extent.size (); ++lane)
		            bound.extent[lane] = readFloat (payload, lane * 4);
		        for (size_t lane = 0; lane < bound.matrix.size (); ++lane)
		            bound.matrix[lane] = readFloat (payload, 12 + lane * 4);
		        for (float value : bound.extent)
		            if (!std::isfinite (value)) throw std::runtime_error ("Nonfinite MDLS emission extent");
		        for (float value : bound.matrix)
		            if (!std::isfinite (value)) throw std::runtime_error ("Nonfinite MDLS emission matrix");
		    }
		}
	    }
	}
    }
    for (const auto& mesh : meshes) {
        for (const auto& range : mesh.boneRanges)
            if (range.boneIndex >= boneCount)
                throw std::runtime_error ("MDLV bone range index out of range");
        for (size_t vertex = 0; vertex < mesh.blendWeights.size (); ++vertex) {
            for (size_t lane = 0; lane < 4; ++lane) {
                if (mesh.blendWeights[vertex][lane] != 0.0f && mesh.blendIndices[vertex][lane] >= boneCount)
                    throw std::runtime_error ("MDLV weighted bone index out of range");
            }
        }
    }
    return result;
}


}

PuppetMeshesData WallpaperEngine::Render::Objects::parsePuppetMeshes (std::span<const uint8_t> bytes) {
    Cursor cursor (bytes);
    const std::string magic = cursor.cstring ();
    if (magic.size () != 8 || !std::string_view (magic).starts_with ("MDLV"))
	throw std::runtime_error ("Invalid MDLV header");
    int version = 0;
    for (size_t i = 4; i < 8; ++i) {
	if (magic[i] < '0' || magic[i] > '9') throw std::runtime_error ("Invalid MDLV version");
	version = version * 10 + (magic[i] - '0');
    }
    if (version != 4 && version != 13 && version != 14 && version != 16 && version != 17 && version != 19 &&
        version != 21 && version != 23)
	throw std::runtime_error ("Unsupported MDLV version " + std::to_string (version));

    const uint32_t headerFlags = cursor.u32 ();
    const uint32_t materialCount = cursor.u32 ();
    const uint32_t meshCount = cursor.u32 ();
    if (meshCount == 0 || meshCount > cursor.remaining () / 13)
        throw std::runtime_error ("Invalid MDLV mesh count");
    PuppetMeshesData result;
    result.version = version;
    result.meshes.reserve (meshCount);
    for (uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex) {
        if (materialCount > cursor.remaining ())
            throw std::runtime_error ("Truncated MDLV material list");
        result.meshes.push_back (parseMesh (cursor, version, headerFlags, materialCount));
        parseMeshTail (cursor, result.meshes.back ());
    }
    result.sectionEndOffset = cursor.offset ();
    return result;
}

PuppetMeshData WallpaperEngine::Render::Objects::parsePuppetFirstMesh (std::span<const uint8_t> bytes) {
    Cursor cursor (bytes);
    const std::string magic = cursor.cstring ();
    if (magic.size () != 8 || !std::string_view (magic).starts_with ("MDLV"))
        throw std::runtime_error ("Invalid MDLV header");
    int version = 0;
    for (size_t i = 4; i < 8; ++i) {
        if (magic[i] < '0' || magic[i] > '9') throw std::runtime_error ("Invalid MDLV version");
        version = version * 10 + (magic[i] - '0');
    }
    if (version != 4 && version != 13 && version != 14 && version != 16 && version != 17 && version != 19 &&
        version != 21 && version != 23)
        throw std::runtime_error ("Unsupported MDLV version " + std::to_string (version));
    const uint32_t headerFlags = cursor.u32 ();
    const uint32_t materialCount = cursor.u32 ();
    const uint32_t meshCount = cursor.u32 ();
    if (meshCount != 1)
        throw std::runtime_error ("Multi-mesh puppet rendering is not supported");
    if (materialCount > cursor.remaining ())
        throw std::runtime_error ("Truncated MDLV material list");
    return parseMesh (cursor, version, headerFlags, materialCount);
}

PuppetSkeletonData WallpaperEngine::Render::Objects::parsePuppetFirstSkeleton (
    std::span<const uint8_t> bytes, const PuppetMeshData& mesh
) {
    Cursor tail (bytes, mesh.payloadEndOffset);
    PuppetMeshData parsed = mesh;
    parseMeshTail (tail, parsed);

    return parseSkeletonAt (bytes, tail.offset (), {&parsed, 1});
}

PuppetSkeletonData WallpaperEngine::Render::Objects::parsePuppetSkeleton (
    std::span<const uint8_t> bytes, const PuppetMeshesData& model
) {
    return parseSkeletonAt (bytes, model.sectionEndOffset, model.meshes);
}

PuppetAnimationHeader WallpaperEngine::Render::Objects::parsePuppetFirstAnimationHeader (
    std::span<const uint8_t> bytes, const PuppetSkeletonData& skeleton
) {
    Cursor cursor (bytes, skeleton.sectionEndOffset);
    PuppetAnimationHeader result;
    std::string magic = cursor.cstring ();
    if (magic == "MDAT0001") {
	const uint32_t attachmentEnd = cursor.u32 ();
	if (attachmentEnd < cursor.offset () || attachmentEnd > bytes.size ())
	    throw std::runtime_error ("Invalid MDAT section bound");
	Cursor attachments (bytes.first (attachmentEnd), cursor.offset ());
	const uint16_t count = attachments.u16 ();
	if (count > attachments.remaining () / (2 + 1 + 64))
	    throw std::runtime_error ("Truncated MDAT attachments");
	result.attachments.reserve (count);
	for (uint16_t i = 0; i < count; ++i) {
	    PuppetAnimationHeader::Attachment attachment;
	    attachment.rawIndex = attachments.u16 ();
	    attachment.name = attachments.cstring ();
	    const auto matrix = attachments.take (64);
	    for (size_t j = 0; j < attachment.matrix.size (); ++j) {
		attachment.matrix[j] = readFloat (matrix, j * sizeof (float));
		if (!std::isfinite (attachment.matrix[j]))
		    throw std::runtime_error ("Nonfinite MDAT attachment matrix");
	    }
	    result.attachments.push_back (std::move (attachment));
	}
	if (attachments.offset () != attachmentEnd)
	    throw std::runtime_error ("Unsupported MDAT trailing records");
	cursor = Cursor (bytes, attachmentEnd);
	magic = cursor.cstring ();
    }
    if (magic.size () != 8 || !std::string_view (magic).starts_with ("MDLA000") ||
        magic[7] < '1' || magic[7] > '6')
	throw std::runtime_error ("Unsupported MDLA version");
    result.version = magic[7] - '0';
    result.sectionEndOffset = cursor.u32 ();
    if (result.sectionEndOffset < cursor.offset () || result.sectionEndOffset > bytes.size ())
	throw std::runtime_error ("Invalid MDLA section bound");
    Cursor section (bytes.first (result.sectionEndOffset), cursor.offset ());
    result.clipCount = section.u32 ();
    if (result.clipCount > section.remaining () / 26)
	throw std::runtime_error ("Truncated MDLA clip headers");
    result.clips.reserve (result.clipCount);
    for (uint32_t clipIndex = 0; clipIndex < result.clipCount; ++clipIndex) {
	PuppetClipHeader clip;
	clip.rawId = section.u64 ();
	clip.name = section.cstring ();
	clip.mode = section.cstring ();
	clip.rawRate = std::bit_cast<float> (section.u32 ());
	clip.rawFrameCount = section.u32 ();
	clip.rawField = section.u32 ();
	clip.trackCount = section.u32 ();
	clip.tracksOffset = section.offset ();
	if (!std::isfinite (clip.rawRate)) throw std::runtime_error ("Nonfinite MDLA rate field");
	auto readTransformTrack = [&] () {
	    PuppetClipHeader::Track track;
	    track.rawFlags = section.u32 ();
	    const uint32_t byteLength = section.u32 ();
	    if (byteLength % (9 * sizeof (float)) != 0 ||
	        uint64_t (byteLength / (9 * sizeof (float))) != uint64_t (clip.rawFrameCount) + 1)
		throw std::runtime_error ("Invalid MDLA transform sample byte count");
	    const auto payload = section.take (byteLength);
	    track.rawSamples.reserve (byteLength / (9 * sizeof (float)));
	    for (size_t offset = 0; offset < payload.size (); offset += 9 * sizeof (float)) {
		std::array<float, 9> sample {};
		for (size_t lane = 0; lane < sample.size (); ++lane) {
		    sample[lane] = readFloat (payload, offset + lane * sizeof (float));
		    if (!std::isfinite (sample[lane])) throw std::runtime_error ("Nonfinite MDLA transform sample");
		}
		track.rawSamples.push_back (sample);
	    }
	    return track;
	};
	auto readScalarTrack = [&] (bool hasFlags) {
	    PuppetClipHeader::ScalarTrack track;
	    if (hasFlags) track.rawFlags = section.u32 ();
	    const uint32_t byteLength = section.u32 ();
	    if (byteLength % sizeof (float) != 0 ||
	        uint64_t (byteLength / sizeof (float)) != uint64_t (clip.rawFrameCount) + 1)
		throw std::runtime_error ("Invalid MDLA scalar sample byte count");
	    const auto payload = section.take (byteLength);
	    track.rawSamples.reserve (byteLength / sizeof (float));
	    for (size_t offset = 0; offset < payload.size (); offset += sizeof (float)) {
		const float sample = readFloat (payload, offset);
		if (!std::isfinite (sample)) throw std::runtime_error ("Nonfinite MDLA scalar sample");
		track.rawSamples.push_back (sample);
	    }
	    return track;
	};
	if (clip.trackCount > section.remaining () / 8)
	    throw std::runtime_error ("Truncated MDLA bone track headers");
	clip.tracks.reserve (clip.trackCount);
	for (uint32_t i = 0; i < clip.trackCount; ++i) clip.tracks.push_back (readTransformTrack ());
	clip.boneTracksEndOffset = section.offset ();
	if (result.version >= 2) {
	    if (skeleton.auxiliaryTransformCount > section.remaining () / 8)
		throw std::runtime_error ("Truncated MDLA auxiliary track headers");
	    clip.auxiliaryTransforms.reserve (skeleton.auxiliaryTransformCount);
	    for (uint16_t i = 0; i < skeleton.auxiliaryTransformCount; ++i)
		clip.auxiliaryTransforms.push_back (readTransformTrack ());
	    if (skeleton.scalarTrackCount > section.remaining () / 8)
		throw std::runtime_error ("Truncated MDLA scalar track headers");
	    clip.scalarTracks.reserve (skeleton.scalarTrackCount);
	    for (uint32_t i = 0; i < skeleton.scalarTrackCount; ++i)
		clip.scalarTracks.push_back (readScalarTrack (true));
	    if (result.version >= 3) {
		const uint32_t extraCount = section.u32 ();
		if (extraCount > section.remaining () / 8)
		    throw std::runtime_error ("Truncated MDLA extra scalar tracks");
		clip.extraScalarTracks.reserve (extraCount);
		for (uint32_t i = 0; i < extraCount; ++i)
		    clip.extraScalarTracks.push_back (readScalarTrack (true));
		if (section.u8 () != 0) {
		    if (clip.trackCount > section.remaining () / 8)
			throw std::runtime_error ("Truncated MDLA bone scalar A tracks");
		    clip.boneScalarsA.reserve (clip.trackCount);
		    for (uint32_t i = 0; i < clip.trackCount; ++i)
			clip.boneScalarsA.push_back (readScalarTrack (true));
		}
	    }
	}
	if (result.version >= 4 && section.u8 () != 0) {
	    // This bounded first-mesh parser has admitted exactly one MDLV mesh.
	    PuppetClipHeader::MeshTrack meshTrack;
	    meshTrack.rawFlags = section.u32 ();
	    if ((meshTrack.rawFlags & 1) != 0) {
		meshTrack.rawField = section.u32 ();
		const uint16_t channelCount = section.u16 ();
		if (channelCount > section.remaining () / 6)
		    throw std::runtime_error ("Truncated MDLA mesh channels");
		meshTrack.channels.reserve (channelCount);
		for (uint16_t i = 0; i < channelCount; ++i) {
		    PuppetClipHeader::MeshChannel channel;
		    channel.rawIndex = section.u16 ();
		    channel.values = readScalarTrack (false);
		    meshTrack.channels.push_back (std::move (channel));
		}
	    }
	    clip.meshTracks.push_back (std::move (meshTrack));
	}
	if (result.version >= 5)
	    for (auto& field : clip.versionFiveFields) field = section.u32 ();
	if (result.version >= 6 && section.u8 () != 0) {
	    if (clip.trackCount > section.remaining () / 8)
		throw std::runtime_error ("Truncated MDLA bone scalar B tracks");
	    clip.boneScalarsB.reserve (clip.trackCount);
	    for (uint32_t i = 0; i < clip.trackCount; ++i)
		clip.boneScalarsB.push_back (readScalarTrack (true));
	}
	const uint32_t eventCount = section.u32 ();
	if (eventCount > section.remaining () / 5)
	    throw std::runtime_error ("Truncated MDLA events");
	clip.events.reserve (eventCount);
	for (uint32_t i = 0; i < eventCount; ++i)
	    clip.events.push_back ({section.u32 (), section.cstring ()});
	clip.endOffset = section.offset ();
	result.clips.push_back (std::move (clip));
    }
    if (section.offset () != result.sectionEndOffset)
	throw std::runtime_error ("Unsupported MDLA trailing records");
    return result;
}
