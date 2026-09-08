#include "Engine/Renderer/ShaderGraph/ShaderGraphCompiler.h"

#include <gtest/gtest.h>

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

} // namespace Engine
