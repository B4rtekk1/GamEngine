#pragma once

#include "Engine/Renderer/RenderGraph/RenderGraph.h"

#include <functional>

namespace Engine::Lighting {

struct ReflectionGraphInputs final {
    RenderGraph::TextureHandle sceneColor;
    RenderGraph::TextureHandle depth;
    RenderGraph::TextureHandle globalSky;
    RenderGraph::TextureHandle localProbeAtlas;
    RenderGraph::TextureHandle rayTracedReflections;
    RenderGraph::TextureDesc reflectionDesc;
    bool enableSsr{};
};

struct ReflectionGraphOutputs final {
    RenderGraph::TextureHandle screenSpace;
    RenderGraph::TextureHandle resolved;
};

/** Declares the strict RT -> SSR -> probe -> sky reflection fallback DAG. */
inline ReflectionGraphOutputs addReflectionPasses(
    RenderGraph::RenderGraph& graph, const ReflectionGraphInputs& inputs,
    RenderGraph::RenderGraph::ExecuteCallback recordSsr = {},
    RenderGraph::RenderGraph::ExecuteCallback recordResolve = {}) {
    using namespace RenderGraph;
    ReflectionGraphOutputs outputs{};
    if (inputs.enableSsr) {
        graph.addPass("Screen-space reflections", Queue::AsyncCompute,
            [&](PassBuilder& builder) {
                builder.read(inputs.sceneColor, TextureUsage::SampledReadCompute);
                builder.read(inputs.depth, TextureUsage::SampledReadCompute);
                outputs.screenSpace = builder.writeTexture("SSR confidence + radiance",
                    inputs.reflectionDesc, TextureUsage::StorageWriteCompute);
            }, std::move(recordSsr));
    }
    graph.addPass("Reflection fallback resolve", Queue::Graphics,
        [&](PassBuilder& builder) {
            builder.read(inputs.globalSky, TextureUsage::SampledReadFragment);
            builder.read(inputs.localProbeAtlas, TextureUsage::SampledReadFragment);
            if (outputs.screenSpace) builder.read(outputs.screenSpace, TextureUsage::SampledReadFragment);
            if (inputs.rayTracedReflections)
                builder.read(inputs.rayTracedReflections, TextureUsage::SampledReadFragment);
            outputs.resolved = builder.writeTexture("Resolved reflections", inputs.reflectionDesc,
                                                     TextureUsage::ColorAttachment);
        }, std::move(recordResolve));
    return outputs;
}

} // namespace Engine::Lighting
