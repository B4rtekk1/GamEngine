import Common.PbrLighting;
import Common.VertexFrame;
import Common.ReflectionFallback;
import Common.ReflectionProbes;
import Common.SceneData;
import Common.ShadowSampling;
import Common.Displacement;
import Common.DdgiCommon;
import Common.DeferredLocalLighting;

// GENERATED_SHADER_GRAPH_DECLARATIONS

#ifndef MATERIAL_VARIANT
#define MATERIAL_VARIANT 0
#endif

// 0 basic, 3 normal, 4 extended, 5 foliage, 6 terrain.
#ifdef ENGINE_SHADER_GRAPH
#define PBR_HAS_NORMAL 1
#define PBR_HAS_EXTENSIONS 1
#define PBR_HAS_PROBES 1
#else
#define PBR_HAS_NORMAL (MATERIAL_VARIANT == 3 || MATERIAL_VARIANT == 4 || MATERIAL_VARIANT == 5 || MATERIAL_VARIANT == 6)
#define PBR_HAS_EXTENSIONS (MATERIAL_VARIANT == 4 || MATERIAL_VARIANT == 5 || MATERIAL_VARIANT == 6)
#define PBR_HAS_PROBES (MATERIAL_VARIANT == 4 || MATERIAL_VARIANT == 5 || MATERIAL_VARIANT == 6)
#endif

#ifndef FORWARD_OUTPUT_VELOCITY
#define FORWARD_OUTPUT_VELOCITY 1
#endif
#ifndef FORWARD_RESOLVED_VSM
#define FORWARD_RESOLVED_VSM 0
#endif
// Resolved variants are selected only for a lighting pass that reuses the
// depth prepass with an EQUAL depth test. The prepass already rejected
// cutout texels, so a discard here would only force late depth testing.
#ifndef FORWARD_DEPTH_PREPASSED
#define FORWARD_DEPTH_PREPASSED FORWARD_RESOLVED_VSM
// The Game View lighting pass's PBR programs (single-sample, after the depth
// prepass, no velocity) leave local lights to the deferred compute pass when
// it runs: they write their shading inputs instead (Common/DeferredLocalLighting).
#if FORWARD_RESOLVED_VSM && !FORWARD_OUTPUT_VELOCITY && !defined(ENGINE_SHADER_GRAPH) && \
    (MATERIAL_VARIANT == 0 || MATERIAL_VARIANT >= 3)
#define FORWARD_DEFERRED_LOCAL 1
#else
#define FORWARD_DEFERRED_LOCAL 0
#endif
#endif
#ifndef FORWARD_MESHLET_INDEXED
#define FORWARD_MESHLET_INDEXED 0
#endif
// 1 replaces the vertex stage with a mesh shader fed by the binned visible
// meshlet list (Common/MeshletDraw.slang).
#ifndef FORWARD_MESH_SHADER
#define FORWARD_MESH_SHADER 0
#endif
#if FORWARD_MESH_SHADER
import Common.MeshletDraw;
#endif
// 0 in the _flat mesh-shader variants, used while no material is displaced:
// the vertices skip the material fetch and the two displacement interpolants.
// A mesh workgroup keeps all of its vertex outputs on chip, so on NVIDIA the
// output size bounds how many workgroups overlap the heavy fragment work.
#ifndef FORWARD_DISPLACEMENT
#define FORWARD_DISPLACEMENT 1
#endif
// 0 in the _noddgi variants, used while no probe volume is valid. The probe
// lookup is never executed then, but its registers are still allocated for
// every pixel: compiling it out made the opaque pass ~19% faster (Nsight).
#ifndef FORWARD_DDGI
#define FORWARD_DDGI 1
#endif
// 0 in the _deferred variants, used while the deferred compute pass adds the
// local lights: like the DDGI lookup, the light walk and its shadow-mask
// reads keep their registers allocated for every pixel even when skipped.
#ifndef FORWARD_LOCAL_LIGHT_LOOP
#define FORWARD_LOCAL_LIGHT_LOOP 1
#endif
// 1 builds the visibility-buffer resolve (compute entry visibilityResolveMain)
// instead of the raster stages: each pixel's triangle comes from the
// prepass's R32_UINT ids, its attributes are interpolated with analytic
// barycentrics and the same surface shading runs once per visible pixel,
// without the helper lanes of partly covered quads.
#ifndef FORWARD_VISIBILITY_RESOLVE
#define FORWARD_VISIBILITY_RESOLVE 0
#endif
#if FORWARD_VISIBILITY_RESOLVE
#if FORWARD_MESH_SHADER
#error The visibility resolve replaces the raster stages
#endif
import Common.MeshletDraw;
// The prepass already rejected cutout texels; compute cannot discard.
#define FORWARD_DISCARD {}
// Screen-space inputs and the BRDF LUT have one mip: no derivatives needed.
#define screenSample(tex, uv) (tex).SampleLevel((uv), 0.0)
#else
#define FORWARD_DISCARD discard
#define screenSample(tex, uv) (tex).Sample(uv)
#endif

static const uint PBR_DEBUG_FINAL_LIGHTING = 0u;
static const uint PBR_DEBUG_NO_SPECULAR_IBL = 1u;
static const uint PBR_DEBUG_NO_DIRECT_SPECULAR = 2u;
static const uint PBR_DEBUG_NO_GTAO = 3u;
static const uint PBR_DEBUG_GEOMETRIC_DIRECT_DIFFUSE = 4u;
static const uint PBR_DEBUG_ALBEDO = 5u;
static const uint PBR_DEBUG_NORMALS = 6u;
static const uint PBR_DEBUG_ROUGHNESS = 7u;
static const uint PBR_DEBUG_METALLIC = 8u;
static const uint PBR_DEBUG_AO = 9u;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(5, 0)]] StructuredBuffer<RendererInstanceData> visibleInstances;
[[vk::binding(6, 0)]] StructuredBuffer<uint> visibleInstanceIndices;
#if FORWARD_OUTPUT_VELOCITY
[[vk::binding(8, 0)]] StructuredBuffer<RendererPreviousTransformData> previousTransforms;
#endif
// Dynamic GTAO is deliberately sampled independently of material AO.  It is
// full resolution after a depth-aware upsample and therefore needs no final
// HDR darkening pass.
[[vk::binding(16, 0)]] Sampler2D gtaoVisibility;
[[vk::binding(18, 0)]] Sampler2D rtContactVisibility;
// Full-resolution screen-space diffuse GI, already in irradiance-map units.
// Sampled only while bit 1 of `screenSpaceEffects` says this view traced it.
[[vk::binding(20, 0)]] Sampler2D screenSpaceGi;
static const uint SCREEN_SPACE_EFFECTS_SSGI = 2u;
static const uint SCREEN_SPACE_EFFECTS_TAA = 4u;
// localShadowMask holds this view's ray-traced local light shadows.
static const uint SCREEN_SPACE_EFFECTS_LOCAL_SHADOWS = 8u;
// x: light cluster walked + 1 (0: none), y: occluded entries of its list.
[[vk::binding(25, 0)]] Sampler2D<uint4> localShadowMask;
// Game View's depth-bounded tile light lists (Culling/tile_light_culling.slang).
static const uint SCREEN_SPACE_EFFECTS_TILE_LIGHTS = 16u;
// Local lights are added by the deferred compute pass (Game View only).
static const uint SCREEN_SPACE_EFFECTS_DEFERRED_LOCAL = 32u;
[[vk::binding(26, 0)]] StructuredBuffer<uint> tileLightIndices;
// World-space ray-traced probes. The volume is per frame and view independent,
// so Scene View samples it as well; gridMin.w is zero while it holds no data.
[[vk::binding(21, 0)]] Sampler2D ddgiIrradianceAtlas;
[[vk::binding(22, 0)]] Sampler2D ddgiDistanceAtlas;
[[vk::binding(23, 0)]] ConstantBuffer<DdgiVolume> ddgiVolume;
#if FORWARD_RESOLVED_VSM
[[vk::binding(19, 0)]] Sampler2D directionalVisibility;
#endif

struct ForwardVertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 color : COLOR0;
    [[vk::location(2)]] float2 texCoord : TEXCOORD0;
    [[vk::location(4)]] float2 texCoord1 : TEXCOORD1;
    [[vk::location(3)]] uint tangentFrame : NORMAL; // GpuVertex::packTangentFrame
    property float3 normal { get { return decodeVertexNormal(tangentFrame); } }
    [[vk::location(8)]] uint materialIndex : MATERIAL_INDEX;
    property float4 tangent { get { return decodeVertexTangent(tangentFrame, decodeVertexNormal(tangentFrame)); } }
};

struct ForwardVertexOutput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
#if FORWARD_DISPLACEMENT
    [[vk::location(9)]] float3 baseWorldPosition;
    [[vk::location(10)]] float3 worldDisplacementDirection;
#endif
#if MATERIAL_VARIANT == 5
    // Object pivot, the centre the card normals are bent around.
    [[vk::location(11)]] nointerpolation float3 instancePivot;
#endif
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(7)]] noperspective float2 currentNdc;
    [[vk::location(8)]] float4 previousClip;
#endif
};

// Shared by the vertex and mesh stages: `visibleIndex` addresses
// RendererInstanceData.
ForwardVertexOutput shadeForwardVertex(ForwardVertexInput input, uint visibleIndex)
{
    RendererInstanceData instance = visibleInstances[visibleIndex];
    const uint materialIndex = asuint(instance.positionMaterial.w) + input.materialIndex;
#if FORWARD_DISPLACEMENT
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

    float4 q = instance.rotation;
    float3x3 rotationMatrix = float3x3(
        1.0 - 2.0 * (q.y * q.y + q.z * q.z), 2.0 * (q.x * q.y + q.z * q.w), 2.0 * (q.x * q.z - q.y * q.w),
        2.0 * (q.x * q.y - q.z * q.w), 1.0 - 2.0 * (q.x * q.x + q.z * q.z), 2.0 * (q.y * q.z + q.x * q.w),
        2.0 * (q.x * q.z + q.y * q.w), 2.0 * (q.y * q.z - q.x * q.w), 1.0 - 2.0 * (q.x * q.x + q.y * q.y));
    float3 worldPosition3 = mul(localPosition * instance.scaleBase.xyz, rotationMatrix) + instance.positionMaterial.xyz;
    float4 worldPosition = float4(worldPosition3, 1.0);
    float3x3 normalMatrix = float3x3(rotationMatrix[0] / instance.scaleBase.x,
                                     rotationMatrix[1] / instance.scaleBase.y,
                                     rotationMatrix[2] / instance.scaleBase.z);

    ForwardVertexOutput output;
    float4 currentClip = mul(mul(worldPosition, frame.view), frame.projection);
#if FORWARD_OUTPUT_VELOCITY
    RendererPreviousTransformData previousTransform = previousTransforms[visibleIndex];
    float4 previousQ = previousTransform.previousRotation;
    float3x3 previousRotationMatrix = float3x3(
        1.0 - 2.0 * (previousQ.y * previousQ.y + previousQ.z * previousQ.z), 2.0 * (previousQ.x * previousQ.y + previousQ.z * previousQ.w), 2.0 * (previousQ.x * previousQ.z - previousQ.y * previousQ.w),
        2.0 * (previousQ.x * previousQ.y - previousQ.z * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.z * previousQ.z), 2.0 * (previousQ.y * previousQ.z + previousQ.x * previousQ.w),
        2.0 * (previousQ.x * previousQ.z + previousQ.y * previousQ.w), 2.0 * (previousQ.y * previousQ.z - previousQ.x * previousQ.w), 1.0 - 2.0 * (previousQ.x * previousQ.x + previousQ.y * previousQ.y));
    float3 previousWorldPosition = mul(localPosition * previousTransform.previousScale.xyz, previousRotationMatrix) +
                                   previousTransform.previousPosition.xyz;
#endif
    output.position = currentClip;
    output.color = input.color;
    output.worldPosition = worldPosition.xyz;
    output.normal = mul(displaced.normal, normalMatrix);
#if FORWARD_DISPLACEMENT
    output.baseWorldPosition = mul(input.position * instance.scaleBase.xyz, rotationMatrix) +
                               instance.positionMaterial.xyz;
    output.worldDisplacementDirection = mul(normalize(input.normal) * instance.scaleBase.xyz,
                                            rotationMatrix);
#endif
    output.materialIndex = materialIndex;
#if MATERIAL_VARIANT == 5
    output.instancePivot = instance.positionMaterial.xyz;
#endif
    output.texCoord = input.texCoord;
    output.texCoord1 = input.texCoord1;
    output.tangent = float4(mul(displaced.tangent.xyz * instance.scaleBase.xyz, rotationMatrix),
                            displaced.tangent.w);
#if FORWARD_OUTPUT_VELOCITY
    output.currentNdc = currentClip.xy / currentClip.w;
    output.previousClip = mul(mul(float4(previousWorldPosition, 1.0), frame.previousView), frame.previousProjection);
#endif
    return output;
}

#if FORWARD_MESH_SHADER
struct ForwardMeshPrimitive { bool cull : SV_CullPrimitive; };
groupshared float4 meshletClip[MeshletMaxVertices];

[numthreads(MeshletThreads, 1, 1)]
[outputtopology("triangle")]
[shader("mesh")]
void meshMain(uint3 groupId : SV_GroupID, uint threadId : SV_GroupThreadID,
              out vertices ForwardVertexOutput vertices[MeshletMaxVertices],
              out indices uint3 triangles[MeshletMaxTriangles],
              out primitives ForwardMeshPrimitive primitives[MeshletMaxTriangles])
{
    MeshletDrawEntry entry;
    const bool valid = meshletDrawEntryFor(groupId, entry);
    MeshletRecord meshlet = (MeshletRecord)0;
    if (valid)
        meshlet = meshletRecords[entry.meshlet];
    SetMeshOutputCounts(meshlet.range.y, meshlet.range.w);
    for (uint vertex = threadId; vertex < meshlet.range.y; vertex += MeshletThreads)
    {
        const MeshletPackedVertex packed = meshletVertexHeap[meshletVertexRefs[meshlet.range.x + vertex]];
        ForwardVertexInput input;
        input.position = packed.position;
        input.color = unpackMeshletColor(packed.color);
        input.texCoord = unpackMeshletHalf2(packed.texCoord);
        input.texCoord1 = unpackMeshletHalf2(packed.texCoord1);
        input.tangentFrame = packed.tangentFrame;
        input.materialIndex = packed.materialIndex;
        const ForwardVertexOutput output = shadeForwardVertex(input, entry.visibleInstance);
        vertices[vertex] = output;
        meshletClip[vertex] = output.position;
    }
    GroupMemoryBarrierWithGroupSync();
    // Not `triangle`; see depth_velocity.slang.
    for (uint tri = threadId; tri < meshlet.range.w; tri += MeshletThreads)
    {
        const uint3 corners = unpackMeshletTriangle(meshletTriangleRefs[meshlet.range.z + tri]);
        triangles[tri] = corners;
        primitives[tri].cull = meshletTriangleCulled(
            meshletClip[corners.x], meshletClip[corners.y], meshletClip[corners.z],
            frame.clusteredViewportNearFar.xy, entry.twoSided);
    }
}
#elif !FORWARD_VISIBILITY_RESOLVE
[shader("vertex")]
ForwardVertexOutput vertexMain(ForwardVertexInput input, uint instanceIndex : SV_InstanceID,
                               uint baseInstance : SV_StartInstanceLocation)
{
#if FORWARD_MESHLET_INDEXED
    const uint visibleIndex = baseInstance + instanceIndex;
#else
    const uint visibleIndex = visibleInstanceIndices[baseInstance + instanceIndex];
#endif
    return shadeForwardVertex(input, visibleIndex);
}
#endif

[[vk::binding(0, 0)]] Sampler2DShadow shadowMap;
[[vk::binding(9, 0)]] Sampler2D shadowDepthMap;
[[vk::binding(17, 0)]] Sampler2DShadow linearShadowMap;
[[vk::binding(2, 0)]] StructuredBuffer<MaterialData> materials;
[[vk::binding(10, 0)]] SamplerCube irradianceMap;
[[vk::binding(11, 0)]] SamplerCube prefilteredEnvironmentMap;
[[vk::binding(12, 0)]] Sampler2D brdfIntegrationLut;
[[vk::binding(4, 0)]] StructuredBuffer<uint4> shadowPageTable;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(7, 0)]] StructuredBuffer<uint> clusterLightIndices;
// Every local light; frame.localLights holds only the first 32.
[[vk::binding(24, 0)]] StructuredBuffer<LocalLightData> localLights;
struct ForwardFragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 color;
    [[vk::location(1)]] float3 worldPosition;
    [[vk::location(2)]] float3 normal;
    [[vk::location(3)]] nointerpolation uint materialIndex;
    [[vk::location(4)]] float2 texCoord;
    [[vk::location(5)]] float4 tangent;
    [[vk::location(6)]] float2 texCoord1;
#if FORWARD_DISPLACEMENT
    [[vk::location(9)]] float3 baseWorldPosition;
    [[vk::location(10)]] float3 worldDisplacementDirection;
#endif
#if MATERIAL_VARIANT == 5
    // Object pivot, the centre the card normals are bent around.
    [[vk::location(11)]] nointerpolation float3 instancePivot;
#endif
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(7)]] noperspective float2 currentNdc;
    [[vk::location(8)]] float4 previousClip;
#endif
    bool isFrontFace : SV_IsFrontFace;
#if FORWARD_VISIBILITY_RESOLVE
    // Screen-space derivatives the raster stages take from the quad.
    float2 texCoordDx;
    float2 texCoordDy;
    float2 texCoord1Dx;
    float2 texCoord1Dy;
    float3 normalDx;
    float3 normalDy;
#endif
};

// A material texture through its UV set and transform. The visibility
// resolve has no quads, so it maps the analytic UV derivatives through the
// same (affine) transform and passes them as gradients.
float4 sampleMaterialTexture(MaterialData material, int textureIndex, uint slot, ForwardFragmentInput input)
{
    const float2 uv = materialTextureUv(material, slot, input.texCoord, input.texCoord1);
#if FORWARD_VISIBILITY_RESOLVE
    const float2 uvDx = materialTextureUv(material, slot, input.texCoord + input.texCoordDx,
                                          input.texCoord1 + input.texCoord1Dx) - uv;
    const float2 uvDy = materialTextureUv(material, slot, input.texCoord + input.texCoordDy,
                                          input.texCoord1 + input.texCoord1Dy) - uv;
    return materialTextures[NonUniformResourceIndex(textureIndex)].SampleGrad(uv, uvDx, uvDy);
#else
    return materialTextures[NonUniformResourceIndex(textureIndex)].Sample(uv);
#endif
}

struct ForwardFragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target;
#if FORWARD_OUTPUT_VELOCITY
    [[vk::location(1)]] float2 velocity : SV_Target1;
#endif
#if FORWARD_DEFERRED_LOCAL
    [[vk::location(1)]] uint4 localLightData : SV_Target1;
#endif
};

#if MATERIAL_VARIANT == 5
// How far card normals bend towards the volume normal, and the wrap of the
// diffuse term that stands in for light scattering through the leaves.
static const float FOLIAGE_NORMAL_BEND = 0.65;
static const float FOLIAGE_DIFFUSE_WRAP = 0.4;
// Default leaf transmission, and the share of it that survives the leaf's
// own sun shadow (light arriving through the foliage in front of it).
static const float FOLIAGE_TRANSMISSION = 0.5;
static const float FOLIAGE_SHADOWED_TRANSMISSION = 0.3;
#endif

float foliageNDotL(bool doubleSided, float3 normal, float3 lightDirection)
{
#if MATERIAL_VARIANT == 5
    // Bent normals face away from the light on the far side of the volume;
    // wrapped lighting keeps that side soft instead of black, while abs()
    // would light it as brightly as the near side and flatten the shape.
    if (doubleSided)
        return saturate((dot(normal, lightDirection) + FOLIAGE_DIFFUSE_WRAP) / (1.0 + FOLIAGE_DIFFUSE_WRAP));
#endif
    return doubleSided ? abs(dot(normal, lightDirection)) : max(dot(normal, lightDirection), 0.0);
}

ForwardFragmentOutput shadeForwardFragment(ForwardFragmentInput input)
{
    MaterialData material = materials[input.materialIndex];
#if FORWARD_DISPLACEMENT
    const float3 displacedGeometricNormal = displacedMaterialWorldNormal(
        material, input.baseWorldPosition, input.normal, input.worldDisplacementDirection,
        input.texCoord, input.texCoord1);
#else
    const float3 displacedGeometricNormal = normalize(input.normal);
#endif
    float3 vertexColor = input.color;
    if (input.color.r < 0.0)
    {
        float cellCount = max(input.color.g, 1.0);
        int2 cell = int2(clamp(floor(input.texCoord * cellCount),
                              float2(0.0), float2(cellCount - 1.0)));
        float shade = ((cell.x + cell.y) & 1) == 0 ? 0.82 : 0.52;
        vertexColor = shade;
    }

    // The material flag disables albedo modulation for FBX colors with
    // unspecified semantics and for foliage wind/mask data.
    float3 albedoMultiplier = (material.textureIndices.w & 32) != 0 ? 1.0 : vertexColor;
    float4 baseColor = float4(albedoMultiplier * material.baseColorMetallic.rgb,
                              material.roughnessAmbientOcclusion.w);
// Unlit materials never use terrain layering.  Keeping this out of their
// module is materially different from a runtime branch: the four descriptor
// samples and their live values disappear before register allocation.
#if MATERIAL_VARIANT == 6 || defined(ENGINE_SHADER_GRAPH)
    if ((material.textureIndices.w & 8) != 0)
    {
        baseColor = float4(material.baseColorMetallic.rgb, material.roughnessAmbientOcclusion.w);
        float3 weights = saturate(input.color);
        float fourthWeight = saturate(1.0 - weights.x - weights.y - weights.z);
        float4 layers = float4(weights.x, weights.y, weights.z, fourthWeight);
        // Terrain UVs cover the complete heightmap. Repeat each layer so an
        // albedo map reads as ground detail rather than one giant decal.
        // A denser repeat keeps brush-painted material layers at a usable
        // ground-detail scale instead of spreading one oversized texture
        // across a large section of the terrain.
        float2 terrainUv = input.texCoord * 16.0;
        int2 cell = int2(floor(input.texCoord * 32.0));
        float checkerShade = ((cell.x + cell.y) & 1) == 0 ? 0.82 : 0.52;
        float4 checker = float4(checkerShade * material.baseColorMetallic.rgb, 1.0);
        baseColor = 0.0;
        float appliedWeight = 0.0;
        if (material.terrainLayerTextures.x >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.x)].Sample(terrainUv) * layers.x; appliedWeight += layers.x; }
        if (material.terrainLayerTextures.y >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.y)].Sample(terrainUv) * layers.y; appliedWeight += layers.y; }
        if (material.terrainLayerTextures.z >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.z)].Sample(terrainUv) * layers.z; appliedWeight += layers.z; }
        if (material.terrainLayerTextures.w >= 0) { baseColor += materialTextures[NonUniformResourceIndex(material.terrainLayerTextures.w)].Sample(terrainUv) * layers.w; appliedWeight += layers.w; }
        // Blend the unpainted part into the grid instead of leaving it with
        // a zero alpha/colour, which exposed the blue clear colour at edges.
        baseColor = baseColor * float4(material.baseColorMetallic.rgb, 1.0) +
                    checker * (1.0 - saturate(appliedWeight));
        baseColor.a = 1.0;
    }
    else
#endif
    if (material.textureIndices.x >= 0)
        baseColor *= sampleMaterialTexture(material, material.textureIndices.x, 0, input);

    // Cooker may pack this map into base-color alpha; retain the separate
    // path for uncooked glTF/Quixel assets.
    if (material.auxiliaryTextureIndices.y >= 0)
        baseColor.a *= sampleMaterialTexture(material, material.auxiliaryTextureIndices.y, 4, input).r;

    const bool doubleSided = (material.textureIndices.w & 1) != 0;
    const uint alphaMode = uint((material.textureIndices.w >> 1) & 3);
    // The cutout runs after the depth prepass too, with the same alpha as
    // depth_velocity: the lighting pass tests LESS_OR_EQUAL (not EQUAL), so
    // a leaf card's transparent texels lie in front of the stored depth and
    // would otherwise be shaded as a solid quad. Without depth writes the
    // discard does not disable early depth testing.
    // The current two-sided foliage stream is a depth-writing cutout stream.
    // Importers commonly mark vegetation cards as BLEND even though their
    // opacity map is binary. Treat those cards as cutouts until they are
    // routed through a dedicated, sorted transparent stream.
    if (alphaMode == 1)
    {
        if (baseColor.a < material.roughnessAmbientOcclusion.z)
            FORWARD_DISCARD;
        baseColor.a = 1.0;
    }
    else if (alphaMode == 2)
    {
        if (baseColor.a < 0.30)
            FORWARD_DISCARD;
        baseColor.a = 1.0;
    }
    else if (alphaMode == 0) baseColor.a = 1.0;

#if MATERIAL_VARIANT == 1
    // This return deliberately precedes metallic/roughness, normal mapping,
    // VSM, clusters, probes, GTAO and IBL.  The unlit PSO is consequently a
    // real compile-time specialization rather than a late colour override.
    float3 unlitEmissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    if (material.extensionTextureIndices.x >= 0)
        unlitEmissive *= sampleMaterialTexture(material, material.extensionTextureIndices.x, 7, input).rgb;
    ForwardFragmentOutput unlitOutput;
    unlitOutput.color = float4(baseColor.rgb + unlitEmissive, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    const float2 unlitPreviousNdc = input.previousClip.xy / input.previousClip.w;
    unlitOutput.velocity = (input.currentNdc - unlitPreviousNdc) * 0.5;
#endif
    return unlitOutput;
#endif

    float3 albedo = baseColor.rgb;
    float metallic = material.baseColorMetallic.a;
    float roughness = material.roughnessAmbientOcclusion.x;
    if (material.textureIndices.y >= 0)
    {
        float4 metallicRoughness =
            sampleMaterialTexture(material, material.textureIndices.y, 1, input);
        metallic *= metallicRoughness.b;
        roughness *= metallicRoughness.g;
    }
    metallic = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.045, 1.0);

    // Keep the unperturbed, geometric normal.  It is required to prevent a
    // normal map from reflecting the environment through the surface.
    float3 geometricNormal = displacedGeometricNormal;
    float faceSign = doubleSided && !input.isFrontFace ? -1.0 : 1.0;
    geometricNormal *= faceSign;
    float3 normal = geometricNormal;
#if PBR_HAS_NORMAL || MATERIAL_VARIANT == 2
    if (material.textureIndices.z >= 0 && abs(material.extensionScalars.x) > 1e-6)
    {
        float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz)) * faceSign;
        float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
        float2 mappedXY = sampleMaterialTexture(material, material.textureIndices.z, 2, input).rg * 2.0 - 1.0;
        if ((material.textureIndices.w & 128) != 0)
            mappedXY.y = -mappedXY.y;
        float mappedZ = sqrt(saturate(1.0 - dot(mappedXY, mappedXY)));
        float3 mappedNormal = float3(mappedXY * material.extensionScalars.x, mappedZ);
        float3x3 tangentFrame = float3x3(tangent, bitangent, normal);
        normal = normalize(mul(mappedNormal, tangentFrame));
    }
#endif

#if MATERIAL_VARIANT == 5
    // A bush or hedge is hundreds of flat two-sided cards. Lit with their own
    // plane normals every card reads as a separate flat tile, so bend them
    // towards the direction from the object's pivot: the foliage then shades
    // as one volume, bright towards the light and darker on the far side.
    // The geometric normal (shadow offset, horizon occlusion) is unchanged.
    if (doubleSided)
    {
        const float3 fromPivot = input.worldPosition - input.instancePivot;
        const float pivotDistanceSquared = dot(fromPivot, fromPivot);
        if (pivotDistanceSquared > 1.0e-6)
            normal = normalize(lerp(normal, fromPivot * rsqrt(pivotDistanceSquared), FOLIAGE_NORMAL_BEND));
    }
#endif

#if MATERIAL_VARIANT == 2
    // Hologram keeps its authored albedo and normal-map silhouette but does
    // not retain the full PBR/VSM/cluster/probe working set.
    const float3 hologramView = normalize(frame.cameraPosition.xyz - input.worldPosition);
    const float scanline = 0.55 + 0.45 * sin(input.worldPosition.y * 90.0 + frame.windGustFrequencyTime.w * 8.0);
    const float hologramFresnel = pow(1.0 - saturate(dot(normal, hologramView)), 3.0);
    const float3 hologramEmission = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    ForwardFragmentOutput hologramOutput;
    hologramOutput.color = float4(baseColor.rgb * (0.25 + scanline * 0.75) + hologramEmission +
                                       float3(0.05, 0.65, 1.0) * hologramFresnel, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    const float2 hologramPreviousNdc = input.previousClip.xy / input.previousClip.w;
    hologramOutput.velocity = (input.currentNdc - hologramPreviousNdc) * 0.5;
#endif
    return hologramOutput;
#endif

    // This isolates the invariant baseline: no normal map, Fresnel, IBL,
    // specular, AO, shadows or local lights may affect it. It is deliberately
    // evaluated before material-graph normal overrides and specular AA.
    if (frame.pbrDebugView == PBR_DEBUG_GEOMETRIC_DIRECT_DIFFUSE)
    {
        ForwardFragmentOutput diagnosticOutput;
        const float3 diagnosticLightDirection = normalize(-frame.lightDirectionIntensity.xyz);
        diagnosticOutput.color = float4(albedo * max(dot(geometricNormal, diagnosticLightDirection), 0.0), baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        diagnosticOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return diagnosticOutput;
    }

    float3 viewDirection = normalize(frame.cameraPosition.xyz - input.worldPosition);
#ifdef ENGINE_SHADER_GRAPH
    SurfaceInput graphInput;
    graphInput.worldPosition = input.worldPosition;
    graphInput.worldNormal = normal;
    graphInput.viewDirection = viewDirection;
    graphInput.uv0 = input.texCoord;
    graphInput.uv1 = input.texCoord1;
    graphInput.tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
    graphInput.bitangent = normalize(cross(normal, graphInput.tangent)) * input.tangent.w;
    graphInput.time = frame.windGustFrequencyTime.w;
    MaterialSurface graphSurface = evaluateMaterial(graphInput);
    baseColor = float4(graphSurface.baseColor, graphSurface.alpha);
    albedo = graphSurface.baseColor;
    metallic = clamp(graphSurface.metallic, 0.0, 1.0);
    roughness = clamp(graphSurface.roughness, 0.045, 1.0);
    normal = normalize(graphSurface.normal);
#endif
    // Specular anti-aliasing: high-frequency normal maps otherwise steer
    // adjacent pixels to unrelated parts of the environment, especially at
    // grazing angles. Derivative-based normal-to-roughness broadens the GGX
    // lobe before both direct-light and IBL BRDF evaluation.
    // Kaplanyan & Hoffman / Tokuyoshi: the screen-space variance of the
    // normal widens the lobe in alpha^2 space. Geometric curvature counts too,
    // so rounded curb and cobble edges no longer alias into white rims when
    // no temporal filter hides them.
    {
#if FORWARD_VISIBILITY_RESOLVE
        // Derivatives of the interpolated vertex normal, from the triangle's
        // barycentric derivatives; the normal map's own variance is left out.
        const float3 normalDx = input.normalDx;
        const float3 normalDy = input.normalDy;
#else
        const float3 normalDx = ddx(normal);
        const float3 normalDy = ddy(normal);
#endif
        const float variance = 0.25 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
        const float kernelRoughness = min(2.0 * variance, 0.18);
        const float alpha = roughness * roughness;
        roughness = sqrt(sqrt(saturate(alpha * alpha + kernelRoughness)));
    }

    float3 lightDirection = normalize(-frame.lightDirectionIntensity.xyz);
    float3 halfway = normalize(viewDirection + lightDirection);
    float3 dielectricF0 = float3(0.04);
    // KHR_materials_specular: strength is in the alpha channel of
    // specularTexture, while specularColorTexture provides its RGB tint.
    // Do not reinterpret non-glTF/legacy named specular maps as this
    // extension; they have different authoring conventions.
#if PBR_HAS_EXTENSIONS
    if ((material.textureIndices.w & 64) != 0)
    {
        float specularStrength = saturate(material.extensionScalars.w);
        float3 specularColor = saturate(material.specularColor.rgb);
        if (material.extensionTextureIndices.y >= 0)
            specularStrength *= sampleMaterialTexture(material, material.extensionTextureIndices.y, 8, input).a;
        if (material.extensionTextureIndices.z >= 0)
            specularColor *= sampleMaterialTexture(material, material.extensionTextureIndices.z, 9, input).rgb;
        dielectricF0 *= specularStrength * specularColor;
    }
#endif
    float3 f0 = lerp(dielectricF0, albedo, metallic);
    float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), f0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    // Thin double-sided cards (leaves, ferns and grass-like glTF meshes) do
    // not have an opaque "back".  Their source winding is often arbitrary,
    // so use the incident-light magnitude on either face instead of letting
    // the camera-facing normal make the entire card black.
    float nDotL = foliageNDotL(doubleSided, normal, lightDirection);
    // View-dependent geometry term is shared by the sun and every local light.
    const float geometryView = geometrySchlickGGX(nDotV, roughness);
    float geometry = geometryView * geometrySchlickGGX(nDotL, roughness);
    // Split-sum terms: x scales F0 and y biases it, so x + y is the single-
    // scattering directional albedo Ess for F0 = 1.
    const float2 brdf = screenSample(brdfIntegrationLut, float2(nDotV, roughness)).rg;
    const float singleScatterAlbedo = max(brdf.x + brdf.y, 1.0e-4);
    // Single-scattering GGX loses the energy of light bouncing between
    // microfacets, so rough metals came out too dark. Fdez-Aguera's
    // compensation scales the specular lobe back to unit albedo.
    const float3 specularEnergyCompensation = 1.0 + f0 * (1.0 / singleScatterAlbedo - 1.0);
    float3 specular = distributionGGX(normal, halfway, roughness) * geometry * fresnel /
                      max(4.0 * nDotV * nDotL, 0.0001) * specularEnergyCompensation;
    const bool disableDirectSpecular = frame.pbrDebugView == PBR_DEBUG_NO_DIRECT_SPECULAR;
    if (disableDirectSpecular)
        specular = 0.0;
    // The diagnostic removes specular and restores its diffuse energy.  This
    // prevents view-dependent Fresnel from masquerading as a diffuse change.
    float3 diffuse = (1.0 - (disableDirectSpecular ? 0.0 : fresnel)) *
                     (1.0 - metallic) * albedo / PBR_PI;
    ShadowSampleDebug shadowDebug = {0.0, 0.0, 0u, 0u, uint2(0, 0), 0u, 0u};
#if FORWARD_RESOLVED_VSM
    float shadow = frame.shadowEnabled != 0
        ? 1.0 - screenSample(directionalVisibility, input.position.xy / frame.clusteredViewportNearFar.xy).r : 0.0;
#else
    float shadow = frame.shadowEnabled != 0
        ? sampleVirtualShadow(shadowMap, linearShadowMap, shadowDepthMap, input.worldPosition, geometricNormal, lightDirection,
                              frame, shadowPageTable,
                              frame.shadowQuality, uint2(input.position.xy),
                              frame.temporalSampleIndex, frame.temporalShadowJitter != 0, shadowDebug) : 0.0;
#endif
    if (frame.shadowDebugView != SHADOW_DEBUG_OFF)
    {
        ForwardFragmentOutput debugOutput;
#if FORWARD_RESOLVED_VSM
        // directional_visibility already evaluated the per-mode debug colour
        // (packShadowDebugColor). Sampling the virtual pages here instead
        // would inline the whole VSM filter into every lighting variant.
        // Point load: filtering would blend the packed bits.
        uint visibilityWidth, visibilityHeight;
        directionalVisibility.GetDimensions(visibilityWidth, visibilityHeight);
        const int2 visibilityPixel = int2(input.position.xy / frame.clusteredViewportNearFar.xy *
                                          float2(visibilityWidth, visibilityHeight));
        debugOutput.color = float4(unpackShadowDebugColor(
            directionalVisibility.Load(int3(visibilityPixel, 0)).r), baseColor.a);
#else
        debugOutput.color = float4(shadowDebugColor(frame.shadowDebugView, shadowDebug), baseColor.a);
#endif
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        debugOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return debugOutput;
    }
    if (frame.gtaoDebugView != 0u && frame.screenSpaceEffects != 0u)
    {
        ForwardFragmentOutput debugOutput;
        const float visibility = screenSample(gtaoVisibility, input.position.xy / frame.clusteredViewportNearFar.xy).r;
        debugOutput.color = float4(visibility.xxx, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
        float2 previousNdc = input.previousClip.xy / input.previousClip.w;
        debugOutput.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
        return debugOutput;
    }
    float finalVisibility = 1.0 - shadow;
    if (frame.clusterZScaleBiasEnvironmentMipRtContact.w != 0.0)
    {
        const float contactVisibility = screenSample(rtContactVisibility, input.position.xy / frame.clusteredViewportNearFar.xy).r;
        finalVisibility = min(finalVisibility, contactVisibility);
    }
    float3 direct = (diffuse + specular) * frame.lightDirectionIntensity.w *
                    frame.lightColor.rgb * nDotL * finalVisibility;
    // A directional-light-only view needs neither the logarithmic cluster
    // lookup nor its dependent SSBO read. Probe-capable variants still load
    // the range when probes are present, even without local lights.
    ClusterLightRange lightRange = {0u, 0u, 0u, 0u};
    // Without the light walk only the probe lookup reads the cluster range.
    bool needsCluster = FORWARD_LOCAL_LIGHT_LOOP != 0 && frame.localLightCount != 0u;
#if PBR_HAS_PROBES
    needsCluster = needsCluster || frame.reflectionProbeCount != 0u;
#endif
    if (needsCluster)
    {
        const uint2 grid = (uint2(frame.clusteredViewportNearFar.xy) + ClusterTilePixelSize - 1u) / ClusterTilePixelSize;
        const float viewDepth = max(-mul(float4(input.worldPosition, 1.0), frame.view).z, frame.clusteredViewportNearFar.z);
        const uint slice = min(uint(max(log2(viewDepth) * frame.clusterZScaleBiasEnvironmentMipRtContact.x + frame.clusterZScaleBiasEnvironmentMipRtContact.y, 0.0)), 23u);
        const uint2 tile = min(uint2(input.position.xy) / ClusterTilePixelSize, grid - 1u);
        const uint clusterIndex = (slice * grid.y + tile.y) * grid.x + tile.x;
        lightRange = clusterRanges[clusterIndex];
    }
    // Denoised ray-traced occlusion of up to four lights, keyed by light index
    // (local_light_shadow_denoise.slang); every other light is unshadowed.
#if FORWARD_LOCAL_LIGHT_LOOP
    const bool deferLocalLights = FORWARD_DEFERRED_LOCAL != 0 &&
                                  (frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_DEFERRED_LOCAL) != 0u &&
                                  frame.pbrDebugView == 0u;
#else
    const bool deferLocalLights = true;
#endif
#if FORWARD_LOCAL_LIGHT_LOOP
    uint4 localShadows = uint4(0u, 0u, 0u, 0u);
    if (needsCluster && !deferLocalLights && (frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_LOCAL_SHADOWS) != 0u)
    {
        // One texel per 2x2 quad: take the nearest whose surface depth
        // matches, else the best of the three toward this pixel.
        const int2 fullPixel = int2(input.position.xy);
        const int2 quad = fullPixel >> 1;
        uint2 quads;
        localShadowMask.GetDimensions(quads.x, quads.y);
        const float pixelDepth = -mul(float4(input.worldPosition, 1.0), frame.view).z;
        localShadows = localShadowMask.Load(int3(quad, 0));
        float depthError = abs(asfloat(localShadows.w & ~0xffu) - pixelDepth);
        const float tolerance = 0.02 * pixelDepth + 0.02;
        if (depthError > tolerance)
        {
            const int2 toward = select((fullPixel & 1) != 0, int2(1, 1), int2(-1, -1));
            const int2 candidates[3] = { int2(toward.x, 0), int2(0, toward.y), toward };
            [unroll] for (uint c = 0u; c < 3u; ++c)
            {
                const int2 neighbour = clamp(quad + candidates[c], int2(0, 0), int2(quads) - 1);
                const uint4 candidate = localShadowMask.Load(int3(neighbour, 0));
                const float candidateError = abs(asfloat(candidate.w & ~0xffu) - pixelDepth);
                if (candidateError < depthError)
                {
                    depthError = candidateError;
                    localShadows = candidate;
                }
            }
            // No quad around saw this surface: leave it unshadowed.
            if (depthError > 3.0 * tolerance)
                localShadows = uint4(0u, 0u, 0u, 0u);
        }
    }
    // Lights go in batches of four: their indices and positions load as
    // independent requests and the range test runs on all four before any
    // shading, so the walk is not one dependent index -> light load chain
    // per light (that chain, not GGX, dominated dense clusters).
    // Game View walks its depth-bounded tile list, far shorter than the
    // cluster's; Scene View and MSAA keep the cluster list.
    const bool tileLights = needsCluster && (frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_TILE_LIGHTS) != 0u;
    uint lightListOffset = lightRange.lightOffset;
    uint lightListCount = lightRange.lightCount;
    if (tileLights)
    {
        const uint tilesX = (uint(frame.clusteredViewportNearFar.x) + TileLightPixelSize - 1u) / TileLightPixelSize;
        const uint2 tile = uint2(input.position.xy) / TileLightPixelSize;
        const uint tileBase = (tile.y * tilesX + tile.x) * TileLightStride;
        lightListCount = tileLightIndices[tileBase];
        lightListOffset = tileBase + 1u;
    }
    if (deferLocalLights)
        lightListCount = 0u;
    for (uint batchStart = 0u; batchStart < lightListCount; batchStart += 4u)
    {
    uint4 batch;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        const uint entry = lightListOffset + batchStart + k;
        batch[k] = batchStart + k >= lightListCount ? 0xffffffffu
                   : tileLights                     ? tileLightIndices[entry]
                                                    : clusterLightIndices[entry];
    }
    uint inRange = 0u;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        if (batch[k] >= frame.localLightCount) continue;
        const float4 positionRange = localLights[batch[k]].positionRange;
        const float3 offset = positionRange.xyz - input.worldPosition;
        const float squared = dot(offset, offset);
        if (squared > 1e-8 && squared < positionRange.w * positionRange.w)
            inRange |= 1u << k;
    }
    [loop] while (inRange != 0u)
    {
        const uint slot = firstbitlow(inRange);
        inRange &= inRange - 1u;
        const uint lightIndex = slot == 0u ? batch.x : slot == 1u ? batch.y : slot == 2u ? batch.z : batch.w;
        float shadowVisibility = 1.0;
        if (localShadows.z != 0u)
        {
            [unroll] for (uint s = 0u; s < 4u; ++s)
            {
                const uint shadowLight = ((s < 2u ? localShadows.x : localShadows.y) >> ((s & 1u) * 16u)) & 0xffffu;
                if (shadowLight == lightIndex)
                    shadowVisibility = 1.0 - float((localShadows.z >> (s * 8u)) & 0xffu) / 255.0;
            }
        }
        if (shadowVisibility <= 0.0) continue;
        LocalLightData local = localLights[lightIndex];
        float3 toLight = local.positionRange.xyz - input.worldPosition;
        const float distanceSquared = dot(toLight, toLight);
        const float radius = local.positionRange.w;
        if (distanceSquared <= 1e-8 || distanceSquared >= radius * radius)
            continue;
        const float inverseDistance = rsqrt(distanceSquared);
        const float distanceToLight = distanceSquared * inverseDistance;
        float3 localDirection = toLight * inverseDistance;
        float rangeFactor = saturate(1.0 - distanceToLight / radius);
        float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
        attenuation *= localLightProfile(local, -localDirection);
        if (local.parameters.y > 1.5) // Spot
        {
            float cone = dot(normalize(-local.directionOuterCos.xyz), localDirection);
            attenuation *= smoothstep(local.directionOuterCos.w, local.parameters.x, cone);
        }
        float localNDotL = foliageNDotL(doubleSided, normal, localDirection);
        // The contribution scales with both; with many lights per cluster
        // most fall outside their cone or behind the surface, so skip GGX.
        // Lights near the end of their range add almost nothing (dozens of
        // them per cluster in dense scenes): skip those too.
        if (local.colorIntensity.w * attenuation * localNDotL * shadowVisibility < LocalLightCutoff)
            continue;
        float3 localHalfway = normalize(viewDirection + localDirection);
        float3 localFresnel = fresnelSchlick(max(dot(localHalfway, viewDirection), 0.0), f0);
        float localGeometry = geometryView * geometrySchlickGGX(localNDotL, roughness);
        float3 localSpecular = distributionGGX(normal, localHalfway, roughness) * localGeometry *
                               localFresnel / max(4.0 * nDotV * localNDotL, 0.0001) *
                               specularEnergyCompensation;
        if (disableDirectSpecular)
            localSpecular = 0.0;
        float3 localDiffuse = (1.0 - (disableDirectSpecular ? 0.0 : localFresnel)) *
                              (1.0 - metallic) * albedo / PBR_PI;
        direct += (localDiffuse + localSpecular) * local.colorIntensity.rgb *
                  local.colorIntensity.w * localNDotL * attenuation * shadowVisibility;
    }
    }
#endif // FORWARD_LOCAL_LIGHT_LOOP
    float ao = 1.0;
    if (material.auxiliaryTextureIndices.x >= 0)
        ao = lerp(1.0, sampleMaterialTexture(material, material.auxiliaryTextureIndices.x, 3, input).r,
                  clamp(material.roughnessAmbientOcclusion.y, 0.0, 1.0));
#ifdef ENGINE_SHADER_GRAPH
    ao = graphSurface.ao;
#endif
    float3 emissive = material.emissiveColorIntensity.rgb * material.emissiveColorIntensity.w;
    if (material.extensionTextureIndices.x >= 0)
    {
        emissive *= sampleMaterialTexture(material, material.extensionTextureIndices.x, 7, input).rgb;
    }
#ifdef ENGINE_SHADER_GRAPH
    emissive = graphSurface.emission;
#endif
    float3 fresnelIbl = fresnelSchlickRoughness(nDotV, f0, roughness);
    const bool disableSpecularIbl = frame.pbrDebugView == PBR_DEBUG_NO_SPECULAR_IBL;
    // Multiple-scattering IBL (Fdez-Aguera 2019). FssEss is the split-sum
    // single-scattering response; the energy it misses (1 - Ess) returns as
    // a multiply-scattered lobe with the average Fresnel, lit by the
    // irradiance. Diffuse receives only what both specular terms leave.
    const float3 singleScatterSpecular = fresnelIbl * brdf.x + brdf.y;
    const float multiScatterEnergy = 1.0 - singleScatterAlbedo;
    const float3 averageFresnel = f0 + (1.0 - f0) / 21.0;
    const float3 multiScatterSpecular = singleScatterSpecular * averageFresnel /
                                        (1.0 - multiScatterEnergy * averageFresnel) * multiScatterEnergy;
    float3 kS = disableSpecularIbl ? 0.0 : singleScatterSpecular + multiScatterSpecular;
    float3 kD = saturate(1.0 - kS) * (1.0 - metallic);
    float3 diffuseIrradiance = irradianceMap.SampleLevel(normal, 0.0).rgb;
#if FORWARD_DDGI
    if (ddgiVolume.gridMin.w != 0) {
        float probeWeight;
        // Under TAA each pixel evaluates one dithered cascade instead of
        // blending two across a cascade border.
        const bool dither = (frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_TAA) != 0u;
        const float cascadeDither = dither ? ddgiScreenDither(uint2(input.position.xy), frame.temporalSampleIndex) : -1.0;
        const float3 probeIrradiance = ddgiIrradiance(ddgiVolume, ddgiIrradianceAtlas, ddgiDistanceAtlas,
                                                      input.worldPosition, normal, viewDirection, cascadeDither,
                                                      probeWeight);
        diffuseIrradiance = lerp(diffuseIrradiance, probeIrradiance, probeWeight);
    }
#endif
    float3 indirectDiffuse = diffuseIrradiance * albedo * kD;
    float3 reflection = reflect(-viewDirection, normal);
    const uint prefilteredMipCount = max(uint(frame.clusterZScaleBiasEnvironmentMipRtContact.z), 1u);
    float3 prefiltered = prefilteredEnvironmentMap.SampleLevel(
        reflection, roughness * float(max(prefilteredMipCount, 1u) - 1u)).rgb;
    // The current forward pass provides the global sky.  Local-probe, SSR and
    // RT passes populate the preceding candidates as their resources become
    // available; this explicit resolver keeps the material-side priority
    // fixed as RT -> SSR -> probe -> sky.
    ReflectionCandidate skyReflection = {prefiltered, 1.0};
#if PBR_HAS_PROBES
    ReflectionCandidate localProbeReflection = sampleLocalProbes(frame, clusterLightIndices, input.worldPosition, reflection, roughness,
                                                                  prefilteredMipCount, lightRange);
#else
    ReflectionCandidate localProbeReflection = {0.0, 0.0};
#endif
    ReflectionCandidate screenSpaceReflection = {0.0, 0.0};
    ReflectionCandidate rayTracedReflection = {0.0, 0.0};
    prefiltered = resolveReflection(rayTracedReflection, screenSpaceReflection,
                                    localProbeReflection, skyReflection);
    float3 specularIbl = prefiltered * singleScatterSpecular;
    // Horizon specular occlusion (Filament-style): if normal mapping turns
    // the reflection vector below the geometric surface, no environment
    // radiance is physically visible along that direction.
    const float horizon = saturate(1.0 + dot(reflection, geometricNormal));
    specularIbl *= horizon * horizon;
    if (disableSpecularIbl)
        specularIbl = 0.0;
    // Scene View has an independent render target and no matching GTAO
    // texture yet. Sampling the Game View result there mixes two unrelated
    // screen spaces and produces hard viewport-aligned seams.
    const float dynamicAo = frame.screenSpaceEffects != 0u && frame.pbrDebugView != PBR_DEBUG_NO_GTAO
        ? screenSample(gtaoVisibility, input.position.xy /
            float2(frame.clusteredViewportNearFar.x, frame.clusteredViewportNearFar.y)).r
        : 1.0;
    // Material and screen-space visibility both describe indirect-light
    // accessibility, so neither is allowed to brighten the other.  The
    // GTAO visibility is used at full strength after its bilateral upsample.
    const float indirectVisibility = min(ao, dynamicAo);
    const float diffuseAO = indirectVisibility;
    // Lagarde's visibility-based specular-occlusion approximation.  It
    // suppresses sky/probe reflections in contact cavities without needing a
    // bent normal; rough surfaces converge to the diffuse visibility while
    // smooth ones retain the expected view-angle dependence.
    const float specularOcclusion = saturate(
        pow(saturate(nDotV) + indirectVisibility,
            exp2(-16.0 * roughness - 1.0)) - 1.0 + indirectVisibility);
    // SSGI is its own visibility-aware estimate of the occluded directions,
    // so GTAO must not darken it a second time; only baked material AO,
    // which describes sub-pixel cavities SSGI cannot see, still applies.
    float3 bouncedDiffuse = 0.0;
    if ((frame.screenSpaceEffects & SCREEN_SPACE_EFFECTS_SSGI) != 0u)
        bouncedDiffuse = screenSample(screenSpaceGi, input.position.xy /
            float2(frame.clusteredViewportNearFar.x, frame.clusteredViewportNearFar.y)).rgb * albedo * kD * ao;
    // The multiply-scattered lobe is broad, so it is lit like diffuse.
    const float3 multiScatterIbl = disableSpecularIbl ? 0.0 : diffuseIrradiance * multiScatterSpecular;
    float3 color = direct + indirectDiffuse * diffuseAO + multiScatterIbl * diffuseAO + bouncedDiffuse +
                   specularIbl * specularOcclusion + emissive;
    // Smooth metals under the sun now reach the GGX peak; keep the result
    // finite in the FP16 HDR target so TAA and bloom never see infinity.
    color = min(color, 65000.0);
    // Foliage has a cheap, stable back-lighting term.  The flag is deliberately
    // material data rather than an asset-name heuristic at runtime.
#if MATERIAL_VARIANT == 5
    // Leaves are thin and translucent: sunlight that enters one side leaves
    // the other tinted by the leaf. Looking towards the sun through foliage
    // it glows (forward scattering); leaves whose volume normal faces away
    // from the sun still receive a soft diffuse share. Every foliage
    // material gets it; a thickness mask, when present, modulates it.
    if ((material.textureIndices.w & 16) != 0) {
        float thickness = 1.0;
        if (material.auxiliaryTextureIndices.z >= 0)
            thickness = sampleMaterialTexture(material, material.auxiliaryTextureIndices.z, 5, input).r;
        const float strength = material.extensionScalars.y > 0.0 ? material.extensionScalars.y : FOLIAGE_TRANSMISSION;
        const float forwardScatter = pow(saturate(dot(-viewDirection, lightDirection)), 6.0);
        const float diffuseTransmission = saturate(-dot(normal, lightDirection));
        // The receiver's shadow describes its lit face; light still reaches a
        // shadowed leaf through the leaves in front of it, only weaker.
        const float transmittedVisibility = max(finalVisibility, FOLIAGE_SHADOWED_TRANSMISSION);
        color += albedo * frame.lightColor.rgb * frame.lightDirectionIntensity.w * transmittedVisibility *
                 (0.35 * diffuseTransmission + 1.5 * forwardScatter) * thickness * strength;
    }
#elif defined(ENGINE_SHADER_GRAPH)
    if ((material.textureIndices.w & 16) != 0 && material.auxiliaryTextureIndices.z >= 0) {
        float mask = sampleMaterialTexture(material, material.auxiliaryTextureIndices.z, 5, input).r;
        float backLight = saturate(dot(-normal, lightDirection));
        color += albedo * frame.lightColor.rgb * frame.lightDirectionIntensity.w *
                 backLight * mask * material.extensionScalars.y;
    }
#endif

    // Material channel visualisations (Rendering > Debug views > PBR).
    if (frame.pbrDebugView == PBR_DEBUG_ALBEDO)
        color = albedo;
    else if (frame.pbrDebugView == PBR_DEBUG_NORMALS)
        color = normalize(normal) * 0.5 + 0.5;
    else if (frame.pbrDebugView == PBR_DEBUG_ROUGHNESS)
        color = roughness;
    else if (frame.pbrDebugView == PBR_DEBUG_METALLIC)
        color = metallic;
    else if (frame.pbrDebugView == PBR_DEBUG_AO)
        color = min(ao, dynamicAo);

    ForwardFragmentOutput output;
    output.color = float4(color, baseColor.a);
#if FORWARD_OUTPUT_VELOCITY
    float2 previousNdc = input.previousClip.xy / input.previousClip.w;
    output.velocity = (input.currentNdc - previousNdc) * 0.5;
#endif
#if FORWARD_DEFERRED_LOCAL
    uint nDotLMode = doubleSided ? LocalNDotLTwoSided : LocalNDotLOneSided;
#if MATERIAL_VARIANT == 5
    if (doubleSided)
        nDotLMode = LocalNDotLWrapped;
#endif
    output.localLightData = deferLocalLights
                                ? packDeferredLocalSurface(normal, (1.0 - metallic) * albedo / PBR_PI, roughness, f0,
                                                           1.0 / singleScatterAlbedo - 1.0, nDotLMode)
                                : uint4(0u, 0u, 0u, 0u);
#endif
    return output;
}

#if !FORWARD_VISIBILITY_RESOLVE
[shader("fragment")]
ForwardFragmentOutput fragmentMain(ForwardFragmentInput input)
{
    return shadeForwardFragment(input);
}
#else
// Visibility buffer of the depth prepass (DEPTH_VISIBILITY_ID): the
// meshletDrawList index above the triangle's index within its meshlet.
[[vk::binding(0, 2)]] Texture2D<uint> visibilityIds;
[[vk::binding(1, 2), vk::image_format("rgba16f")]] RWTexture2D<float4> hdrOutput;
[[vk::binding(2, 2), vk::image_format("rgba32ui")]] RWTexture2D<uint4> localLightSurfaceOutput;

// Perspective-correct barycentrics of a pixel centre and their screen-space
// derivatives (Schied and Dachsbacher; The Forge's visibility buffer). The
// clip positions are those the prepass rasterized. Vulkan's viewport maps
// NDC y to pixel rows in the same direction, so no axis flips.
struct VisibilityBarycentrics
{
    float3 lambda;
    float3 ddx;
    float3 ddy;
};

VisibilityBarycentrics visibilityBarycentrics(float4 clip0, float4 clip1, float4 clip2, float2 pixelNdc,
                                              float2 viewport)
{
    VisibilityBarycentrics result;
    const float3 invW = 1.0 / float3(clip0.w, clip1.w, clip2.w);
    const float2 ndc0 = clip0.xy * invW.x;
    const float2 ndc1 = clip1.xy * invW.y;
    const float2 ndc2 = clip2.xy * invW.z;
    const float invDet = 1.0 / determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1));
    float3 dx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet * invW;
    float3 dy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet * invW;
    float dxSum = dx.x + dx.y + dx.z;
    float dySum = dy.x + dy.y + dy.z;
    const float2 delta = pixelNdc - ndc0;
    const float interpInvW = invW.x + delta.x * dxSum + delta.y * dySum;
    const float interpW = 1.0 / interpInvW;
    result.lambda = interpW * (float3(invW.x, 0.0, 0.0) + delta.x * dx + delta.y * dy);
    // One pixel in NDC.
    const float2 pixelSize = 2.0 / viewport;
    dx *= pixelSize.x;
    dy *= pixelSize.y;
    dxSum *= pixelSize.x;
    dySum *= pixelSize.y;
    const float interpWdx = 1.0 / (interpInvW + dxSum);
    const float interpWdy = 1.0 / (interpInvW + dySum);
    result.ddx = interpWdx * (result.lambda * interpInvW + dx) - result.lambda;
    result.ddy = interpWdy * (result.lambda * interpInvW + dy) - result.lambda;
    return result;
}

ForwardVertexOutput visibilityVertex(MeshletRecord meshlet, uint corner, uint visibleInstance)
{
    const MeshletPackedVertex packed = meshletVertexHeap[meshletVertexRefs[meshlet.range.x + corner]];
    ForwardVertexInput input;
    input.position = packed.position;
    input.color = unpackMeshletColor(packed.color);
    input.texCoord = unpackMeshletHalf2(packed.texCoord);
    input.texCoord1 = unpackMeshletHalf2(packed.texCoord1);
    input.tangentFrame = packed.tangentFrame;
    input.materialIndex = packed.materialIndex;
    return shadeForwardVertex(input, visibleInstance);
}

[numthreads(8, 8, 1)]
[shader("compute")]
void visibilityResolveMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrOutput.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;
    const uint visibility = visibilityIds.Load(int3(id.xy, 0));
    if (visibility == VisibilityNone)
        return;
    const MeshletDrawEntry entry = meshletDrawList[visibility >> VisibilityTriangleBits];
    const MeshletRecord meshlet = meshletRecords[entry.meshlet];
    const uint3 corners = unpackMeshletTriangle(
        meshletTriangleRefs[meshlet.range.z + (visibility & ((1u << VisibilityTriangleBits) - 1u))]);
    const ForwardVertexOutput v0 = visibilityVertex(meshlet, corners.x, entry.visibleInstance);
    const ForwardVertexOutput v1 = visibilityVertex(meshlet, corners.y, entry.visibleInstance);
    const ForwardVertexOutput v2 = visibilityVertex(meshlet, corners.z, entry.visibleInstance);

    const float2 viewport = float2(width, height);
    const float2 pixelNdc = (float2(id.xy) + 0.5) / viewport * 2.0 - 1.0;
    const VisibilityBarycentrics b = visibilityBarycentrics(v0.position, v1.position, v2.position, pixelNdc, viewport);
#define VISIBILITY_INTERPOLATE(field, weights) \
    ((weights).x * v0.field + (weights).y * v1.field + (weights).z * v2.field)

    ForwardFragmentInput input;
    input.position = float4(float2(id.xy) + 0.5, 0.0, 1.0);
    input.color = VISIBILITY_INTERPOLATE(color, b.lambda);
    input.worldPosition = VISIBILITY_INTERPOLATE(worldPosition, b.lambda);
    const float3 normal = VISIBILITY_INTERPOLATE(normal, b.lambda);
    input.normal = normal;
    input.materialIndex = v0.materialIndex;
    input.texCoord = VISIBILITY_INTERPOLATE(texCoord, b.lambda);
    input.tangent = VISIBILITY_INTERPOLATE(tangent, b.lambda);
    input.texCoord1 = VISIBILITY_INTERPOLATE(texCoord1, b.lambda);
#if MATERIAL_VARIANT == 5
    input.instancePivot = v0.instancePivot;
#endif
    // Front faces have a negative signed area (meshletTriangleCulled).
    input.isFrontFace = determinant(float3x3(v0.position.xyw, v1.position.xyw, v2.position.xyw)) < 0.0;
    input.texCoordDx = VISIBILITY_INTERPOLATE(texCoord, b.ddx);
    input.texCoordDy = VISIBILITY_INTERPOLATE(texCoord, b.ddy);
    input.texCoord1Dx = VISIBILITY_INTERPOLATE(texCoord1, b.ddx);
    input.texCoord1Dy = VISIBILITY_INTERPOLATE(texCoord1, b.ddy);
    // Derivative of normalize(n): the change of n without its radial part.
    const float inverseLength = rsqrt(max(dot(normal, normal), 1e-12));
    const float3 unitNormal = normal * inverseLength;
    const float3 normalDx = VISIBILITY_INTERPOLATE(normal, b.ddx);
    const float3 normalDy = VISIBILITY_INTERPOLATE(normal, b.ddy);
    input.normalDx = (normalDx - unitNormal * dot(unitNormal, normalDx)) * inverseLength;
    input.normalDy = (normalDy - unitNormal * dot(unitNormal, normalDy)) * inverseLength;
#undef VISIBILITY_INTERPOLATE

    const ForwardFragmentOutput output = shadeForwardFragment(input);
    hdrOutput[id.xy] = output.color;
#if FORWARD_DEFERRED_LOCAL
    localLightSurfaceOutput[id.xy] = output.localLightData;
#endif
}
#endif
