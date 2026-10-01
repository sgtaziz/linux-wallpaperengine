#include "WallpaperEngine/Render/Objects/ParticleChildControlPoints.h"

#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_inverse.hpp>

using WallpaperEngine::Render::Objects::ParticleCore::childParticleControlPointPosition;

TEST_CASE ("Child particle CP positions use the parent stack for differing preset spaces",
           "[particle-child-cp]") {
    // Independent triangular affine oracle: x'=2x-3y+11,
    // y'=4y+z/2-7, z'=2z+5. Off-origin XYZ detects translation, shear,
    // nonuniform scale, and use of the child stack instead of the parent.
    glm::mat4 parent (1.0f);
    parent[0] = {2.0f, 0.0f, 0.0f, 0.0f};
    parent[1] = {-3.0f, 4.0f, 0.0f, 0.0f};
    parent[2] = {0.0f, 0.5f, 2.0f, 0.0f};
    parent[3] = {11.0f, -7.0f, 5.0f, 1.0f};
    const auto inverse = std::optional<glm::mat4> (glm::inverse (parent));
    const glm::vec3 source {1.0f, 2.0f, 3.0f};
    REQUIRE (childParticleControlPointPosition (source, false, false, parent, inverse)
             == std::optional<glm::vec3> (source));
    REQUIRE (childParticleControlPointPosition (source, true, true, parent, inverse)
             == std::optional<glm::vec3> (source));
    REQUIRE (childParticleControlPointPosition (source, false, true, parent, inverse)
             == std::optional<glm::vec3> ({7.0f, 2.5f, 11.0f}));
    // Inverse independently solved from the three equations: z=-1,
    // y=19/8, x=-23/16.
    REQUIRE (childParticleControlPointPosition (source, true, false, parent, inverse)
             == std::optional<glm::vec3> ({-1.4375f, 2.375f, -1.0f}));
}

TEST_CASE ("Child CP transport needs an inverse only for world to local",
           "[particle-child-cp]") {
    glm::mat4 singular (0.0f);
    singular[3] = {9.0f, -4.0f, 2.0f, 1.0f};
    const glm::vec3 source {1.0f, 2.0f, 3.0f};
    REQUIRE (childParticleControlPointPosition (source, false, false, singular, std::nullopt)
             == std::optional<glm::vec3> (source));
    REQUIRE (childParticleControlPointPosition (source, true, true, singular, std::nullopt)
             == std::optional<glm::vec3> (source));
    REQUIRE (childParticleControlPointPosition (source, false, true, singular, std::nullopt)
             == std::optional<glm::vec3> ({9.0f, -4.0f, 2.0f}));
    REQUIRE_FALSE (childParticleControlPointPosition (source, true, false, singular, std::nullopt));
}
