#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "WallpaperEngine/Render/Objects/PuppetMeshParser.h"
#include "WallpaperEngine/Render/Objects/PuppetSkinning.h"
#include "WallpaperEngine/Render/Objects/StaticModelTail.h"

#include <bit>
#include <cstdint>
#include <limits>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

using WallpaperEngine::Render::Objects::parsePuppetFirstMesh;
using WallpaperEngine::Render::Objects::parsePuppetMeshes;

namespace {
void u32 (std::string& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back (char (value >> shift));
}
void u64 (std::string& out, uint64_t value) {
    u32 (out, uint32_t (value));
    u32 (out, uint32_t (value >> 32));
}
void f32 (std::string& out, float value) { u32 (out, std::bit_cast<uint32_t> (value)); }
void cstr (std::string& out, const std::string& value) { out += value; out.push_back ('\0'); }
void patch32 (std::string& out, size_t at, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out[at + shift / 8] = char (value >> shift);
}
struct Fixture {
    std::string bytes;
    size_t vertexLength = 0;
    size_t vertexPayload = 0;
    size_t indexLength = 0;
    size_t indexPayload = 0;
};
Fixture makeFixture (int version = 23, uint32_t headerFlags = 0x01800009,
                     uint32_t vertexMask = 0x0180000f, uint32_t meshCount = 1,
                     uint32_t meshFlags = 2, uint32_t materialCount = 1) {
    Fixture f;
    cstr (f.bytes, (version < 10 ? "MDLV000" : "MDLV00") + std::to_string (version));
    u32 (f.bytes, headerFlags);
    u32 (f.bytes, materialCount);
    u32 (f.bytes, meshCount);
    if (materialCount) cstr (f.bytes, "material/MDLS_embedded");
    u32 (f.bytes, meshFlags);
    if (meshFlags & 2) u32 (f.bytes, 1); // Native channel-map row count.
    if (version >= 17) for (int i = 0; i < 6; ++i) f32 (f.bytes, float (i));
    if (version >= 15) u32 (f.bytes, vertexMask);
    const size_t blendIndexOffset = 12 + ((vertexMask & 2) ? 12 : 0) + ((vertexMask & 4) ? 16 : 0);
    const bool hasBlendIndices = (vertexMask & 0x00800000) != 0;
    const bool hasBlendWeights = (vertexMask & 0x01000000) != 0;
    const size_t blendWeightOffset = blendIndexOffset + (hasBlendIndices ? 16 : 0);
    const size_t uvOffset = blendWeightOffset + (hasBlendWeights ? 16 : 0);
    const bool uv4 = (vertexMask & 0x20) != 0;
    const size_t stride = uvOffset + (uv4 ? 16 : 8);
    f.vertexLength = f.bytes.size ();
    u32 (f.bytes, uint32_t (3 * stride));
    f.vertexPayload = f.bytes.size ();
    for (int i = 0; i < 3; ++i) {
        std::string vertex (stride, '\0');
        patch32 (vertex, 0, std::bit_cast<uint32_t> (float (i + 1)));
        patch32 (vertex, 4, std::bit_cast<uint32_t> (float (i + 2)));
        patch32 (vertex, 8, std::bit_cast<uint32_t> (float (i + 3)));
        vertex.replace (20, 4, "MDLS");
        if (hasBlendIndices) {
            patch32 (vertex, blendIndexOffset, 0);
            patch32 (vertex, blendIndexOffset + 4, 1);
        }
        if (hasBlendWeights) {
            patch32 (vertex, blendWeightOffset, std::bit_cast<uint32_t> (0.25f));
            patch32 (vertex, blendWeightOffset + 4, std::bit_cast<uint32_t> (0.75f));
        }
        patch32 (vertex, uvOffset, std::bit_cast<uint32_t> (float (i) / 4));
        patch32 (vertex, uvOffset + 4, std::bit_cast<uint32_t> (float (i) / 2));
        if (uv4) {
            patch32 (vertex, uvOffset + 8, std::bit_cast<uint32_t> (float (i) / 8));
            patch32 (vertex, uvOffset + 12, std::bit_cast<uint32_t> (float (i) / 16));
        }
        f.bytes += vertex;
    }
    f.indexLength = f.bytes.size ();
    u32 (f.bytes, 6);
    f.indexPayload = f.bytes.size ();
    f.bytes += std::string {char (0), char (0), char (1), char (0), char (2), char (0)};
    cstr (f.bytes, "MDLS0004"); // Opaque tail sentinel, not a complete native tail.
    return f;
}
auto parse (const std::string& bytes) {
    return parsePuppetFirstMesh ({reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()});
}
auto parseMeshes (const std::string& bytes) {
    return parsePuppetMeshes ({reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()});
}
auto skeleton (const std::string& bytes, const WallpaperEngine::Render::Objects::PuppetMeshData& mesh) {
    return WallpaperEngine::Render::Objects::parsePuppetFirstSkeleton (
        {reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()}, mesh);
}
struct SkeletonFixture {
    std::string bytes;
    size_t sectionBound = 0;
    size_t firstMatrixLength = 0;
    size_t firstMatrix = 0;
    size_t firstParent = 0;
};
SkeletonFixture withSkeleton (int version, bool optionalRestMatrices = false,
                              bool oneAuxiliary = false) {
    const auto mesh = makeFixture (version, 0x01800009, version <= 16 ? 0x01800009 : 0x0180000f);
    SkeletonFixture f {mesh.bytes.substr (0, mesh.indexPayload + 6)};
    if (version >= 21) {
        f.bytes.push_back ('\0'); // first optional blob absent
        f.bytes.push_back ('\1'); // second optional blob present
        u32 (f.bytes, 32); // two 16-byte bone triangle ranges
        for (uint32_t bone = 0; bone < 2; ++bone) {
            u32 (f.bytes, bone);
            u32 (f.bytes, 0); // raw flags
            u32 (f.bytes, bone == 0 ? 0 : 3);
            u32 (f.bytes, bone == 0 ? 3 : 0);
        }
    }
    if (version >= 23) u32 (f.bytes, 0); // no extra v23 records
    cstr (f.bytes, "MDLS000" + std::to_string (version == 13 ? 1 : version <= 19 ? 2 : version == 21 ? 3 : 4));
    f.sectionBound = f.bytes.size ();
    u32 (f.bytes, 0); // absolute section end, patched below
    u32 (f.bytes, 2); // bones
    for (int i = 0; i < 2; ++i) {
        cstr (f.bytes, "bone" + std::to_string (i));
        u32 (f.bytes, uint32_t (10 + i));
        if (i == 0) f.firstParent = f.bytes.size ();
        u32 (f.bytes, i == 0 ? UINT32_MAX : 0);
        if (i == 0) f.firstMatrixLength = f.bytes.size ();
        u32 (f.bytes, 64);
        if (i == 0) f.firstMatrix = f.bytes.size ();
        for (int j = 0; j < 16; ++j) f32 (f.bytes, j % 5 == 0 ? 1.0f : 0.0f);
        cstr (f.bytes, i == 0 ? "{\"first\":true}" : "{}"); // native per-bone metadata string
    }
    if (version >= 14) {
        f.bytes.push_back (oneAuxiliary ? '\1' : '\0');
        f.bytes.push_back ('\0');
        if (oneAuxiliary) {
            cstr (f.bytes, "aux");
            u32 (f.bytes, 0);
            u32 (f.bytes, 0);
            for (int j = 0; j < 16; ++j) f32 (f.bytes, j % 5 == 0 ? 1.0f : 0.0f);
        }
        f.bytes.push_back (optionalRestMatrices ? '\1' : '\0');
        if (optionalRestMatrices) {
            for (int bone = 0; bone < 2; ++bone) {
                for (int j = 0; j < 16; ++j)
                    f32 (f.bytes, j == 12 ? float (bone + 1) : j % 5 == 0 ? 1.0f : 0.0f);
            }
        }
        u32 (f.bytes, 0); // No scalar tracks.
	// Native MDLS continues with group and bone-mapping table counts.
	f.bytes += std::string (4, '\0');
    }
    f.bytes += "opaque"; // Remaining MDLS tail is deliberately not interpreted here.
    patch32 (f.bytes, f.sectionBound, uint32_t (f.bytes.size ()));
    return f;
}
struct AnimationFixture {
    std::string bytes;
    size_t sectionBound = 0;
    size_t firstTrackLength = 0;
    size_t firstSample = 0;
};
AnimationFixture withAnimation (int version, bool attachment = false) {
    const auto skeleton = withSkeleton (version);
    AnimationFixture f {skeleton.bytes};
    if (attachment) {
        cstr (f.bytes, "MDAT0001");
        const size_t attachmentBound = f.bytes.size ();
        u32 (f.bytes, 0);
        f.bytes.push_back ('\1');
        f.bytes.push_back ('\0'); // u16 attachment count = 1
        f.bytes.push_back ('\7');
        f.bytes.push_back ('\0'); // u16 attachment index = 7
        cstr (f.bytes, "socket");
        for (int j = 0; j < 16; ++j) f32 (f.bytes, j % 5 == 0 ? 1.0f : 0.0f);
        patch32 (f.bytes, attachmentBound, uint32_t (f.bytes.size ()));
    }
    const int animationVersion = version == 13 ? 1 : version == 14 ? 2 : version == 16 ? 3 :
                                 version == 17 ? 4 : version == 19 ? 5 : 6;
    cstr (f.bytes, "MDLA000" + std::to_string (animationVersion));
    f.sectionBound = f.bytes.size ();
    u32 (f.bytes, 0); // Absolute end of MDLA section.
    u32 (f.bytes, 1); // One clip.
    u64 (f.bytes, 0x123456789abcdef0ULL);
    cstr (f.bytes, "move");
    cstr (f.bytes, "loop");
    f32 (f.bytes, 30.0f);
    u32 (f.bytes, 2); // Native track payloads contain frameCount + 1 samples.
    u32 (f.bytes, 0x2345); // Raw clip field with semantics still under study.
    u32 (f.bytes, 2); // Two bone tracks.
    for (int track = 0; track < 2; ++track) {
        u32 (f.bytes, uint32_t (track));
        if (track == 0) f.firstTrackLength = f.bytes.size ();
        u32 (f.bytes, 3 * 9 * sizeof (float));
        if (track == 0) f.firstSample = f.bytes.size ();
        for (int sample = 0; sample < 3; ++sample) {
            for (int lane = 0; lane < 9; ++lane)
                f32 (f.bytes, float (track * 100 + sample * 10 + lane));
        }
    }
    if (animationVersion >= 3) {
        u32 (f.bytes, 0); // Extra scalar tracks.
        f.bytes.push_back ('\0'); // No bone scalar A tracks.
    }
    if (animationVersion >= 4) f.bytes.push_back ('\0'); // No mesh channels.
    if (animationVersion >= 5) for (int i = 0; i < 6; ++i) u32 (f.bytes, 0);
    if (animationVersion >= 6) f.bytes.push_back ('\0'); // No bone scalar B tracks.
    u32 (f.bytes, 0); // No events.
    patch32 (f.bytes, f.sectionBound, uint32_t (f.bytes.size ()));
    return f;
}
} // namespace

TEST_CASE ("Versioned MDLV first mesh follows fields instead of embedded markers", "[puppet][mesh]") {
    for (int version : {13, 14, 16, 17, 19, 21, 23}) {
        const std::vector<uint32_t> headers = version < 15 ? std::vector<uint32_t> {0x01800009u} :
                                                       std::vector<uint32_t> {0u, 0x01800009u};
        for (uint32_t headerFlags : headers) {
            const auto fixture = makeFixture (version, headerFlags,
                                              version <= 16 ? 0x01800009 : 0x0180000f);
            const auto mesh = parse (fixture.bytes);
            REQUIRE (mesh.version == version);
            REQUIRE (mesh.vertexMask == (version <= 16 ? 0x01800009u : 0x0180000fu));
            REQUIRE (mesh.material == "material/MDLS_embedded");
            REQUIRE (mesh.positions.size () == 3);
            REQUIRE (mesh.positions[2][0] == 3.0f);
            REQUIRE (mesh.texcoords[2][0] == 0.5f);
            REQUIRE (mesh.blendIndices[2][1] == 1);
            REQUIRE (mesh.blendWeights[2][0] == 0.25f);
            REQUIRE (mesh.blendWeights[2][1] == 0.75f);
            REQUIRE (mesh.indices == std::vector<uint16_t> {0, 1, 2});
            REQUIRE (mesh.payloadEndOffset == fixture.indexPayload + 6);
            REQUIRE (fixture.bytes.compare (mesh.payloadEndOffset, 8, "MDLS0004") == 0);
        }
    }
    const auto unskinned = parse (makeFixture (13, 0x0000000f, 0x0000000f).bytes);
    REQUIRE (unskinned.vertexMask == 0x0000000fu);
    REQUIRE (unskinned.positions.size () == 3);
    REQUIRE (unskinned.texcoords[2][1] == 1.0f);
    REQUIRE (unskinned.blendWeights[0][0] == 0.0f);
    const auto channelMap = parse (makeFixture (17, 0, 0x00800021).bytes);
    REQUIRE (channelMap.blendIndices[2][1] == 1);
    REQUIRE (channelMap.blendWeights[2][0] == 0.0f);
    REQUIRE (channelMap.texcoordsFull[2][2] == 0.25f);
    REQUIRE (channelMap.texcoordsFull[2][3] == 0.125f);
    REQUIRE (parse (makeFixture (17, 0, 0x0180000f, 1, 0, 0).bytes).material.empty ());
}

TEST_CASE ("MDLV mesh list retains each material and independent channel layout", "[puppet][mesh]") {
    const auto first = makeFixture (17, 0, 0x0180000f, 2);
    auto second = makeFixture (17, 0, 0x00800021);
    const auto name = second.bytes.find ("material/MDLS_embedded");
    REQUIRE (name != std::string::npos);
    second.bytes.replace (name, std::string ("material/MDLS_embedded").size (),
                          "material/channelmap_blend");
    std::string bytes = first.bytes.substr (0, first.indexPayload + 6);
    constexpr size_t headerBytes = 9 + 3 * sizeof (uint32_t);
    bytes += second.bytes.substr (headerBytes, second.bytes.size () - headerBytes - 9);
    const size_t meshesEnd = bytes.size ();
    cstr (bytes, "MDLS0002"); // Opaque next-section sentinel.

    const auto model = parseMeshes (bytes);
    REQUIRE (model.version == 17);
    REQUIRE (model.meshes.size () == 2);
    REQUIRE (model.sectionEndOffset == meshesEnd);
    REQUIRE (model.meshes[0].material == "material/MDLS_embedded");
    REQUIRE (model.meshes[1].material == "material/channelmap_blend");
    REQUIRE (model.meshes[0].materials == std::vector<std::string> {"material/MDLS_embedded"});
    REQUIRE (model.meshes[1].materials == std::vector<std::string> {"material/channelmap_blend"});
    REQUIRE (model.meshes[0].vertexMask == 0x0180000fu);
    REQUIRE (model.meshes[1].vertexMask == 0x00800021u);
    REQUIRE (model.meshes[0].meshFlags == 2);
    REQUIRE (model.meshes[1].meshFlags == 2);
    REQUIRE (model.meshes[1].blendRowCount == 1);
    REQUIRE (model.meshes[1].blendWeights[0][0] == 0.0f);
    REQUIRE (model.meshes[1].texcoordsFull[2][3] == 0.125f);
    REQUIRE (model.meshes[1].indices == std::vector<uint16_t> {0, 1, 2});
    REQUIRE (model.meshes[0].payloadEndOffset < model.meshes[1].payloadEndOffset);
    REQUIRE_THROWS (parse (bytes)); // CImage still accepts only one drawable mesh.
    REQUIRE_THROWS (parseMeshes (bytes.substr (0, model.meshes[1].payloadEndOffset - 1)));

    const auto oneMeshSkeleton = withSkeleton (17);
    bytes.resize (meshesEnd);
    bytes += oneMeshSkeleton.bytes.substr (first.indexPayload + 6);
    patch32 (bytes, meshesEnd + 9, uint32_t (bytes.size ()));
    const auto withBones = parseMeshes (bytes);
    const auto parsedBones = WallpaperEngine::Render::Objects::parsePuppetSkeleton (
        {reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()}, withBones);
    REQUIRE (parsedBones.bones.size () == 2);
    REQUIRE (parsedBones.boneRecordsEndOffset > withBones.sectionEndOffset);
}

TEST_CASE ("MDLV v21 retains bounded position and per-bone triangle streams", "[puppet][mesh]") {
    const auto fixture = withSkeleton (21);
    const auto first = parse (fixture.bytes);
    const size_t tail = first.payloadEndOffset;
    std::string positionStream;
    positionStream.push_back ('\1');
    u32 (positionStream, 1); // One float3 stream, with one value per vertex.
    u32 (positionStream, 3 * 3 * sizeof (float));
    for (int vertex = 0; vertex < 3; ++vertex)
        for (int lane = 0; lane < 3; ++lane)
            f32 (positionStream, float (10 * vertex + lane));
    std::string bytes = fixture.bytes;
    bytes.replace (tail, 1, positionStream);
    const size_t sectionBound = fixture.sectionBound + positionStream.size () - 1;
    patch32 (bytes, sectionBound, uint32_t (bytes.size ()));
    const auto span = [&] (const std::string& value) {
        return std::span<const uint8_t> (reinterpret_cast<const uint8_t*> (value.data ()), value.size ());
    };

    const auto model = parsePuppetMeshes (span (bytes));
    REQUIRE (model.sectionEndOffset == sectionBound - 9);
    REQUIRE (model.meshes[0].optionalPositionStreamCount == 1);
    REQUIRE (model.meshes[0].optionalPositions.size () == 3);
    REQUIRE (model.meshes[0].optionalPositions[2] == std::array<float, 3> {20.0f, 21.0f, 22.0f});
    REQUIRE (model.meshes[0].boneRanges.size () == 2);
    REQUIRE (model.meshes[0].boneRanges[0].boneIndex == 0);
    REQUIRE (model.meshes[0].boneRanges[0].indexCount == 3);
    REQUIRE (model.meshes[0].boneRanges[1].firstIndex == 3);
    REQUIRE (WallpaperEngine::Render::Objects::parsePuppetSkeleton (span (bytes), model).bones.size () == 2);

    auto bad = bytes;
    patch32 (bad, tail + 1, 2); // Unsupported second position stream.
    REQUIRE_THROWS (parsePuppetMeshes (span (bad)));
    bad = bytes;
    patch32 (bad, tail + 5, 35); // Incorrect float3 byte count.
    REQUIRE_THROWS (parsePuppetMeshes (span (bad)));
    bad = bytes;
    patch32 (bad, tail + 9, std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parsePuppetMeshes (span (bad)));
    bad = bytes.substr (0, tail + 9 + 35);
    REQUIRE_THROWS (parsePuppetMeshes (span (bad))); // Truncated position payload.
    const size_t ranges = tail + positionStream.size () + 1 + 4;
    bad = bytes;
    patch32 (bad, ranges + 8, UINT32_MAX); // Range begins beyond triangle indices.
    REQUIRE_THROWS (parsePuppetMeshes (span (bad)));
    bad = bytes;
    patch32 (bad, ranges, 2); // No third skeleton bone.
    const auto badModel = parsePuppetMeshes (span (bad));
    REQUIRE_THROWS (WallpaperEngine::Render::Objects::parsePuppetSkeleton (span (bad), badModel));
}

TEST_CASE ("Static MDLV v14 retains normal and tangent attribute lanes", "[puppet][mesh]") {
    auto fixture = makeFixture (14, 0x0000000f, 0x0000000f, 1, 0);
    for (size_t lane = 0; lane < 3; ++lane)
        patch32 (fixture.bytes, fixture.vertexPayload + 12 + lane * 4,
                 std::bit_cast<uint32_t> (float (lane + 1) * 0.125f));
    for (size_t lane = 0; lane < 4; ++lane)
        patch32 (fixture.bytes, fixture.vertexPayload + 24 + lane * 4,
                 std::bit_cast<uint32_t> (float (lane + 1) * -0.25f));
    const auto mesh = parseMeshes (fixture.bytes).meshes.at (0);
    REQUIRE (mesh.meshFlags == 0);
    REQUIRE (mesh.normals.size () == 3);
    REQUIRE (mesh.tangents.size () == 3);
    const std::array<float, 3> expectedNormal {0.125f, 0.25f, 0.375f};
    const std::array<float, 4> expectedTangent {-0.25f, -0.5f, -0.75f, -1.0f};
    REQUIRE (mesh.normals[0] == expectedNormal);
    REQUIRE (mesh.tangents[0] == expectedTangent);
    patch32 (fixture.bytes, fixture.vertexPayload + 12,
             std::bit_cast<uint32_t> (std::numeric_limits<float>::quiet_NaN ()));
    REQUIRE_THROWS (parseMeshes (fixture.bytes));
}

TEST_CASE ("Legacy static MDLV v4 uses its header attribute mask", "[puppet][mesh]") {
    auto fixture = makeFixture (4, 0x0000000b, 0x0000000b, 1, 0);
    const auto model = parseMeshes (fixture.bytes);
    REQUIRE (model.version == 4);
    REQUIRE (model.meshes.size () == 1);
    REQUIRE (model.meshes[0].vertexMask == 0x0000000bu);
    REQUIRE (model.meshes[0].normals.size () == 3);
    REQUIRE (model.meshes[0].texcoords.size () == 3);
    REQUIRE (model.meshes[0].indices == std::vector<uint16_t> {0, 1, 2});
}

TEST_CASE ("Static MDLV padding begins at the parsed mesh boundary", "[model][static][padding]") {
    using WallpaperEngine::Render::Objects::staticModelHasOnlyPadding;
    const auto fixture = makeFixture (13, 0xf, 0xf, 1, 0);
    const auto bytes = fixture.bytes.substr (0, fixture.indexPayload + 6);
    const auto unpadded = parseMeshes (bytes);
    REQUIRE (unpadded.sectionEndOffset == bytes.size ());
    // MDLS appears inside valid material/vertex data. Only the parsed end
    // determines which bytes are optional sections or terminal padding.
    REQUIRE (bytes.find ("MDLS") != std::string::npos);
    for (const size_t length : {size_t (0), size_t (1), size_t (2), size_t (16), size_t (727009)}) {
        const auto padded = bytes + std::string (length, '\0');
        const auto parsed = parseMeshes (padded);
        REQUIRE (parsed.sectionEndOffset == unpadded.sectionEndOffset);
        REQUIRE (staticModelHasOnlyPadding (
            {reinterpret_cast<const uint8_t*> (padded.data ()), padded.size ()}, parsed.sectionEndOffset));
        REQUIRE (parsed.meshes[0].positions == unpadded.meshes[0].positions);
        REQUIRE (parsed.meshes[0].indices == unpadded.meshes[0].indices);
        REQUIRE (parsed.meshes[0].materials == unpadded.meshes[0].materials);
    }
}

TEST_CASE ("Static MDLV padding does not accept nonzero sections or malformed mesh framing",
           "[model][static][padding]") {
    using WallpaperEngine::Render::Objects::staticModelHasOnlyPadding;
    const auto fixture = makeFixture (13, 0xf, 0xf, 1, 0);
    const auto bytes = fixture.bytes.substr (0, fixture.indexPayload + 6);
    const auto end = parseMeshes (bytes).sectionEndOffset;
    for (const auto tag : {"MDLS0004", "MDLA0002", "MDAT0001", "unknown"}) {
        auto sections = bytes;
        cstr (sections, tag);
        REQUIRE_FALSE (staticModelHasOnlyPadding (
            {reinterpret_cast<const uint8_t*> (sections.data ()), sections.size ()}, end));
        // Keep conservative rejection even after an empty terminator; this
        // does not implement the native unknown/optional section machinery.
        sections.insert (end, 16, '\0');
        REQUIRE_FALSE (staticModelHasOnlyPadding (
            {reinterpret_cast<const uint8_t*> (sections.data ()), sections.size ()}, end));
    }
    auto lateNonzero = bytes + std::string (727009, '\0');
    lateNonzero.back () = '\1';
    REQUIRE_FALSE (staticModelHasOnlyPadding (
        {reinterpret_cast<const uint8_t*> (lateNonzero.data ()), lateNonzero.size ()}, end));
    REQUIRE_FALSE (staticModelHasOnlyPadding (
        {reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()}, end + 1));
    REQUIRE_THROWS (parseMeshes (bytes.substr (0, bytes.size () - 1)));
    auto malformed = bytes + std::string (16, '\0');
    patch32 (malformed, fixture.indexLength, 7); // Nonintegral uint16 index stream.
    REQUIRE_THROWS (parseMeshes (malformed));
}

TEST_CASE ("MDLV first mesh rejects unsupported and truncated layouts", "[puppet][mesh]") {
    auto fixture = makeFixture ();
    REQUIRE_THROWS (parse (makeFixture (23, 0, 0x01800008).bytes));
    REQUIRE_THROWS (parse (makeFixture (13, 0, 0x01800009).bytes));
    REQUIRE_THROWS (parse (makeFixture (24).bytes));
    REQUIRE_THROWS (parse (makeFixture (23, 0, 0x0180000f, 2).bytes));
    for (size_t length : {size_t (0), size_t (4), fixture.vertexLength + 3,
                          fixture.vertexPayload + 239, fixture.indexLength + 3,
                          fixture.indexPayload + 5}) {
        REQUIRE_THROWS (parse (fixture.bytes.substr (0, length)));
    }
    patch32 (fixture.bytes, fixture.vertexLength, std::numeric_limits<uint32_t>::max ());
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.indexLength, std::numeric_limits<uint32_t>::max ());
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    fixture.bytes[fixture.indexPayload + 4] = char (3);
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.vertexPayload, std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.vertexPayload + 72,
             std::bit_cast<uint32_t> (std::numeric_limits<float>::quiet_NaN ()));
    REQUIRE_THROWS (parse (fixture.bytes));
    fixture = makeFixture ();
    patch32 (fixture.bytes, fixture.vertexPayload + 56,
             std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parse (fixture.bytes));
    REQUIRE_THROWS (parse ("MDLV0023")); // Missing NUL and all fields.
}

TEST_CASE ("Versioned MDLS bone records have bounded raw matrices", "[puppet][skeleton]") {
    for (int version : {13, 14, 16, 17, 19, 21, 23}) {
        const auto fixture = withSkeleton (version);
        const auto mesh = parse (fixture.bytes);
        const auto data = skeleton (fixture.bytes, mesh);
        REQUIRE (data.version == (version == 13 ? 1 : version <= 19 ? 2 : version == 21 ? 3 : 4));
        REQUIRE (data.bones.size () == 2);
        REQUIRE (data.bones[0].name == "bone0");
        REQUIRE (data.bones[0].rawFlags == 10);
        REQUIRE (data.bones[0].parentIndex == -1);
        REQUIRE (data.bones[0].metadata == "{\"first\":true}");
        REQUIRE (data.bones[1].parentIndex == 0);
        REQUIRE (data.bones[1].metadata == "{}");
        REQUIRE (data.bones[1].matrix[0] == 1.0f);
        REQUIRE (data.auxiliaryTransformCount == 0);
        REQUIRE (data.scalarTrackCount == 0);
        REQUIRE (data.boneRecordsEndOffset + (version == 13 ? 6 : 17) == data.sectionEndOffset);
	if (version >= 14) {
	    REQUIRE (data.boneMappingKnown);
	    REQUIRE (data.mappingRecordByBone == std::vector<int32_t> {-1, -1});
	}
    }
    const auto optional = withSkeleton (17, true);
    const auto optionalData = skeleton (optional.bytes, parse (optional.bytes));
    REQUIRE (optionalData.optionalRestMatrices.size () == 2);
    REQUIRE (optionalData.optionalRestMatrices[0][12] == 1.0f);
    REQUIRE (optionalData.optionalRestMatrices[1][12] == 2.0f);
}

TEST_CASE ("MDLS bounds and unsupported tail branches fail explicitly", "[puppet][skeleton]") {
    auto fixture = withSkeleton (23);
    const auto mesh = parse (fixture.bytes);
    patch32 (fixture.bytes, fixture.sectionBound, UINT32_MAX);
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    patch32 (fixture.bytes, fixture.firstMatrixLength, UINT32_MAX);
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    patch32 (fixture.bytes, fixture.firstParent, 9);
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    patch32 (fixture.bytes, fixture.firstMatrix,
             std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    fixture.bytes[mesh.payloadEndOffset] = '\1'; // first optional branch not decoded yet
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    patch32 (fixture.bytes, fixture.sectionBound + 4, 1); // one bone; weighted index 1 is invalid
    REQUIRE_THROWS (skeleton (fixture.bytes, mesh));
    fixture = withSkeleton (23);
    REQUIRE_THROWS (skeleton (fixture.bytes.substr (0, fixture.bytes.size () - 7), mesh));
}

TEST_CASE ("MDLS mapping table distinguishes unmapped and mapped flag-2 bones", "[puppet][skeleton]") {
    auto fixture = withSkeleton (17, false, true);
    const size_t mappingCountOffset = fixture.bytes.size () - std::string ("opaque").size () - 2;
    fixture.bytes[mappingCountOffset] = '\1';
    std::string mapping;
    u32 (mapping, 1); // Bone 1 receives native bone+d4 mapping record zero.
    u32 (mapping, 1); // One valid auxiliary index.
    u32 (mapping, 0);
    mapping += std::string (2, '\0'); // No nested constraints.
    fixture.bytes.insert (mappingCountOffset + 2, mapping);
    patch32 (fixture.bytes, fixture.sectionBound, uint32_t (fixture.bytes.size ()));
    const auto parsed = skeleton (fixture.bytes, parse (fixture.bytes));
    REQUIRE (parsed.boneMappingKnown);
    REQUIRE (parsed.auxiliaryTransformCount == 1);
    REQUIRE (parsed.mappingRecordByBone == std::vector<int32_t> {-1, 0});

    fixture.bytes[mappingCountOffset + 2] = char (3); // Invalid bone index.
    REQUIRE_THROWS (skeleton (fixture.bytes, parse (fixture.bytes)));
    fixture.bytes[mappingCountOffset + 2] = char (1);
    fixture.bytes[mappingCountOffset + 10] = char (1); // Auxiliary index 1 is out of range.
    REQUIRE_THROWS (skeleton (fixture.bytes, parse (fixture.bytes)));
    fixture.bytes[mappingCountOffset + 10] = char (0);
    for (size_t i = mappingCountOffset + 6; i < mappingCountOffset + 10; ++i)
        fixture.bytes[i] = char (0xff); // Truncated auxiliary index array.
    REQUIRE_THROWS (skeleton (fixture.bytes, parse (fixture.bytes)));
}

TEST_CASE ("Versioned MDLA first clip keeps bounded raw bone samples", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    for (int version : {13, 14, 16, 17, 19, 21, 23}) {
        const auto fixture = withAnimation (version);
        const auto mesh = parse (fixture.bytes);
        const auto data = skeleton (fixture.bytes, mesh);
        const auto animation = parsePuppetFirstAnimationHeader (
            {reinterpret_cast<const uint8_t*> (fixture.bytes.data ()), fixture.bytes.size ()}, data);
        REQUIRE (animation.version == (version == 13 ? 1 : version == 14 ? 2 : version == 16 ? 3 :
                                       version == 17 ? 4 : version == 19 ? 5 : 6));
        REQUIRE (animation.clipCount == 1);
        REQUIRE (animation.clips.size () == 1);
        const auto& clip = animation.clips[0];
        REQUIRE (clip.rawId == 0x123456789abcdef0ULL);
        REQUIRE (clip.name == "move");
        REQUIRE (clip.mode == "loop");
        REQUIRE (clip.rawRate == 30.0f);
        REQUIRE (clip.rawFrameCount == 2);
        REQUIRE (clip.rawField == 0x2345);
        REQUIRE (clip.trackCount == 2);
        REQUIRE (clip.tracks.size () == 2);
        REQUIRE (clip.tracks[0].rawFlags == 0);
        REQUIRE (clip.tracks[1].rawFlags == 1);
        REQUIRE (clip.tracks[0].rawSamples.size () == 3);
        REQUIRE (clip.tracks[0].rawSamples[2][8] == 28.0f);
        REQUIRE (clip.tracks[1].rawSamples[1][4] == 114.0f);
        REQUIRE (clip.boneTracksEndOffset < clip.endOffset);
        REQUIRE (clip.endOffset == animation.sectionEndOffset);
    }
    const auto withAttachment = withAnimation (23, true);
    const auto mesh = parse (withAttachment.bytes);
    const auto data = skeleton (withAttachment.bytes, mesh);
    const auto animation = parsePuppetFirstAnimationHeader (
        {reinterpret_cast<const uint8_t*> (withAttachment.bytes.data ()), withAttachment.bytes.size ()}, data);
    REQUIRE (animation.attachments.size () == 1);
    REQUIRE (animation.attachments[0].rawIndex == 7);
    REQUIRE (animation.attachments[0].name == "socket");
    REQUIRE (animation.attachments[0].matrix[0] == 1.0f);
    REQUIRE (animation.clips[0].tracks.size () == 2);
}

TEST_CASE ("MDLA bounds and sample shape reject malformed first clips", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    auto fixture = withAnimation (23);
    const auto mesh = parse (fixture.bytes);
    const auto data = skeleton (fixture.bytes, mesh);
    auto parseAnimation = [&] (const std::string& bytes) {
        return parsePuppetFirstAnimationHeader (
            {reinterpret_cast<const uint8_t*> (bytes.data ()), bytes.size ()}, data);
    };
    REQUIRE_THROWS (parseAnimation (fixture.bytes.substr (0, fixture.bytes.size () - 1)));
    patch32 (fixture.bytes, fixture.sectionBound, UINT32_MAX);
    REQUIRE_THROWS (parseAnimation (fixture.bytes));
    fixture = withAnimation (23);
    patch32 (fixture.bytes, fixture.firstTrackLength, 37);
    REQUIRE_THROWS (parseAnimation (fixture.bytes));
    fixture = withAnimation (23);
    patch32 (fixture.bytes, fixture.firstTrackLength, 4 * 9 * sizeof (float));
    REQUIRE_THROWS (parseAnimation (fixture.bytes));
    fixture = withAnimation (23);
    patch32 (fixture.bytes, fixture.firstSample,
             std::bit_cast<uint32_t> (std::numeric_limits<float>::infinity ()));
    REQUIRE_THROWS (parseAnimation (fixture.bytes));
}

TEST_CASE ("MDLA v6 retains scalar, mesh-channel, event and second-clip records", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    auto fixture = withAnimation (23);
    const auto mesh = parse (fixture.bytes);
    const auto data = skeleton (fixture.bytes, mesh);
    auto parseAnimation = [&] () {
        return parsePuppetFirstAnimationHeader (
            {reinterpret_cast<const uint8_t*> (fixture.bytes.data ()), fixture.bytes.size ()}, data);
    };
    const size_t boneTracksEnd = parseAnimation ().clips[0].boneTracksEndOffset;
    fixture.bytes.resize (boneTracksEnd);
    auto scalar = [&] (uint32_t flags, bool includeFlags) {
        if (includeFlags) u32 (fixture.bytes, flags);
        u32 (fixture.bytes, 3 * sizeof (float));
        for (int i = 0; i < 3; ++i) f32 (fixture.bytes, float (int (flags) + i));
    };
    u32 (fixture.bytes, 1); // One v3+ extra scalar track.
    scalar (10, true);
    fixture.bytes.push_back ('\1'); // Bone scalar A tracks present.
    scalar (20, true);
    scalar (21, true);
    fixture.bytes.push_back ('\1'); // Mesh-channel tracks present.
    u32 (fixture.bytes, 1); // Mesh flags.
    u32 (fixture.bytes, 44); // Raw mesh field.
    fixture.bytes.push_back ('\1');
    fixture.bytes.push_back ('\0'); // One channel.
    fixture.bytes.push_back ('\7');
    fixture.bytes.push_back ('\0'); // Channel index 7.
    scalar (0, false);
    for (uint32_t i = 1; i <= 6; ++i) u32 (fixture.bytes, i);
    fixture.bytes.push_back ('\1'); // Bone scalar B tracks present.
    scalar (30, true);
    scalar (31, true);
    u32 (fixture.bytes, 1);
    u32 (fixture.bytes, 99);
    cstr (fixture.bytes, "event");
    const size_t secondClipStart = fixture.bytes.size ();
    const auto second = withAnimation (23);
    const size_t secondSectionData = second.sectionBound + 8; // Skip end bound and clip count.
    fixture.bytes.append (second.bytes, secondSectionData, second.bytes.size () - secondSectionData);
    patch32 (fixture.bytes, fixture.sectionBound, uint32_t (fixture.bytes.size ()));
    patch32 (fixture.bytes, fixture.sectionBound + 4, 2);
    const auto animation = parseAnimation ();
    REQUIRE (animation.clipCount == 2);
    REQUIRE (animation.clips.size () == 2);
    REQUIRE (animation.clips[0].scalarTracks.empty ());
    REQUIRE (animation.clips[0].extraScalarTracks.size () == 1);
    REQUIRE (animation.clips[0].extraScalarTracks[0].rawFlags == 10);
    REQUIRE (animation.clips[0].extraScalarTracks[0].rawSamples[2] == 12.0f);
    REQUIRE (animation.clips[0].boneScalarsA.size () == 2);
    REQUIRE (animation.clips[0].boneScalarsA[1].rawFlags == 21);
    REQUIRE (animation.clips[0].meshTracks.size () == 1);
    REQUIRE (animation.clips[0].meshTracks[0].rawField == 44);
    REQUIRE (animation.clips[0].meshTracks[0].channels[0].rawIndex == 7);
    REQUIRE (animation.clips[0].meshTracks[0].channels[0].values.rawSamples[2] == 2.0f);
    REQUIRE (animation.clips[0].versionFiveFields[5] == 6);
    REQUIRE (animation.clips[0].boneScalarsB.size () == 2);
    REQUIRE (animation.clips[0].boneScalarsB[1].rawSamples[2] == 33.0f);
    REQUIRE (animation.clips[0].events.size () == 1);
    REQUIRE (animation.clips[0].events[0].rawField == 99);
    REQUIRE (animation.clips[0].events[0].name == "event");
    REQUIRE (animation.clips[0].endOffset == secondClipStart);
    REQUIRE (animation.clips[1].name == "move");
    REQUIRE (animation.clips[1].endOffset == animation.sectionEndOffset);
}

TEST_CASE ("Puppet skin palette uses current global times inverse bind global", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetSkeletonData skeleton;
    skeleton.bones.resize (2);
    skeleton.bones[0].parentIndex = -1;
    skeleton.bones[1].parentIndex = 0;
    const glm::mat4 rootBind = glm::translate (glm::mat4 (1.0f), glm::vec3 (2, 0, 0));
    const glm::mat4 childBind = glm::translate (glm::mat4 (1.0f), glm::vec3 (0, 3, 0));
    for (size_t i = 0; i < 16; ++i) {
        skeleton.bones[0].matrix[i] = glm::value_ptr (rootBind)[i];
        skeleton.bones[1].matrix[i] = glm::value_ptr (childBind)[i];
    }
    PuppetMeshData mesh;
    mesh.vertexMask = 0x01800009;
    mesh.positions.push_back ({2, 4, 0});
    mesh.blendIndices.push_back ({0, 1, 0, 0});
    mesh.blendWeights.push_back ({0.5f, 0.5f, 0, 0});
    const auto rest = puppetSkinPalette (skeleton, puppetLocalBindMatrices (skeleton));
    const auto restPoint = puppetSkinnedPosition (mesh, 0, rest);
    REQUIRE (restPoint.x == 2.0f);
    REQUIRE (restPoint.y == 4.0f);
    const std::vector<glm::mat4> moved = {
        glm::translate (glm::mat4 (1.0f), glm::vec3 (4, 0, 0)),
        glm::translate (glm::mat4 (1.0f), glm::vec3 (0, 5, 0))};
    const auto palette = puppetSkinPalette (skeleton, moved);
    const auto point = puppetSkinnedPosition (mesh, 0, palette);
    REQUIRE (point.x == 4.0f);
    REQUIRE (point.y == 5.0f);
    const std::vector<glm::mat4> rotated = {
        glm::translate (glm::mat4 (1.0f), glm::vec3 (4, 0, 0)) *
            glm::rotate (glm::mat4 (1.0f), 1.57079632679f, glm::vec3 (0, 0, 1)),
        glm::translate (glm::mat4 (1.0f), glm::vec3 (0, 5, 0))};
    const auto rotatedPoint = puppetSkinnedPosition (mesh, 0, puppetSkinPalette (skeleton, rotated));
    REQUIRE (rotatedPoint.x == Catch::Approx (-1.0f).margin (1e-5f));
    REQUIRE (rotatedPoint.y == Catch::Approx (0.0f).margin (1e-5f));
    const auto currentGlobals = puppetGlobalMatrices (skeleton, rotated);
    const auto inverseBind = puppetInverseBindMatrices (skeleton);
    const auto emission = puppetEmissionBoneMatrix (currentGlobals, inverseBind, 1);
    REQUIRE (emission.has_value ());
    const glm::vec4 sourcePosition (2, 4, 0, 1);
    REQUIRE ((*emission * sourcePosition).x ==
             Catch::Approx ((puppetSkinPalette (skeleton, rotated)[1] * sourcePosition).x).margin (1e-5f));
    REQUIRE ((*emission * sourcePosition).y ==
             Catch::Approx ((puppetSkinPalette (skeleton, rotated)[1] * sourcePosition).y).margin (1e-5f));
    REQUIRE_FALSE (puppetEmissionBoneMatrix (currentGlobals, inverseBind, 0xff).has_value ());
    REQUIRE_FALSE (puppetEmissionBoneMatrix (currentGlobals, inverseBind, 2).has_value ());
    mesh.blendWeights[0] = {0, 0, 0, 0};
    REQUIRE (puppetSkinnedPosition (mesh, 0, palette).x == 0.0f);
    mesh.vertexMask = 0x0000000f;
    REQUIRE (puppetSkinnedPosition (mesh, 0, palette).x == 2.0f);
    mesh.vertexMask = 0x01800009;
    mesh.blendWeights[0] = {0.5f, 0.5f, 0, 0};
    skeleton.bones[1].parentIndex = -2;
    REQUIRE_THROWS (puppetSkinPalette (skeleton, moved));
    skeleton.bones[1].parentIndex = 1;
    REQUIRE_THROWS (puppetSkinPalette (skeleton, moved));
    skeleton.bones[1].parentIndex = 0;
    skeleton.bones[0].matrix[0] = 0.0f;
    REQUIRE_THROWS (puppetSkinPalette (skeleton, moved));
    const glm::mat4 tiny = glm::scale (glm::mat4 (1.0f), glm::vec3 (0.001f));
    for (size_t i = 0; i < 16; ++i) skeleton.bones[0].matrix[i] = glm::value_ptr (tiny)[i];
    REQUIRE_NOTHROW (puppetSkinPalette (skeleton, moved));
}

TEST_CASE ("MDLA nine-float pose deforms weighted vertices through two bones", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetSkeletonData skeleton;
    skeleton.bones.resize (2);
    skeleton.bones[0].parentIndex = -1;
    skeleton.bones[1].parentIndex = 0;
    const glm::mat4 rootBind = glm::translate (glm::mat4 (1.0f), glm::vec3 (2, 0, 0));
    const glm::mat4 childBind = glm::translate (glm::mat4 (1.0f), glm::vec3 (0, 3, 0));
    for (size_t i = 0; i < 16; ++i) {
        skeleton.bones[0].matrix[i] = glm::value_ptr (rootBind)[i];
        skeleton.bones[1].matrix[i] = glm::value_ptr (childBind)[i];
    }
    PuppetClipHeader clip;
    clip.tracks.resize (2);
    clip.tracks[0].rawSamples = {{2, 0, 0, 0, 0, 0, 1, 1, 1},
                                  {4, 0, 0, 0, 0, 1.57079632679f, 1, 1, 1}};
    clip.tracks[1].rawSamples = {{0, 3, 0, 0, 0, 0, 1, 1, 1},
                                  {0, 5, 0, 0, 0, 0, 1, 1, 1}};
    PuppetMeshData mesh;
    mesh.vertexMask = 0x01800009;
    mesh.positions.push_back ({2, 4, 0});
    mesh.blendIndices.push_back ({0, 1, 0, 0});
    mesh.blendWeights.push_back ({0.5f, 0.5f, 0, 0});
    const auto rest = puppetSkinnedPosition (mesh, 0, puppetSkinPalette (skeleton, puppetLocalPoseAtSample (clip, 0)));
    REQUIRE (rest.x == Catch::Approx (2.0f));
    REQUIRE (rest.y == Catch::Approx (4.0f));
    const auto moved = puppetSkinnedPosition (mesh, 0, puppetSkinPalette (skeleton, puppetLocalPoseAtSample (clip, 1)));
    REQUIRE (moved.x == Catch::Approx (-1.0f).margin (1e-5f));
    REQUIRE (moved.y == Catch::Approx (0.0f).margin (1e-5f));
    REQUIRE_THROWS (puppetLocalPoseAtSample (clip, 2));
}

TEST_CASE ("Puppet frame and ordinary-layer quaternions use shortest normalized linear blends", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetClipHeader clip;
    clip.tracks.resize (1);
    clip.tracks[0].rawSamples = {
        {0, 0, 0, 0, 0, 0, 1, 1, 1},
        {8, 0, 0, 0, 0, 2.09439510239f, 2, 1, 1}};
    const auto quarter = puppetPoseAtFrames (clip, 0, 1, 0.25f);
    REQUIRE (quarter.size () == 1);
    REQUIRE (quarter[0].position.x == 2.0f);
    REQUIRE (quarter[0].scale.x == 1.25f);
    const glm::quat q = quarter[0].rotation;
    const float expectedAngle = 2.0f * std::atan ((0.25f * std::sin (1.0471975512f)) /
                                                   (0.75f + 0.25f * std::cos (1.0471975512f)));
    REQUIRE (2.0f * std::atan (q.z / q.w) == Catch::Approx (expectedAngle).margin (1e-6f));
    REQUIRE (expectedAngle < 0.5235987756f); // A quarter slerp would be 30 degrees.
    const auto halfLayer = puppetBlendPoseSamples (puppetRawPoseSample (clip.tracks[0].rawSamples[0]),
                                                   quarter[0], 0.5f);
    REQUIRE (halfLayer.position.x == 1.0f);
    REQUIRE (halfLayer.scale.x == 1.125f);
    const auto antipodal = puppetShortestNlerp (
        puppetRawPoseSample ({0, 0, 0, 0, 0, 0, 1, 1, 1}).rotation,
        puppetRawPoseSample ({0, 0, 0, 0, 0, 6.28318530718f, 1, 1, 1}).rotation, 0.5f);
    REQUIRE (std::abs (antipodal.w) == Catch::Approx (1.0f).margin (1e-6f));
    REQUIRE_THROWS (puppetPoseAtFrames (clip, 0, 2, 0.5f));
    REQUIRE (puppetPoseAtFrames (clip, 0, 1, 1.5f)[0].position.x == 12.0f);
}

TEST_CASE ("Additive puppet layers compose reference-relative rotation after prior pose", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    const auto reference = puppetRawPoseSample ({2, 3, 0, 0, 0, 0, 1, 1, 1});
    const auto previous = puppetRawPoseSample ({5, 7, 0, 0, 0, 1.57079632679f, 2, 2, 1});
    const auto next = puppetRawPoseSample ({6, 9, 0, 1.57079632679f, 0, 0, 3, 4, 1});
    const auto full = puppetAdditivePoseSample (previous, next, reference, 1.0f);
    REQUIRE (full.position.x == 9.0f);
    REQUIRE (full.position.y == 13.0f);
    REQUIRE (full.scale.x == 4.0f);
    REQUIRE (full.scale.y == 5.0f);
    const glm::vec4 turned = puppetPoseMatrix (full) * glm::vec4 (0, 1, 0, 0);
    REQUIRE (turned.x == Catch::Approx (0.0f).margin (1e-5f));
    REQUIRE (turned.y == Catch::Approx (0.0f).margin (1e-5f));
    REQUIRE (turned.z == Catch::Approx (5.0f).margin (1e-5f));
    const auto half = puppetAdditivePoseSample (previous, next, reference, 0.5f);
    REQUIRE (half.position.x == 7.0f);
    REQUIRE (half.scale.y == 3.5f);
    const auto none = puppetAdditivePoseSample (previous, next, reference, 0.0f);
    REQUIRE (none.position.x == previous.position.x);
    REQUIRE (glm::dot (none.rotation, previous.rotation) == Catch::Approx (1.0f).margin (1e-6f));
}

TEST_CASE ("Puppet rest reference and frame selector follow native optional matrices and terminal sample", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    const auto fixture = withSkeleton (17, true);
    const auto rest = skeleton (fixture.bytes, parse (fixture.bytes));
    const auto reference = puppetReferencePoseSamples (rest);
    REQUIRE (reference.size () == 2);
    REQUIRE (reference[0].position.x == 1.0f);
    REQUIRE (reference[1].position.x == 2.0f);
    REQUIRE (reference[0].scale == glm::vec3 (1.0f));
    auto sheared = rest;
    sheared.optionalRestMatrices[0][4] = 0.25f;
    const auto exactRest = puppetUnanimatedLocalMatrices (sheared);
    REQUIRE (exactRest[0][1][0] == 0.25f);
    REQUIRE (puppetPoseMatrices (puppetReferencePoseSamples (sheared))[0][1][0] != 0.25f);
    PuppetClipHeader clip;
    clip.rawRate = 2.0f;
    clip.rawFrameCount = 2;
    const auto start = puppetSelectFrames (clip, 0.0f);
    REQUIRE (start.first == 0);
    REQUIRE (start.next == 1);
    REQUIRE (start.weight == 0.0f);
    const auto quarter = puppetSelectFrames (clip, 0.25f);
    REQUIRE (quarter.first == 0);
    REQUIRE (quarter.next == 1);
    REQUIRE (quarter.weight == 0.5f);
    const auto second = puppetSelectFrames (clip, 0.75f);
    REQUIRE (second.first == 1);
    REQUIRE (second.next == 2);
    REQUIRE (second.weight == 0.5f);
    const auto terminal = puppetSelectFrames (clip, 1.0f);
    REQUIRE (terminal.first == 1);
    REQUIRE (terminal.next == 2);
    REQUIRE (terminal.weight == 0.0f);
    REQUIRE_THROWS (puppetSelectFrames (clip, -0.1f));
    clip.rawRate = 0.0f;
    REQUIRE_THROWS (puppetSelectFrames (clip, 0.0f));
}

TEST_CASE ("Puppet playback modes normalize positive elapsed time before frame selection", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetClipHeader clip;
    clip.rawRate = 2.0f;
    clip.rawFrameCount = 2; // One-second duration, with an extra terminal sample.
    clip.mode = "loop";
    PuppetPlaybackState state;
    puppetAdvancePlayback (clip, state, 1.25f, 1.0f);
    REQUIRE (state.time == 0.25f);
    state = {};
    puppetAdvancePlayback (clip, state, 0.25f, 2.0f);
    REQUIRE (state.time == 0.5f);
    clip.mode = "mirror";
    state = {};
    puppetAdvancePlayback (clip, state, 1.0f, 1.0f);
    REQUIRE (state.time == 1.0f);
    REQUIRE (state.reverse);
    puppetAdvancePlayback (clip, state, 0.25f, 1.0f);
    REQUIRE (state.time == 0.75f);
    puppetAdvancePlayback (clip, state, 1.0f, 1.0f);
    REQUIRE (state.time == 0.25f);
    REQUIRE_FALSE (state.reverse);
    clip.mode = "single";
    state = {};
    puppetAdvancePlayback (clip, state, 1.25f, 1.0f);
    REQUIRE (state.time == 1.0f);
    REQUIRE (state.stopped);
    REQUIRE (puppetSelectFrames (clip, state.time).next == 2);
    puppetAdvancePlayback (clip, state, 0.5f, 1.0f);
    REQUIRE (state.time == 1.0f);
    state = {};
    REQUIRE_THROWS (puppetAdvancePlayback (clip, state, 0.1f, std::numeric_limits<float>::infinity ()));
}

TEST_CASE ("Two puppet layers deform a weighted vertex with ordinary and additive poses", "[puppet][skinning]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetSkeletonData skeleton;
    skeleton.bones.resize (2);
    skeleton.bones[0].parentIndex = -1;
    skeleton.bones[1].parentIndex = 0;
    const glm::mat4 childBind = glm::translate (glm::mat4 (1.0f), glm::vec3 (1, 0, 0));
    for (size_t i = 0; i < 16; ++i) {
        skeleton.bones[0].matrix[i] = glm::value_ptr (glm::mat4 (1.0f))[i];
        skeleton.bones[1].matrix[i] = glm::value_ptr (childBind)[i];
    }
    const auto reference = puppetReferencePoseSamples (skeleton);
    PuppetClipHeader ordinary;
    ordinary.tracks.resize (2);
    ordinary.tracks[0].rawSamples = {{0, 0, 0, 0, 0, 0, 1, 1, 1},
                                      {2, 0, 0, 0, 0, 0, 1, 1, 1}};
    ordinary.tracks[1].rawSamples = {{1, 0, 0, 0, 0, 0, 1, 1, 1},
                                      {1, 0, 0, 0, 0, 0, 1, 1, 1}};
    PuppetClipHeader additive = ordinary;
    additive.tracks[0].rawFlags = 1; // Native bit0 preserves the previous pose.
    additive.tracks[1].rawSamples[1] = {1, 2, 0, 0, 0, 0, 1, 1, 1};
    const auto first = puppetApplyClipLayer (skeleton, ordinary, reference, reference,
                                             {1, 1, 0}, 0.5f, false);
    PuppetMeshData mesh;
    mesh.vertexMask = 0x01800009;
    mesh.positions.push_back ({1, 0, 0});
    mesh.blendIndices.push_back ({0, 1, 0, 0});
    mesh.blendWeights.push_back ({0.25f, 0.75f, 0, 0});
    for (const auto [weight, expectedY] : {std::pair {0.0f, 0.0f}, std::pair {0.5f, 0.75f},
                                           std::pair {1.0f, 1.5f}}) {
        const auto layered = puppetApplyClipLayer (skeleton, additive, first, reference,
                                                    {1, 1, 0}, weight, true);
        const auto point = puppetSkinnedPosition (mesh, 0,
            puppetSkinPalette (skeleton, puppetPoseMatrices (layered)));
        REQUIRE (point.x == Catch::Approx (2.0f));
        REQUIRE (point.y == Catch::Approx (expectedY));
    }
    skeleton.bones[1].rawFlags = 2;
    REQUIRE_THROWS (puppetApplyClipLayer (skeleton, additive, first, reference,
                                           {1, 1, 0}, 1.0f, true));
    skeleton.boneMappingKnown = true;
    skeleton.mappingRecordByBone = {-1, -1};
    const auto disabled = puppetApplyClipLayer (skeleton, additive, first, reference,
                                                {1, 1, 0}, 1.0f, true);
    REQUIRE (disabled[1].position.y == Catch::Approx (first[1].position.y));
    skeleton.mappingRecordByBone[1] = 0;
    REQUIRE_THROWS (puppetApplyClipLayer (skeleton, additive, first, reference,
                                           {1, 1, 0}, 1.0f, true));
}

TEST_CASE ("Puppet layer weight follows independent native entry and exit ramps", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetClipHeader clip;
    clip.rawRate = 2.0f;
    clip.rawFrameCount = 4; // duration two seconds; half-duration one second.
    clip.mode = "loop";
    PuppetPlaybackState playback;
    bool blendIn = true;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.5f, 0.5f, blendIn, false) == 0.0f);
    playback.time = 0.25f;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.5f, 0.5f, blendIn, false) == 0.75f);
    playback.time = 0.5f;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.5f, 0.5f, blendIn, false) == 1.5f);
    REQUIRE_FALSE (blendIn);
    playback.time = 1.75f;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.5f, 0.5f, blendIn, true) == 0.75f);
    playback.time = 2.0f;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.5f, 0.5f, blendIn, true) == 0.0f);
    blendIn = true;
    clip.mode = "single";
    playback.time = 0.5f;
    REQUIRE (puppetEffectiveLayerWeight (clip, playback, 1.0f, 0.5f, blendIn, false) == 1.0f);
    REQUIRE (blendIn); // Native retains single-mode entry flag.
}

TEST_CASE ("Puppet channel scalar preserves prior value through ordinary and additive layers", "[puppet][animation]") {
    using namespace WallpaperEngine::Render::Objects;
    PuppetClipHeader::ScalarTrack ordinary {{0}, {0.0f, 1.0f}};
    PuppetClipHeader::ScalarTrack additive {{0}, {0.0f, 0.8f}};
    const PuppetFrameSelection sample {0, 1, 0.25f};
    const float ordinarySample = puppetScalarAtFrames (ordinary, sample);
    const float additiveSample = puppetScalarAtFrames (additive, sample);
    REQUIRE (ordinarySample == Catch::Approx (0.25f));
    REQUIRE (additiveSample == Catch::Approx (0.2f));
    const float afterOrdinary = puppetApplyScalarLayer (0.4f, ordinarySample, 0.5f, false);
    REQUIRE (afterOrdinary == Catch::Approx (0.325f));
    const float afterAdditive = puppetApplyScalarLayer (afterOrdinary, additiveSample, 0.25f, true);
    REQUIRE (afterAdditive == Catch::Approx (0.375f));
    REQUIRE (puppetApplyScalarLayer (afterAdditive, 0.0f, 0.0f, false) == Catch::Approx (afterAdditive));
    REQUIRE_THROWS (puppetScalarAtFrames (ordinary, {1, 2, 0.0f}));
}
