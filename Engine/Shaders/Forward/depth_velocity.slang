// Minimal opaque depth/velocity prepass.  Keep this deliberately independent
// from forward_pbr: depth must not pay for BRDF, IBL, clustered lights, GTAO
// or virtual-shadow projection a second time.
import Common.SceneData;
import Common.VertexFrame;
import Common.Displacement;

#ifndef DEPTH_OUTPUT_VELOCITY
#define DEPTH_OUTPUT_VELOCITY 1
#endif
#ifndef FORWARD_MESHLET_INDEXED
#define FORWARD_MESHLET_INDEXED 0
#endif
// 0 for scenes without displaced materials: the vertex stage then skips the
// material fetch, and the six interpolants that only the displaced normal
// reconstruction reads are not written. Nsight showed the prepass vertex
// warps stalled half the time on attribute storage (ISBE) with them.
#ifndef DEPTH_DISPLACEMENT
#define DEPTH_DISPLACEMENT 1
#endif
// 1 replaces the vertex stage with a mesh shader fed by the binned visible
// meshlet list (Common/MeshletDraw.slang).
#ifndef FORWARD_MESH_SHADER
#define FORWARD_MESH_SHADER 0
#endif
#if FORWARD_MESH_SHADER
import Common.MeshletDraw;
#endif
// 1 in the _vis mesh-shader variants: colour target 0 is the R32_UINT
// visibility buffer, which receives each pixel's meshlet entry and triangle.
#ifndef DEPTH_VISIBILITY_ID
#define DEPTH_VISIBILITY_ID 0
#endif
// 0 in the `_opaque` mesh-shader variants, which draw a slot's one-sided
// meshlets (never masked or blended, ForwardPass::drawMeshShaderMaterial):
// no alpha test, so the depth test runs before the fragment stage. A
// `discard` anywhere in the shader otherwise moves every fragment of the
// draw to late depth testing, and occluded fragments pay for the material
// fetch and three colour writes.
#ifndef DEPTH_ALPHA_TEST
#define DEPTH_ALPHA_TEST 1
#endif

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if DEPTH_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif

struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(3)]] uint tangentFrame : NORMAL; // GpuVertex::packTangentFrame
    property float3 normal { get { return decodeVertexNormal(tangentFrame); } }
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    property float4 tangent { get { return decodeVertexTangent(tangentFrame, decodeVertexNormal(tangentFrame)); } }
};
struct VertexOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 texCoord;
    [[vk::location(1)]] float2 texCoord1;
    [[vk::location(2)]] nointerpolation uint materialIndex;
    [[vk::location(5)]] float3 worldNormal;
#if DEPTH_DISPLACEMENT
    [[vk::location(6)]] float3 baseWorldPosition;
    [[vk::location(7)]] float3 worldDisplacementDirection;
#endif
#if DEPTH_OUTPUT_VELOCITY
    // The current NDC position is SV_Position: interpolating it as well cost
    // two of the mesh stage's per-vertex outputs.
    [[vk::location(4)]] float4 previousClip;
#endif
};

float3x3 rotation(float4 q) {
    return float3x3(
        1.0 - 2.0 * (q.y*q.y + q.z*q.z), 2.0 * (q.x*q.y + q.z*q.w), 2.0 * (q.x*q.z - q.y*q.w),
        2.0 * (q.x*q.y - q.z*q.w), 1.0 - 2.0 * (q.x*q.x + q.z*q.z), 2.0 * (q.y*q.z + q.x*q.w),
        2.0 * (q.x*q.z + q.y*q.w), 2.0 * (q.y*q.z - q.x*q.w), 1.0 - 2.0 * (q.x*q.x + q.y*q.y));
}

// Shared by the vertex and mesh stages: `visibleIndex` addresses
// RendererInstanceData.
VertexOutput shadeVertex(VertexInput input, uint visibleIndex) {
    RendererInstanceData instance = visibleInstances[visibleIndex];
    uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
#if DEPTH_DISPLACEMENT
    MaterialData material = materials[materialIndex];
    const DisplacedVertex displaced = displaceMaterialVertex(
        material, input.position, input.normal, input.tangent,
        input.texCoord, input.texCoord1);
#else
    DisplacedVertex displaced;
    displaced.position = input.position;
    displaced.normal = normalize(input.normal);
    displaced.tangent = input.tangent;
#endif
    const float3 localPosition = displaced.position;
    float3 world = mul(localPosition * instance.scaleBase.xyz, rotation(instance.rotation)) + instance.positionMaterial.xyz;
    VertexOutput output;
    float4 currentClip = mul(mul(float4(world, 1.0), frame.view), frame.projection);
    output.position = currentClip;
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.materialIndex = materialIndex;
    output.worldNormal = normalize(mul(displaced.normal / instance.scaleBase.xyz, rotation(instance.rotation)));
#if DEPTH_DISPLACEMENT
    output.baseWorldPosition = mul(input.position * instance.scaleBase.xyz,
                                   rotation(instance.rotation)) + instance.positionMaterial.xyz;
    output.worldDisplacementDirection = mul(normalize(input.normal) * instance.scaleBase.xyz,
                                            rotation(instance.rotation));
#endif
#if DEPTH_OUTPUT_VELOCITY
    RendererPreviousTransformData previous = previousTransforms[visibleIndex];
    float3 previousWorld = mul(localPosition * previous.previousScale.xyz, rotation(previous.previousRotation)) + previous.previousPosition.xyz;
    output.previousClip = mul(mul(float4(previousWorld, 1.0), frame.previousView), frame.previousProjection);
#endif
    return output;
}

#if FORWARD_MESH_SHADER
#if DEPTH_VISIBILITY_ID
struct MeshPrimitive { bool cull : SV_CullPrimitive; uint visibility : SV_PrimitiveID; };
#else
struct MeshPrimitive { bool cull : SV_CullPrimitive; };
#endif
groupshared float4 meshletClip[MeshletMaxVertices];

[numthreads(MeshletThreads, 1, 1)]
[outputtopology("triangle")]
[shader("mesh")]
void meshMain(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID,
              out vertices VertexOutput vertices[MeshletMaxVertices],
              out indices uint3 triangles[MeshletMaxTriangles],
              out primitives MeshPrimitive primitives[MeshletMaxTriangles]) {
    MeshletDrawEntry entry;
    uint listIndex;
    bool valid = meshletDrawEntryFor(groupId, entry, listIndex);
    // The draws already cover one group each; this only catches a two-sided
    // entry written over a one-sided one when the slot's range overflowed.
    if (valid && (meshletDraw.flags & (entry.twoSided ? MeshletDrawSkipTwoSided : MeshletDrawSkipOneSided)) != 0u)
        valid = false;
    MeshletRecord meshlet = (MeshletRecord)0;
    if (valid) meshlet = meshletRecords[entry.meshlet];
    SetMeshOutputCounts(meshlet.range.y, meshlet.range.w);
    for (uint vertex = threadId; vertex < meshlet.range.y; vertex += MeshletThreads) {
        const MeshletPackedVertex packed = meshletVertexHeap[meshletVertexRefs[meshlet.range.x + vertex]];
        VertexInput input;
        input.position = packed.position;
        input.tangentFrame = packed.tangentFrame;
        input.texCoord = unpackMeshletHalf2(packed.texCoord);
        input.texCoord1 = unpackMeshletHalf2(packed.texCoord1);
        input.materialIndex = packed.materialIndex;
        const VertexOutput output = shadeVertex(input, entry.visibleInstance);
        vertices[vertex] = output;
        meshletClip[vertex] = output.position;
    }
    GroupMemoryBarrierWithGroupSync();
    // Not `triangle`: that name is an HLSL/Slang primitive keyword, and with
    // -g2 debug names NVIDIA's compiler then drops every primitive.
    for (uint tri = threadId; tri < meshlet.range.w; tri += MeshletThreads) {
        const uint3 corners = unpackMeshletTriangle(meshletTriangleRefs[meshlet.range.z + tri]);
        triangles[tri] = corners;
        primitives[tri].cull = meshletTriangleCulled(
            meshletClip[corners.x], meshletClip[corners.y], meshletClip[corners.z],
            frame.clusteredViewportNearFar.xy, entry.twoSided);
#if DEPTH_VISIBILITY_ID
        primitives[tri].visibility = (meshletDraw.flags & MeshletDrawVisibility) != 0u
                                         ? packVisibility(meshletDraw.slot, listIndex, tri)
                                         : VisibilityNone;
#endif
    }
}
#else
[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                        uint baseInstance : SV_StartInstanceLocation) {
#if FORWARD_MESHLET_INDEXED
    uint visibleIndex = baseInstance + instanceIndex;
#else
    uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
#endif
    return shadeVertex(input, visibleIndex);
}
#endif

void alphaTest(VertexOutput input) {
#if !DEPTH_ALPHA_TEST
    return;
#endif
    MaterialData material = materials[input.materialIndex];
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // Opaque materials never touch textures in this pass.
    if (alphaMode == 0) return;
    float alpha = material.roughnessAmbientOcclusion.w;
    if (material.textureIndices.x >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.textureIndices.x)].Sample(
            materialTextureUv(material, 0, input.texCoord, input.texCoord1)).a;
    if (material.auxiliaryTextureIndices.y >= 0)
        alpha *= materialTextures[NonUniformResourceIndex(material.auxiliaryTextureIndices.y)].Sample(
            materialTextureUv(material, 4, input.texCoord, input.texCoord1)).r;
    const float cutoff = alphaMode == 1 ? material.roughnessAmbientOcclusion.z : 0.30;
    if (alpha < cutoff) discard;
}

#if DEPTH_OUTPUT_VELOCITY
float2 encodeOct(float3 n) { n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-5); if(n.z < 0) n.xy = (1-abs(n.yx))*float2(n.x>=0?1:-1,n.y>=0?1:-1); return n.xy; }
struct FragmentOutput {
#if DEPTH_VISIBILITY_ID
    [[vk::location(0)]] uint visibility : SV_Target0;
#endif
    [[vk::location(1)]] float2 velocity : SV_Target1;
    [[vk::location(2)]] float2 viewNormal : SV_Target2;
};
[shader("fragment")]
#if !DEPTH_ALPHA_TEST
[earlydepthstencil]
#endif
FragmentOutput fragmentMain(VertexOutput input, bool frontFace : SV_IsFrontFace
#if DEPTH_VISIBILITY_ID
                            , uint primitiveVisibility : SV_PrimitiveID
#endif
                            ) {
    MaterialData material = materials[input.materialIndex];
#if DEPTH_DISPLACEMENT
    float3 worldNormal = displacedMaterialWorldNormal(
        material, input.baseWorldPosition, input.worldNormal, input.worldDisplacementDirection,
        input.texCoord, input.texCoord1);
#else
    float3 worldNormal = normalize(input.worldNormal);
#endif
    alphaTest(input);
    FragmentOutput output;
    // Positive-height viewport over the whole target: NDC from the pixel position.
    const float2 currentNdc = input.position.xy * (2.0 / frame.clusteredViewportNearFar.xy) - 1.0;
    output.velocity = (currentNdc - input.previousClip.xy / input.previousClip.w) * 0.5;
    float3 viewNormal = normalize(mul(float4(worldNormal, 0.0), frame.view).xyz);
    if ((material.textureIndices.w & 1) != 0 && !frontFace) viewNormal = -viewNormal;
    output.viewNormal = encodeOct(viewNormal);
#if DEPTH_VISIBILITY_ID
    output.visibility = primitiveVisibility;
#endif
    return output;
}
#else
[shader("fragment")]
#if !DEPTH_ALPHA_TEST
[earlydepthstencil]
#endif
void fragmentMain(VertexOutput input) { alphaTest(input); }
#endif
