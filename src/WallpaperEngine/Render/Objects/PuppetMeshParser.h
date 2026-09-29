#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Objects {

// Bounded MDLV0004/0013/0014/0016/0017/0019/0021/0023 mesh subset. Later MDLS
// skeleton and animation records are parsed separately.
struct PuppetMeshData {
    struct BoneRange {
        uint32_t boneIndex = 0;
        uint32_t rawFlags = 0;
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
    };
    int version = 0;
    uint32_t vertexMask = 0;
    uint32_t meshFlags = 0; // Native per-mesh flags; bit 1 supplies an extra field.
    uint32_t blendRowCount = 0; // Present when meshFlags bit 1 is set.
    std::string material;
    std::vector<std::string> materials; // Native reads this list for each mesh.
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 4>> tangents;
    std::vector<std::array<uint32_t, 4>> blendIndices;
    std::vector<std::array<float, 4>> blendWeights;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<std::array<float, 4>> texcoordsFull; // Preserves float4 TEXCOORD layouts.
    std::vector<uint16_t> indices;
    // MDLV0021+ optional vertex-position stream. Native reads a stream count
    // followed by a sized float3 payload; the observed assets contain one
    // position per mesh vertex. Keep it distinct from render positions.
    uint32_t optionalPositionStreamCount = 0;
    std::vector<std::array<float, 3>> optionalPositions;
    // The second sized stream holds per-bone triangle-index ranges in the
    // observed v21 models. Its raw flag is retained for later consumers.
    std::vector<BoneRange> boneRanges;
    size_t payloadEndOffset = 0; // Before version-specific tail records and MDLS.
};

struct PuppetMeshesData {
    int version = 0;
    std::vector<PuppetMeshData> meshes;
    size_t sectionEndOffset = 0; // Immediately before MDLS after all mesh tails.
};

struct PuppetBoneRecord {
    std::string name;
    uint32_t rawFlags = 0;
    int32_t parentIndex = -1;
    std::array<float, 16> matrix {}; // Native local bind transform, column-major.
    std::string metadata; // Native reads a trailing C-string before the next bone.
};

struct PuppetSkeletonData {
    int version = 0;
    size_t sectionEndOffset = 0;
    size_t boneRecordsEndOffset = 0;
    uint16_t auxiliaryTransformCount = 0;
    uint32_t scalarTrackCount = 0;
    // Native bone+d4 is -1 until a later MDLS mapping record names that bone.
    // Only the zero-scalar/zero-group subset of that tail is decoded here.
    bool boneMappingKnown = false;
    std::vector<int32_t> mappingRecordByBone;
    std::vector<PuppetBoneRecord> bones;
    std::vector<std::array<float, 16>> optionalRestMatrices;
};

struct PuppetClipHeader {
    struct ScalarTrack {
        uint32_t rawFlags = 0;
        std::vector<float> rawSamples;
    };
    struct MeshChannel {
        uint16_t rawIndex = 0;
        ScalarTrack values;
    };
    struct MeshTrack {
        uint32_t rawFlags = 0;
        uint32_t rawField = 0;
        std::vector<MeshChannel> channels;
    };
    struct Event {
        uint32_t rawField = 0;
        std::string name;
    };
    uint64_t rawId = 0;
    std::string name;
    std::string mode;
    float rawRate = 0.0f;
    uint32_t rawFrameCount = 0;
    uint32_t rawField = 0;
    uint32_t trackCount = 0;
    size_t tracksOffset = 0;
    struct Track {
        uint32_t rawFlags = 0;
        std::vector<std::array<float, 9>> rawSamples;
    };
    std::vector<Track> tracks;
    size_t boneTracksEndOffset = 0; // Additional clip records may follow.
    std::vector<Track> auxiliaryTransforms;
    std::vector<ScalarTrack> scalarTracks;
    std::vector<ScalarTrack> extraScalarTracks;
    std::vector<ScalarTrack> boneScalarsA;
    std::vector<MeshTrack> meshTracks;
    std::array<uint32_t, 6> versionFiveFields {};
    std::vector<ScalarTrack> boneScalarsB;
    std::vector<Event> events;
    size_t endOffset = 0;
};

struct PuppetAnimationHeader {
    struct Attachment {
        uint16_t rawIndex = 0;
        std::string name;
        std::array<float, 16> matrix {};
    };
    std::vector<Attachment> attachments; // Optional MDAT0001 section before MDLA.
    int version = 0;
    size_t sectionEndOffset = 0;
    uint32_t clipCount = 0;
    std::vector<PuppetClipHeader> clips;
};

PuppetMeshData parsePuppetFirstMesh (std::span<const uint8_t> bytes);
PuppetMeshesData parsePuppetMeshes (std::span<const uint8_t> bytes);
PuppetSkeletonData parsePuppetFirstSkeleton (std::span<const uint8_t> bytes, const PuppetMeshData& mesh);
PuppetSkeletonData parsePuppetSkeleton (std::span<const uint8_t> bytes, const PuppetMeshesData& model);
PuppetAnimationHeader parsePuppetFirstAnimationHeader (
    std::span<const uint8_t> bytes, const PuppetSkeletonData& skeleton
);

} // namespace WallpaperEngine::Render::Objects
