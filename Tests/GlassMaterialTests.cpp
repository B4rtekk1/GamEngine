#include "../Engine/Source/Assets/GlbLoader.h"
#include "Engine/Assets/Gmesh.h"
#include "Engine/Renderer/Geometry/Cube.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneEditor.h"
#include "Engine/Scene/SceneSerializer.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <sstream>

namespace {
class GlassMaterialTests : public ::testing::Test {
  protected:
    std::filesystem::path directory;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() / "gamengine-glass-material-tests";
        std::filesystem::create_directories(directory);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

TEST_F(GlassMaterialTests, SceneRoundTripPreservesGlassAndTextureTransforms) {
    Engine::PBRMaterial material;
    material.transmission = 0.95F;
    material.ior = 1.52F;
    material.thickness = 0.006F;
    material.attenuationColor = {0.95F, 0.99F, 0.97F};
    material.attenuationDistance = 1.0F;
    material.dirtIntensity = 0.3F;
    material.glassTextureTransforms[0] = {1, 0.2F, 0.3F, 2.0F, 3.0F, 0.4F};
    Engine::Scene source;
    ASSERT_TRUE(source.createCube("Glass", material).valid());
    std::ostringstream serialized;
    Engine::SceneSerializer::save(source, serialized);
    Engine::Scene loaded;
    std::istringstream input{serialized.str()};
    Engine::SceneSerializer::load(loaded, input);
    const auto actor = loaded.findActor("Glass");
    ASSERT_TRUE(actor.valid());
    const auto& restored =
        loaded.editor().read<Engine::MeshRendererComponent>(loaded.findEntity(actor.id())).material.pbr;
    EXPECT_FLOAT_EQ(restored.transmission, material.transmission);
    EXPECT_FLOAT_EQ(restored.ior, material.ior);
    EXPECT_FLOAT_EQ(restored.thickness, material.thickness);
    EXPECT_FLOAT_EQ(restored.attenuationDistance, material.attenuationDistance);
    EXPECT_FLOAT_EQ(restored.attenuationColor.g(), material.attenuationColor.g());
    EXPECT_FLOAT_EQ(restored.dirtIntensity, material.dirtIntensity);
    EXPECT_EQ(restored.glassTextureTransforms[0].texCoord, 1);
    EXPECT_FLOAT_EQ(restored.glassTextureTransforms[0].rotation, 0.4F);
    EXPECT_FLOAT_EQ(restored.glassTextureTransforms[0].scaleY, 3.0F);
}

TEST_F(GlassMaterialTests, ImportsGltfTransmissionIorVolumeAndIndependentUvs) {
    const std::array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::ofstream buffer(directory / "triangle.bin", std::ios::binary);
    buffer.write(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    buffer.close();
    std::ofstream gltf(directory / "glass.gltf");
    gltf << R"({"asset":{"version":"2.0"},
            "buffers":[{"uri":"triangle.bin","byteLength":36}],
            "bufferViews":[{"buffer":0,"byteLength":36}],
            "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
            "images":[{"uri":"mask.png"}],"textures":[{"source":0}],
            "materials":[{"doubleSided":true,"alphaMode":"BLEND","extensions":{
                "KHR_materials_transmission":{"transmissionFactor":0.9,"transmissionTexture":{"index":0,"extensions":{"KHR_texture_transform":{"texCoord":1,"scale":[2,3],"rotation":0.4}}}},
                "KHR_materials_ior":{"ior":1.52},
                "KHR_materials_volume":{"thicknessFactor":0.006,"thicknessTexture":{"index":0},"attenuationDistance":1,"attenuationColor":[0.95,0.99,0.97]}}}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],
            "nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    gltf.close();
    const auto mesh = Engine::Assets::load_gltf_mesh_for_cooking(directory / "glass.gltf");
    ASSERT_TRUE(mesh);
    ASSERT_FALSE(mesh->materials.empty());
    const auto& material = mesh->materials[0];
    EXPECT_FLOAT_EQ(material.transmission, 0.9F);
    EXPECT_FLOAT_EQ(material.ior, 1.52F);
    EXPECT_FLOAT_EQ(material.thickness, 0.006F);
    EXPECT_FLOAT_EQ(material.attenuationDistance, 1.0F);
    EXPECT_FLOAT_EQ(material.attenuationColor.g(), 0.99F);
    EXPECT_EQ(material.transmissionTexture, 0);
    EXPECT_EQ(material.thicknessTexture, 0);
    EXPECT_EQ(material.glassTextureTransforms[0].texCoord, 1);
    EXPECT_FLOAT_EQ(material.glassTextureTransforms[0].scaleY, 3.0F);
    EXPECT_EQ(material.shadingModel, Engine::MaterialShadingModel::Standard);
}

TEST_F(GlassMaterialTests, CookedRoundTripAndVersionSixKeepTheirMaterialStride) {
    auto mesh = Engine::Cube::createMesh();
    Engine::PBRMaterial glass;
    glass.transmission = 1.0F;
    glass.ior = 1.52F;
    glass.thickness = 0.006F;
    glass.dirtIntensity = 0.4F;
    mesh.materials = {glass, Engine::PBRMaterial{}};
    const auto path = directory / "glass.gmesh";
    ASSERT_TRUE(Engine::Assets::save_gmesh(path, mesh));
    const auto loaded = Engine::Assets::load_gmesh(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->materials.size(), 2U);
    EXPECT_FLOAT_EQ(loaded->materials[0].transmission, 1.0F);
    EXPECT_FLOAT_EQ(loaded->materials[0].thickness, 0.006F);

    // gmesh 6 has the same header/geometry, followed by shorter material records.
    struct Header {
        std::array<char, 8> magic;
        std::uint32_t version, vertices, indices, meshlets, meshletVertices, meshletTriangles, materials, images,
            renderSections, meshletClusters, meshletClusterRoot;
        Engine::AABB localBounds;
    };
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>{file}, {}};
    Header header{};
    ASSERT_GE(bytes.size(), sizeof(header));
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.version = 6;
    std::memcpy(bytes.data(), &header, sizeof(header));
    const auto materialOffset = sizeof(Header) + header.vertices * sizeof(Engine::Vertex) +
                                header.indices * sizeof(std::uint32_t) + header.meshlets * sizeof(Engine::Meshlet) +
                                (header.meshletVertices + header.meshletTriangles) * sizeof(std::uint32_t);
    constexpr auto legacyStride = offsetof(Engine::PBRMaterial, transmission);
    for (std::size_t i = header.materials; i > 0; --i) {
        const auto first = materialOffset + (i - 1) * sizeof(Engine::PBRMaterial) + legacyStride;
        const auto last = materialOffset + i * sizeof(Engine::PBRMaterial);
        ASSERT_LE(last, bytes.size());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(first),
                    bytes.begin() + static_cast<std::ptrdiff_t>(last));
    }
    const auto legacyPath = directory / "legacy.gmesh";
    std::ofstream legacy(legacyPath, std::ios::binary);
    legacy.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    legacy.close();
    const auto old = Engine::Assets::load_gmesh(legacyPath);
    ASSERT_TRUE(old);
    ASSERT_EQ(old->materials.size(), 2U);
    EXPECT_FLOAT_EQ(old->materials[0].transmission, 0.0F);
    EXPECT_FLOAT_EQ(old->materials[0].ior, glass.ior);
    EXPECT_EQ(old->indices, loaded->indices);
    const auto geometry = Engine::Assets::load_gmesh_geometry(legacyPath);
    ASSERT_TRUE(geometry);
    EXPECT_EQ(geometry->indices, old->indices);
}
} // namespace
