import Common.SceneData;
import Common.ReflectionFallback;

// A fixed-capacity table avoids a CPU probe choice per instance. Only
// frame.reflectionProbeCount entries are valid and may be read.
struct ReflectionProbeData
{
    float4 positionAndBlendDistance;
    float4 extentsAndShape;
    uint textureIndex;
    int priority;
    uint flags;
    uint padding;
};
[[vk::binding(14, 0)]] StructuredBuffer<ReflectionProbeData> reflectionProbes;
// Fixed capacity deliberately avoids descriptor indexing/count changes while
// probes are baked. Entry zero is the global prefiltered environment, so an
// unbaked probe is safe to sample immediately.
[[vk::binding(15, 0)]] SamplerCube reflectionProbeTextures[256];

static const uint Probe_Box = 1u << 0u;
static const uint Probe_BoxProject = 1u << 1u;
static const uint Probe_Enabled = 1u << 2u;

float probeInfluence(ReflectionProbeData probe, float3 worldPosition)
{
    if ((probe.flags & Probe_Enabled) == 0u) return 0.0;
    float3 delta = abs(worldPosition - probe.positionAndBlendDistance.xyz);
    float boundary;
    if ((probe.flags & Probe_Box) != 0u)
        boundary = min(min(probe.extentsAndShape.x - delta.x, probe.extentsAndShape.y - delta.y),
                       probe.extentsAndShape.z - delta.z);
    else
        boundary = probe.extentsAndShape.x - length(delta);
    return saturate(boundary / max(probe.positionAndBlendDistance.w, 1e-4));
}

float3 boxProjectedReflection(ReflectionProbeData probe, float3 worldPosition, float3 direction)
{
    if ((probe.flags & (Probe_Box | Probe_BoxProject)) != (Probe_Box | Probe_BoxProject)) return direction;
    float3 local = worldPosition - probe.positionAndBlendDistance.xyz;
    float3 safeDirection = direction + sign(direction) * 1e-5;
    float3 positive = (probe.extentsAndShape.xyz - local) / safeDirection;
    float3 negative = (-probe.extentsAndShape.xyz - local) / safeDirection;
    float3 distances = float3(safeDirection.x > 0.0 ? positive.x : negative.x,
                              safeDirection.y > 0.0 ? positive.y : negative.y,
                              safeDirection.z > 0.0 ? positive.z : negative.z);
    float hitDistance = min(min(distances.x, distances.y), distances.z);
    return normalize(local + direction * max(hitDistance, 0.0));
}

ReflectionCandidate sampleLocalProbes(RendererFrameData frame, StructuredBuffer<uint> clusterLightIndices, float3 worldPosition, float3 reflection, float roughness,
                                      uint prefilteredMipCount, ClusterLightRange clusterRange)
{
    if (frame.reflectionProbeCount == 0u)
    {
        ReflectionCandidate none = {float3(0.0), 0.0};
        return none;
    }

    int bestPriority = -2147483647;
    int secondPriority = -2147483647;
    float bestWeight = 0.0;
    float secondWeight = 0.0;
    ReflectionProbeData best;
    ReflectionProbeData second;
    // Probe indices occupy a separate fixed-stride region in the cluster list,
    // so this loop never loads local-light indices.
    [loop] for (uint candidate = 0u; candidate < clusterRange.probeCount; ++candidate)
    {
        const uint index = clusterLightIndices[clusterRange.probeOffset + candidate];
        if (index >= frame.reflectionProbeCount) continue;
        ReflectionProbeData probe = reflectionProbes[index];
        float weight = probeInfluence(probe, worldPosition);
        if (weight <= 0.0) continue;
        if (probe.priority > bestPriority || (probe.priority == bestPriority && weight > bestWeight))
        {
            second = best; secondPriority = bestPriority; secondWeight = bestWeight;
            best = probe; bestPriority = probe.priority; bestWeight = weight;
        }
        else if (probe.priority > secondPriority || (probe.priority == secondPriority && weight > secondWeight))
        {
            second = probe; secondPriority = probe.priority; secondWeight = weight;
        }
    }
    float total = bestWeight + secondWeight;
    if (total <= 0.0) { ReflectionCandidate none = {float3(0.0), 0.0}; return none; }
    float mip = roughness * float(max(prefilteredMipCount, 1u) - 1u);
    const uint bestTexture = min(best.textureIndex, 255u);
    float3 value = reflectionProbeTextures[NonUniformResourceIndex(bestTexture)].SampleLevel(
        boxProjectedReflection(best, worldPosition, reflection), mip).rgb * bestWeight;
    if (secondWeight > 0.0)
    {
        const uint secondTexture = min(second.textureIndex, 255u);
        value += reflectionProbeTextures[NonUniformResourceIndex(secondTexture)].SampleLevel(
            boxProjectedReflection(second, worldPosition, reflection), mip).rgb * secondWeight;
    }
    ReflectionCandidate result = {value / total, saturate(total)};
    return result;
}

