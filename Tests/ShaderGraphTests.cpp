#include "Engine/Renderer/ShaderGraph/ShaderGraphCompiler.h"
#include "Engine/Renderer/ShaderGraph/ShaderGraphSerializer.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeFactory.h"
#include "Engine/Renderer/ShaderGraph/ShaderNodeRegistry.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

namespace Engine {
    namespace {
        ShaderGraphAsset basicGraph() {
            return {.id = 1, .name = "ConstantTint", .nodes = {
                {.id = 1, .type = ShaderNodeType::Float, .outputs = {{.id = 10, .name = "Value", .type = ShaderValueType::Float}}, .value = 2.0F},
                {.id = 2, .type = ShaderNodeType::Float, .outputs = {{.id = 20, .name = "Value", .type = ShaderValueType::Float}}, .value = 3.0F},
                {.id = 3, .type = ShaderNodeType::Multiply, .inputs = {{.id = 30, .name = "A", .type = ShaderValueType::Float}, {.id = 31, .name = "B", .type = ShaderValueType::Float}}, .outputs = {{.id = 32, .name = "Result", .type = ShaderValueType::Float}}},
                {.id = 4, .type = ShaderNodeType::SurfaceOutput, .inputs = {{.id = 40, .name = "Base Color", .type = ShaderValueType::Float3}, {.id = 41, .name = "Metallic", .type = ShaderValueType::Float}, {.id = 42, .name = "Roughness", .type = ShaderValueType::Float}, {.id = 43, .name = "Normal", .type = ShaderValueType::Float3}, {.id = 44, .name = "Ambient Occlusion", .type = ShaderValueType::Float}, {.id = 45, .name = "Emission", .type = ShaderValueType::Float3}, {.id = 46, .name = "Alpha", .type = ShaderValueType::Float}, {.id = 47, .name = "Alpha Cutoff", .type = ShaderValueType::Float}}}
            }, .links = {{.fromPin = 10, .toPin = 30}, {.fromPin = 20, .toPin = 31}, {.fromPin = 32, .toPin = 40}}};
        }
    }

    TEST(ShaderGraphCompiler, FoldsReachableScalarConstantsAndSplatsSurfaceColor) {
        const ShaderGraphCompileResult result = ShaderGraphCompiler{}.compile(basicGraph());
        ASSERT_TRUE(result.succeeded());
        EXPECT_EQ(result.ir.instructions.size(), 3U);
        EXPECT_NE(result.slang.find("float v2 = 6;"), std::string::npos);
        EXPECT_NE(result.slang.find("result.baseColor = float3(v2);"), std::string::npos);
    }

    TEST(ShaderGraphCompiler, RejectsCyclesReachableFromOutput) {
        ShaderGraphAsset graph = basicGraph();
        graph.links[0] = {.fromPin = 32, .toPin = 30};
        const ShaderGraphCompileResult result = ShaderGraphCompiler{}.compile(graph);
        ASSERT_FALSE(result.succeeded());
        EXPECT_NE(result.diagnostics.front().message.find("cycle"), std::string::npos);
    }

    TEST(ShaderNodeRegistry, CreatesMvpNodesWithStablePins) {
        ShaderPinId nextPin = 1;
        const ShaderNode combine = ShaderNodeFactory::create(ShaderNodeType::Combine, 7, nextPin);
        const ShaderNode split = ShaderNodeFactory::create(ShaderNodeType::Split, 8, nextPin);
        ASSERT_EQ(combine.inputs.size(), 3U);
        ASSERT_EQ(combine.outputs.front().type, ShaderValueType::Float3);
        ASSERT_EQ(split.inputs.front().type, ShaderValueType::Float3);
        EXPECT_EQ(split.outputs.size(), 3U);
        EXPECT_EQ(ShaderNodeRegistry::find(ShaderNodeType::Sin)->category, "Math");
        EXPECT_EQ(ShaderNodeRegistry::find(ShaderNodeType::Sin)->subcategory, "Trigonometry");
    }

    TEST(ShaderGraphCompiler, EmitsRegisteredMvpMath) {
        ShaderPinId nextPin = 1;
        ShaderNode value = ShaderNodeFactory::create(ShaderNodeType::Float, 1, nextPin);
        value.value = 0.5F;
        ShaderNode sine = ShaderNodeFactory::create(ShaderNodeType::Sin, 2, nextPin);
        ShaderNode saturate = ShaderNodeFactory::create(ShaderNodeType::Saturate, 3, nextPin);
        ShaderNode output = ShaderNodeFactory::create(ShaderNodeType::SurfaceOutput, 4, nextPin);
        ShaderGraphAsset graph{.id = 2, .name = "MvpMath", .nodes = {value, sine, saturate, output},
                               .links = {{.id = 1, .fromPin = value.outputs[0].id, .toPin = sine.inputs[0].id},
                                         {.id = 2, .fromPin = sine.outputs[0].id, .toPin = saturate.inputs[0].id},
                                         {.id = 3, .fromPin = saturate.outputs[0].id, .toPin = output.inputs[6].id}}};
        const ShaderGraphCompileResult result = ShaderGraphCompiler{}.compile(graph);
        ASSERT_TRUE(result.succeeded());
        EXPECT_NE(result.slang.find("sin(v0)"), std::string::npos);
        EXPECT_NE(result.slang.find("saturate(v1)"), std::string::npos);
    }

    TEST(ShaderGraphSerializer, WritesStableNodeTypeIdsAndMigratesV1SurfaceOutput) {
        const auto path = std::filesystem::temp_directory_path() / "gamengine_shader_graph_serializer_test.shadergraph";
        const ShaderGraphAsset graph{.id = 1, .name = "Serializer", .nodes = {
            {.id = 1, .type = ShaderNodeType::SurfaceOutput},
            {.id = 2, .type = ShaderNodeType::Time},
        }};
        ShaderGraphSerializer::save(graph, path);

        std::ifstream saved(path);
        std::string contents{std::istreambuf_iterator<char>{saved}, {}};
        EXPECT_NE(contents.find("SHADERGRAPH 2"), std::string::npos);
        EXPECT_NE(contents.find("NODE 1 surface_output"), std::string::npos);
        EXPECT_EQ(ShaderGraphSerializer::load(path).nodes[0].type, ShaderNodeType::SurfaceOutput);

        {
            std::ofstream legacy(path, std::ios::trunc);
            legacy << "SHADERGRAPH 1\nGRAPH 2 \"Legacy\"\nNODE 1 20 0 0 0 NONE\nENDNODE\nEND\n";
        }
        const ShaderGraphAsset migrated = ShaderGraphSerializer::load(path);
        ASSERT_EQ(migrated.nodes.size(), 1U);
        EXPECT_EQ(migrated.nodes[0].type, ShaderNodeType::SurfaceOutput);
        std::filesystem::remove(path);
    }

} // namespace Engine
