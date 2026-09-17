// Fine-grained visibility is instance-relative: GpuMeshlet bounds and cones
// are in mesh local space, while the camera and frustum planes are world space.

struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };
struct GPUSceneInstanceRecord { float4x4 worldMatrix; float4 localBoundsMin; float4 localBoundsMax; uint4 idsAndFlags; };
struct GPUSceneMeshRecord { uint4 draw; uint4 lod; uint4 clusters; }; // clusters: first, root, count, reserved
struct GpuMeshletCluster { float4 bounds; float4 cone; uint4 range; };
struct VisibleMeshlet { uint instanceId; uint meshletId; };
struct MeshletCullUniforms { float4x4 viewProjection; float4x4 occlusionViewProjection; float3 cameraPosition; uint meshletCount; float4 frustumPlanes[6]; float viewportWidth; float viewportHeight; float depthBias; uint hiZMipCount; uint enableOcclusionCulling; uint cameraCut; uint2 reserved; };

[[vk::binding(0, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(2, 0)]] StructuredBuffer<GPUSceneMeshRecord> meshes;
[[vk::binding(3, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(4, 0)]] StructuredBuffer<uint> visibleInstanceCount;
[[vk::binding(5, 0)]] RWStructuredBuffer<VisibleMeshlet> visibleMeshlets;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(7, 0)]] ConstantBuffer<MeshletCullUniforms> culling;
[[vk::binding(8, 0)]] Sampler2D hiZTexture;
[[vk::binding(9, 0)]] StructuredBuffer<GpuMeshletCluster> clusters;

// One workgroup owns one coarse-visible instance.  Cache its basis-vector
// lengths once; every meshlet in the group uses the same world transform.
groupshared float4 instanceScale;
groupshared uint clusterVisible;

bool rangeContains(uint first, uint count, uint value) {
    return value >= first && value - first < count;
}

bool outsideFrustum(float3 center, float radius) {
    for (uint plane = 0; plane < 6; ++plane)
        if (dot(culling.frustumPlanes[plane].xyz, center) + culling.frustumPlanes[plane].w < -radius) return true;
    return false;
}

// Conservative projected sphere rectangle. The near-facing sphere point is
// used for depth; objects crossing the near plane remain visible.
bool sphereOccluded(float3 center, float radius) {
    if (culling.enableOcclusionCulling == 0u || culling.cameraCut != 0u || culling.hiZMipCount == 0u)
        return false;
    const float4 clip = mul(float4(center, 1.0), culling.occlusionViewProjection);
    if (clip.w <= radius || clip.z <= 0.0) return false;
    const float2 ndc = clip.xy / clip.w;
    const float radiusNdc = radius / max(clip.w - radius, 1.0e-4);
    float2 minimumUv = saturate((ndc - radiusNdc) * float2(0.5, -0.5) + 0.5);
    float2 maximumUv = saturate((ndc + radiusNdc) * float2(0.5, -0.5) + 0.5);
    const float loY = min(minimumUv.y, maximumUv.y);
    const float hiY = max(minimumUv.y, maximumUv.y);
    minimumUv.y = loY; maximumUv.y = hiY;
    const float extent = max(maximumUv.x - minimumUv.x, maximumUv.y - minimumUv.y) *
                         max(culling.viewportWidth, culling.viewportHeight);
    const uint mip = uint(clamp(ceil(log2(max(extent, 1.0))), 0.0, float(culling.hiZMipCount - 1u)));
    uint width, height, mipCount;
    hiZTexture.GetDimensions(mip, width, height, mipCount);
    const int2 size = int2(width, height);
    const int2 lo = clamp(int2(floor(minimumUv * float2(size))), int2(0), size - 1);
    const int2 hi = clamp(int2(floor(maximumUv * float2(size))), int2(0), size - 1);
    float depth = hiZTexture.Load(int3(lo, int(mip))).r;
    depth = max(depth, hiZTexture.Load(int3(hi.x, lo.y, int(mip))).r);
    depth = max(depth, hiZTexture.Load(int3(lo.x, hi.y, int(mip))).r);
    depth = max(depth, hiZTexture.Load(int3(hi, int(mip))).r);
    const float nearestDepth = max(0.0, (clip.z - radius) / clip.w);
    return nearestDepth > depth + culling.depthBias;
}

bool sphereTooSmall(float3 center, float radius) {
    const float4 clip = mul(float4(center, 1.0), culling.viewProjection);
    if (clip.w <= radius || clip.z <= 0.0) return false;
    // Do not descend into a hierarchy node that cannot cover a pixel.  The
    // threshold deliberately remains below one pixel to avoid visible LOD
    // popping while still pruning distant Megascans aggressively.
    const float projectedRadius = radius / max(clip.w - radius, 1.0e-4) *
                                  0.5 * max(culling.viewportWidth, culling.viewportHeight);
    return projectedRadius < 0.5;
}

bool coneVisible(float4 cone, float3 center, bool uniformScale) {
    if (cone.w < -1.0 || !uniformScale) return true;
    const float3 axis = normalize(cone.xyz);
    const float3 toCamera = normalize(culling.cameraPosition - center);
    const float threshold = -sqrt(max(0.0, 1.0 - cone.w * cone.w));
    return dot(axis, toCamera) > threshold;
}

// Descend only through the branch that owns meshletId. Child and descendant
// ranges are contiguous in the baked hierarchy, therefore this work is
// bounded by hierarchy depth rather than meshlet count. Every visited node is
// a coarse frustum/cone/Hi-Z rejection opportunity before fine culling.
bool clusterPathVisible(GPUSceneMeshRecord mesh, uint meshletId,
                        GPUSceneInstanceRecord instance, bool uniformScale) {
    if (mesh.clusters.z == 0u || !rangeContains(mesh.lod.y, mesh.lod.z, meshletId)) return true;

    uint nodeId = mesh.clusters.y;
    [loop] for (uint depth = 0u; depth < 32u; ++depth) {
        const GpuMeshletCluster node = clusters[nodeId];
        const float3 center = mul(float4(node.bounds.xyz, 1.0), instance.worldMatrix).xyz;
        const float radius = node.bounds.w * instanceScale.w;
        if (outsideFrustum(center, radius) || !coneVisible(node.cone, center, uniformScale) ||
            sphereOccluded(center, radius)) return false;
        if (node.range.y == 0u) return true;

        uint childId = 0xffffffffu;
        [unroll] for (uint child = 0u; child < 8u; ++child) {
            if (child >= node.range.y) break;
            const uint candidateId = node.range.x + child;
            const GpuMeshletCluster candidate = clusters[candidateId];
            if (rangeContains(candidate.range.z, candidate.range.w, meshletId)) {
                childId = candidateId;
                break;
            }
        }
        // CPU upload validates the hierarchy. Retaining a candidate on an
        // invalid branch is conservative and avoids a false reject on corrupt
        // streamed data.
        if (childId == 0xffffffffu) return true;
        nodeId = childId;
    }
    return true;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint visibleIndex = groupId.x;
    if (visibleIndex >= visibleInstanceCount[0]) return;
    const uint instanceId = visibleInstances[visibleIndex];
    const GPUSceneInstanceRecord instance = instances[instanceId];
    const GPUSceneMeshRecord mesh = meshes[instance.idsAndFlags.x];

    if (threadId.x == 0) {
        const float3 axisX = mul(float4(1.0, 0.0, 0.0, 0.0), instance.worldMatrix).xyz;
        const float3 axisY = mul(float4(0.0, 1.0, 0.0, 0.0), instance.worldMatrix).xyz;
        const float3 axisZ = mul(float4(0.0, 0.0, 1.0, 0.0), instance.worldMatrix).xyz;
        const float scaleX = length(axisX), scaleY = length(axisY), scaleZ = length(axisZ);
        instanceScale = float4(scaleX, scaleY, scaleZ, max(scaleX, max(scaleY, scaleZ)));
    }
    GroupMemoryBarrierWithGroupSync();

    // Test the hierarchy root once before expanding. Fine culling descends
    // through the remaining branch for each candidate meshlet below.
    if (threadId.x == 0) {
        clusterVisible = 1u;
        if (mesh.lod.z != 0u) {
            const GpuMeshletCluster root = clusters[mesh.clusters.y];
            const float3 center = mul(float4(root.bounds.xyz, 1.0), instance.worldMatrix).xyz;
            const float radius = root.bounds.w * instanceScale.w;
            const bool uniformScale = abs(instanceScale.x - instanceScale.y) < 1.0e-5 &&
                                      abs(instanceScale.x - instanceScale.z) < 1.0e-5;
            clusterVisible = !outsideFrustum(center, radius) &&
                             coneVisible(root.cone, center, uniformScale) &&
                             !sphereOccluded(center, radius) ? 1u : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (clusterVisible == 0u) return;

    uint capacity, stride;
    visibleMeshlets.GetDimensions(capacity, stride);

    // Use rounds rather than a lane-strided loop so every active lane enters
    // each wave operation, including the final partial round.
    const uint meshletRounds = (mesh.lod.z + 63u) / 64u;
    for (uint round = 0; round < meshletRounds; ++round) {
        const uint localMeshlet = round * 64u + threadId.x;
        const uint meshletId = mesh.lod.y + localMeshlet;
        bool visible = localMeshlet < mesh.lod.z && meshletId < culling.meshletCount;
        GpuMeshlet meshlet = (GpuMeshlet)0;
        float3 worldCenter = 0.0;

        if (visible) {
            meshlet = meshlets[meshletId];
            worldCenter = mul(float4(meshlet.bounds.xyz, 1.0), instance.worldMatrix).xyz;
            const float radius = meshlet.bounds.w * instanceScale.w;
            const bool uniformScale = abs(instanceScale.x - instanceScale.y) < 1.0e-5 &&
                                      abs(instanceScale.x - instanceScale.z) < 1.0e-5;
            visible = clusterPathVisible(mesh, meshletId, instance, uniformScale) &&
                      !outsideFrustum(worldCenter, radius);
            if (visible) visible = !sphereOccluded(worldCenter, radius);
        }

        // A cone cutoff is not invariant under non-uniform scale. Skipping
        // the optional backface test in that case is conservative.
        const bool uniformScale = abs(instanceScale.x - instanceScale.y) < 1.0e-5 &&
                                  abs(instanceScale.x - instanceScale.z) < 1.0e-5;
        if (visible && meshlet.cone.w >= -1.0 && uniformScale) {
            const float3 worldConeAxis = normalize(mul(float4(meshlet.cone.xyz, 0.0), instance.worldMatrix).xyz);
            const float3 toCamera = normalize(culling.cameraPosition - worldCenter);
            const float threshold = -sqrt(max(0.0, 1.0 - meshlet.cone.w * meshlet.cone.w));
            visible = dot(worldConeAxis, toCamera) > threshold;
        }

        // Reserve one contiguous range per wave, rather than one atomic per
        // visible meshlet. All lanes execute the wave operations uniformly.
        const uint visibleInWave = WaveActiveCountBits(visible);
        uint waveBase = 0;
        if (WaveIsFirstLane() && visibleInWave != 0)
            InterlockedAdd(visibleCount[0], visibleInWave, waveBase);
        waveBase = WaveReadLaneFirst(waveBase);
        if (visible) {
            const uint destination = waveBase + WavePrefixCountBits(visible);
            if (destination < capacity)
                visibleMeshlets[destination] = VisibleMeshlet(instanceId, meshletId);
        }
    }
}
