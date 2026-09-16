// Fine-grained visibility is instance-relative: GpuMeshlet bounds and cones
// are in mesh local space, while the camera and frustum planes are world space.

struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };
struct GPUSceneInstanceRecord { float4x4 worldMatrix; float4 localBoundsMin; float4 localBoundsMax; uint4 idsAndFlags; };
struct GPUSceneMeshRecord { uint4 draw; uint4 lod; }; // lod: lod2, firstMeshlet, meshletCount, reserved
struct VisibleMeshlet { uint instanceId; uint meshletId; };
struct MeshletCullUniforms { float4x4 viewProjection; float3 cameraPosition; uint meshletCount; float4 frustumPlanes[6]; };

[[vk::binding(0, 0)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(1, 0)]] StructuredBuffer<GPUSceneInstanceRecord> instances;
[[vk::binding(2, 0)]] StructuredBuffer<GPUSceneMeshRecord> meshes;
[[vk::binding(3, 0)]] StructuredBuffer<uint> visibleInstances;
[[vk::binding(4, 0)]] StructuredBuffer<uint> visibleInstanceCount;
[[vk::binding(5, 0)]] RWStructuredBuffer<VisibleMeshlet> visibleMeshlets;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> visibleCount;
[[vk::binding(7, 0)]] ConstantBuffer<MeshletCullUniforms> culling;

// One workgroup owns one coarse-visible instance.  Cache its basis-vector
// lengths once; every meshlet in the group uses the same world transform.
groupshared float4 instanceScale;

bool outsideFrustum(float3 center, float radius) {
    for (uint plane = 0; plane < 6; ++plane)
        if (dot(culling.frustumPlanes[plane].xyz, center) + culling.frustumPlanes[plane].w < -radius) return true;
    return false;
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
            visible = !outsideFrustum(worldCenter, radius);
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
