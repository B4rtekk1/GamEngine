// DDGI probe rays, traced with a ray tracing pipeline against a partitioned
// TLAS (VK_NV_partitioned_acceleration_structure) of cluster BLASes
// (VK_NV_cluster_acceleration_structure), which only pipelines may traverse.
// Shading happens in the ray generation shader after
// ReorderThread(hitObject): shader execution reordering regroups the warp by
// instance so material fetches and texture reads stay coherent. No closest-hit
// shader runs; the hit object supplies everything the shading needs.
// ClusterID() (the DXR Shader Model 6.10 intrinsic, SPV_NV_cluster_acceleration_structure)
// names the hit cluster directly, including clusters instantiated from
// templates for displaced geometry, so no per-instance offset is needed.
import Common.SceneData;
import Common.VertexFrame;
import Common.DdgiCommon;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(11, 0)]] SamplerCube prefilteredEnvironmentMap;
[[vk::binding(13, 0)]] Sampler2D materialTextures[];

struct DdgiInstance { float4 objectToWorld[3]; uint4 ids; }; // ids.y: material base
struct GpuMeshlet { uint4 range; float4 bounds; float4 cone; uint4 material; };

[[vk::binding(0, 1)]] ConstantBuffer<DdgiVolume> volume;
[[vk::binding(1, 1)]] Sampler2D irradianceAtlas;
[[vk::binding(2, 1)]] Sampler2D distanceAtlas;
// x: ray, y: update-list slot. rgb: radiance, a: hit distance (negative for back faces).
[[vk::binding(3, 1), vk::image_format("rgba32f")]] RWTexture2D<float4> rayData;
[[vk::binding(4, 1)]] StructuredBuffer<uint> probeList;
[[vk::binding(5, 1)]] StructuredBuffer<DdgiInstance> instances;
// Indexed by ClusterID. x: global meshlet, y: geometry index, z: template slot.
[[vk::binding(6, 1)]] StructuredBuffer<uint4> clusterMeshlets;
[[vk::binding(7, 1)]] StructuredBuffer<GpuMeshlet> meshlets;
[[vk::binding(8, 1)]] StructuredBuffer<uint> meshletVertices;
[[vk::binding(9, 1)]] StructuredBuffer<uint> meshletTriangles;
[[vk::binding(10, 1)]] ByteAddressBuffer vertices;

// A partitioned TLAS is bound by address: it is traced exactly like a TLAS.
struct Settings { uint64_t sceneAddress; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

struct Payload { uint unused; };

static const uint VertexStride = 32u; // sizeof(GpuVertex)
static const uint HitKindBackFace = 0xFFu;

struct HitVertex { float3 position; float3 normal; float2 uv0; float2 uv1; float3 color; uint material; };

HitVertex loadVertex(uint index) {
    const uint offset = index * VertexStride;
    HitVertex v;
    v.position = asfloat(vertices.Load3(offset));
    v.normal = decodeVertexNormal(vertices.Load(offset + 12u));
    const uint uv0 = vertices.Load(offset + 16u), uv1 = vertices.Load(offset + 20u);
    v.uv0 = float2(f16tof32(uv0 & 0xffffu), f16tof32(uv0 >> 16u));
    v.uv1 = float2(f16tof32(uv1 & 0xffffu), f16tof32(uv1 >> 16u));
    const uint color = vertices.Load(offset + 24u);
    v.color = float3(color & 255u, (color >> 8u) & 255u, (color >> 16u) & 255u) / 255.0;
    v.material = vertices.Load(offset + 28u);
    return v;
}

// Each probe ray reads one cascade; the probe's many rays and its hysteresis
// average the choice just as TAA does on screen.
float ddgiRayDither(uint2 index, uint frameIndex) {
    uint hash = index.x * 1664525u + index.y * 1013904223u + frameIndex * 747796405u;
    hash = (hash ^ (hash >> 16u)) * 2246822519u;
    hash ^= hash >> 13u;
    return float(hash & 0x00ffffffu) / float(0x01000000u);
}

float3 skyRadiance(float3 direction) {
    return prefilteredEnvironmentMap.SampleLevel(direction, 0.0).rgb;
}

bool occluded(RaytracingAccelerationStructure scene, float3 origin, float3 direction, float maxDistance) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;
    Payload payload = { 0u };
    const HitObject shadow = HitObject::TraceRay(
        scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE,
        0xff, 0, 0, 0, ray, payload);
    return shadow.IsHit();
}

// Material flag bit 0, as in the depth prepass: such surfaces have no inside.
bool doubleSided(HitObject hit) {
    const DdgiInstance instance = instances[hit.GetInstanceID()];
    const uint4 range = meshlets[clusterMeshlets[hit.GetClusterID()].x].range;
    const uint triangle = meshletTriangles[range.z + hit.GetPrimitiveIndex()];
    const uint vertex = meshletVertices[range.x + (triangle & 0xffu)];
    return (materials[instance.ids.y + vertices.Load(vertex * VertexStride + 28u)].textureIndices.w & 1) != 0;
}

float3 shadeHit(RaytracingAccelerationStructure scene, HitObject hit, float3 rayDirection, float hitDistance,
                float3 hitPosition) {
    const DdgiInstance instance = instances[hit.GetInstanceID()];
    const uint meshlet = clusterMeshlets[hit.GetClusterID()].x;
    const uint4 range = meshlets[meshlet].range;
    // PrimitiveIndex is cluster-local and the cluster keeps the meshlet's triangle order.
    const uint triangle = meshletTriangles[range.z + hit.GetPrimitiveIndex()];
    const HitVertex a = loadVertex(meshletVertices[range.x + (triangle & 0xffu)]);
    const HitVertex b = loadVertex(meshletVertices[range.x + ((triangle >> 8u) & 0xffu)]);
    const HitVertex c = loadVertex(meshletVertices[range.x + ((triangle >> 16u) & 0xffu)]);
    const float2 barycentrics = hit.GetAttributes<BuiltInTriangleIntersectionAttributes>().barycentrics;
    const float3 w = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);

    // Normals use the cofactor of the object-to-world 3x3, which stays
    // perpendicular under non-uniform and mirrored scale.
    const float3 axisX = float3(instance.objectToWorld[0].x, instance.objectToWorld[1].x, instance.objectToWorld[2].x);
    const float3 axisY = float3(instance.objectToWorld[0].y, instance.objectToWorld[1].y, instance.objectToWorld[2].y);
    const float3 axisZ = float3(instance.objectToWorld[0].z, instance.objectToWorld[1].z, instance.objectToWorld[2].z);
    const float handedness = dot(axisX, cross(axisY, axisZ)) < 0.0 ? -1.0 : 1.0;
    const float3 localNormal = a.normal * w.x + b.normal * w.y + c.normal * w.z;
    float3 normal = normalize((localNormal.x * cross(axisY, axisZ) + localNormal.y * cross(axisZ, axisX) +
                               localNormal.z * cross(axisX, axisY)) * handedness);
    // Two-sided and mirrored assets may carry normals facing away from the ray.
    if (dot(normal, rayDirection) > 0.0) normal = -normal;

    const MaterialData material = materials[instance.ids.y + a.material];
    const float2 uv0 = a.uv0 * w.x + b.uv0 * w.y + c.uv0 * w.z;
    const float2 uv1 = a.uv1 * w.x + b.uv1 * w.y + c.uv1 * w.z;
    const float3 vertexColor = (material.textureIndices.w & 32) != 0 || a.color.r < 0.0
        ? 1.0 : a.color * w.x + b.color * w.y + c.color * w.z;
    float3 albedo = material.baseColorMetallic.rgb * vertexColor;
    if (material.textureIndices.x >= 0) {
        // GI is low frequency: a coarse mip keeps the incoherent fetch cheap.
        const float lod = clamp(log2(1.0 + hitDistance) + 2.0, 2.0, 10.0);
        albedo *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)]
                      .SampleLevel(materialTextureUv(material, 0, uv0, uv1), lod).rgb;
    }
    const float3 diffuseAlbedo = albedo * (1.0 - saturate(material.baseColorMetallic.a));

    float3 direct = 0.0;
    const float3 origin = hitPosition + normal * 0.01;
    if (frame.lightDirectionIntensity.w > 0.0) {
        const float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
        const float nDotL = dot(normal, lightDirection);
        if (nDotL > 0.0 && !occluded(scene, origin, lightDirection, 1.0e4))
            direct += frame.lightColor.rgb * frame.lightDirectionIntensity.w * nDotL;
    }
    // Local lights are unshadowed here; their bounce is a small fraction of the total.
    for (uint light = 0u; light < min(frame.localLightCount, 32u); ++light) {
        const LocalLightData local = frame.localLights[light];
        const float3 toLight = local.positionRange.xyz - hitPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = local.positionRange.w;
        if (distanceSquared <= 1e-8 || distanceSquared >= radius * radius) continue;
        const float inverseDistance = rsqrt(distanceSquared);
        const float3 direction = toLight * inverseDistance;
        const float rangeFactor = saturate(1.0 - distanceSquared * inverseDistance / radius);
        float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
        attenuation *= localLightProfile(local, -direction);
        if (local.parameters.y > 1.5)
            attenuation *= smoothstep(local.directionOuterCos.w, local.parameters.x,
                                      dot(normalize(-local.directionOuterCos.xyz), direction));
        direct += local.colorIntensity.rgb * local.colorIntensity.w * max(dot(normal, direction), 0.0) * attenuation;
    }
    // Last frame's probes light the hit: one more bounce per update.
    float probeWeight;
    const float3 bounced =
        ddgiIrradiance(volume, irradianceAtlas, distanceAtlas, hitPosition, normal, -rayDirection,
                       ddgiRayDither(DispatchRaysIndex().xy, volume.atlasInfo.w), probeWeight);
    const float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    return diffuseAlbedo * (direct / DdgiPi + bounced * probeWeight) + emissive;
}

[shader("raygeneration")]
void rayGen() {
    const uint ray = DispatchRaysIndex().x, slot = DispatchRaysIndex().y;
    const uint probe = probeList[slot];
    const uint cascade = probe / uint(volume.probeCounts.w);
    const uint local = probe - cascade * uint(volume.probeCounts.w);
    const int3 grid = ddgiGridFromStorage(volume, cascade, ddgiStorageFromIndex(local, volume.probeCounts.xyz));
    const float3 probePosition = float3(grid) * ddgiCascadeSpacing(volume, cascade);
    const float3 direction = ddgiRayDirection(volume, ray);
    const float maxDistance = ddgiMaxRayDistance(volume, cascade);
    const RaytracingAccelerationStructure scene = RaytracingAccelerationStructure(settings.sceneAddress);

    RayDesc desc;
    desc.Origin = probePosition;
    desc.Direction = direction;
    desc.TMin = 0.0;
    desc.TMax = maxDistance;
    Payload payload = { 0u };
    HitObject hit = HitObject::TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xff, 0, 0, 0, desc, payload);
    // Misses and hits form separate groups automatically; the hint sorts hits by instance.
    ReorderThread(hit, hit.IsHit() ? hit.GetInstanceID() & 0xffffu : 0u, 16);

    float4 result;
    if (!hit.IsHit()) {
        result = float4(skyRadiance(direction), maxDistance * 1.5);
    } else {
        const float distance = hit.GetRayDesc().TMax;
        if (hit.GetHitKind() == HitKindBackFace && !doubleSided(hit)) {
            // DDGI convention: back faces shorten the distance to pull the
            // probe's visibility in and flag the probe as buried.
            result = float4(0.0, 0.0, 0.0, -distance * 0.2);
        } else {
            float3 radiance = shadeHit(scene, hit, direction, distance, probePosition + direction * distance);
            const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
            if (luminance > 64.0) radiance *= 64.0 / luminance;
            result = float4(radiance, distance);
        }
    }
    if (any(isnan(result))) result = float4(0.0, 0.0, 0.0, maxDistance);
    rayData[int2(ray, slot)] = result;
}

[shader("miss")]
void miss(inout Payload payload) { payload.unused = 0u; }

[shader("closesthit")]
void closestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.unused = 1u; }
