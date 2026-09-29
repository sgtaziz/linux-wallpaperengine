#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Model {
struct DynamicModelShape {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::array<float, 4>> tangents;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<uint32_t> indices;
    std::vector<int> vertexFormat;
    std::string material;
    bool vertexDynamic = false;
    bool indexDynamic = false;
};

struct DynamicModelData {
    std::vector<DynamicModelShape> shapes;
    uint64_t revision = 1;
    uint64_t structureRevision = 1;
    bool destroyed = false;
};
}
