struct LocalLightData
{
    float4 positionRange;
    // xyz: the light's forward axis (spot beam, profile axis), w: outer cone cosine.
    float4 directionOuterCos;
    float4 colorIntensity;
    // x inner cone cosine, y LightType, z casts shadows, w 1 with a profile.
    float4 parameters;
    // Photometric profile: 32 unorm8 intensities, peak 1, sampled at equal
    // steps of the angle from the forward axis (0 to 180 degrees). See
    // Engine/Renderer/Lighting/LightProfiles.h.
    uint4 profile0;
    uint4 profile1;
};

static const uint LocalLightProfileSamples = 32u;

float localLightProfileSample(LocalLightData light, uint index)
{
    const uint word = index < 16u ? light.profile0[index >> 2u] : light.profile1[(index - 16u) >> 2u];
    return float((word >> ((index & 3u) * 8u)) & 0xffu) * (1.0 / 255.0);
}

// Relative intensity a profiled lamp sends along `fromLight` (unit vector
// from the light to the lit point); 1 for lights without a profile.
float localLightProfile(LocalLightData light, float3 fromLight)
{
    if (light.parameters.w < 0.5)
        return 1.0;
    const float cosine = clamp(dot(fromLight, light.directionOuterCos.xyz), -1.0, 1.0);
    const float position = acos(cosine) * (float(LocalLightProfileSamples - 1u) / 3.14159265359);
    const uint index = min(uint(position), LocalLightProfileSamples - 2u);
    return lerp(localLightProfileSample(light, index), localLightProfileSample(light, index + 1u),
                saturate(position - float(index)));
}

// Screen tile edge of one light cluster; must match ClusterTileSize on the CPU.
static const uint ClusterTilePixelSize = 64u;
// Depth-bounded Forward+ light lists (Culling/tile_light_culling.slang):
// TileLightStride entries per tile, the count then up to MaxTileLights
// light indices. TileLightPixelSize / TileLightStride in renderer_types.h.
static const uint TileLightPixelSize = 16u;
static const uint TileLightStride = 64u;
static const uint MaxTileLights = TileLightStride - 1u;
// Squared radius of the sphere a local light stands for: inverse-square
// falloff is clamped there, as a real lamp globe or lantern spreads its
// output over its surface. A point source would light a wall a few
// centimetres away hundreds of times brighter than the room.
static const float LocalLightSourceRadiusSquared = 0.25 * 0.25;
// Irradiance below which a local light's GGX is not evaluated (scene-referred
// units; a lamp pool is ~1-10, the faintest visible night fill ~0.01).
static const float LocalLightCutoff = 5e-4;

struct ClusterLightRange
{
    uint lightOffset;
    uint lightCount;
    uint probeOffset;
    uint probeCount;
};

struct ShadowClipConstants
{
    float worldTexelSize;
    float depthScale;
    float2 padding;
};

struct RendererFrameData
{
    float4x4 view;
    float4x4 projection;
    float4x4 inverseProjection;
    float4x4 inverseView;
    float4x4 previousView;
    float4x4 previousProjection;
    float4x4 shadowClipMatrices[7];
    ShadowClipConstants shadowClipConstants[7];
    float4 cameraPosition;
    float4 lightDirectionIntensity;
    float4 lightColor;
    float4 windDirectionStrength;
    float4 windSourcePositionRange;
    float4 windGustFrequencyTime;
    uint shadowEnabled;
    uint shadowQuality;
    uint shadowDebugView;
    uint gtaoDebugView;
    uint materialSlots;
    uint selectedInstance;
    uint localLightCount;
    // Number of valid records in the reflection-probe SSBO.  The buffer has
    // fixed storage capacity, but entries beyond this count are not populated.
    uint reflectionProbeCount;
    // Zero for views without matching full-screen inputs (currently Scene
    // View); prevents sampling Game View's GTAO/depth/colour textures.
    uint screenSpaceEffects;
    uint temporalShadowJitter;
    uint temporalSampleIndex;
    // PbrDebugView: 0=final, 1=no specular IBL, 2=no direct specular,
    // 3=no GTAO, 4=geometric-normal direct diffuse, 5=albedo, 6=normals,
    // 7=roughness, 8=metallic, 9=ambient occlusion. Kept in the former
    // padding slot to preserve UBO layout.
    uint pbrDebugView;
    // xy viewport pixels, z near plane, w far plane.
    float4 clusteredViewportNearFar;
    float4 clusterZScaleBiasEnvironmentMipRtContact;
    LocalLightData localLights[32];
    // x: wetness, y: puddle coverage, z: 1 / puddle scale (m), w: rain ripple strength.
    float4 wetnessParams;
    // x: SSR intensity, y: max roughness, z: Hi-Z steps, w: hit thickness (m).
    float4 ssrParams;
    // x: rain intensity (0..1), y: ripple frequency, z: ripple size (m), w: unused.
    float4 rainParams;
};

struct MaterialData
{
    float4 baseColorMetallic;
    float4 roughnessAmbientOcclusion;
    int4 textureIndices;
    int4 terrainLayerTextures;
    int4 auxiliaryTextureIndices;
    float4 extensionScalars;
    // x = scale, y = offset
    float4 displacementParams;
    int4 extensionTextureIndices;
    float4 specularColor;
    float4 emissiveColorIntensity;
    int4 textureCoordinateSets0;
    int4 textureCoordinateSets1;
    int4 textureCoordinateSets2;
    float4 textureTransforms[10];
    float4 textureTransformRows1[6];
    float4 waterShallowColorRoughness;
    float4 waterDeepColorIor;
    float4 waterAbsorptionRefraction;
    float4 waterScatteringMaxDepth;
    // x foam intensity, y threshold, z normal strength, w feature flags.
    float4 waterFoam;
    // normal map, foam map, flow map, reserved.
    int4 waterTextureIndices;
    float4 waterWaves[8];
    float4 waterWaveMotion[8];
    uint4 waterWaveCount;
    // Glass block, see Engine::GPUMaterialData. x transmission, y IOR,
    // z thickness, w dirt intensity.
    float4 glassParams;
    // RGB attenuation colour, w attenuation distance (0: no absorption).
    float4 glassAttenuation;
    // Transmission, thickness, dirt textures.
    int4 glassTextureIndices;
    int4 glassTextureCoordinateSets;
    float4 glassTextureTransforms[3];
    float4 glassTextureRows1[3];
};

float2 materialTextureUv(MaterialData material, uint slot, float2 uv0, float2 uv1)
{
    int encodedTexCoord = slot < 4 ? material.textureCoordinateSets0[slot] :
                   slot < 8 ? material.textureCoordinateSets1[slot - 4] :
                              material.textureCoordinateSets2[slot - 8];
    float2 uv = (encodedTexCoord & 1) != 0 ? uv1 : uv0;
    if ((encodedTexCoord & 2) != 0) return uv;
    float4 transform = material.textureTransforms[slot];
    float2 row1 = float2(material.textureTransformRows1[(slot / 4) * 2][slot % 4],
                         material.textureTransformRows1[(slot / 4) * 2 + 1][slot % 4]);
    return float2(dot(uv, transform.xy), dot(uv, row1)) + transform.zw;
}

// Mirrors Engine::RendererInstanceData and RendererPreviousTransformData.
// The renderer binds these tables as SSBOs so instance fetch is driven by the
// indirect command's firstInstance, not a vertex-input binding.
struct RendererInstanceData
{
    float4 positionMaterial;
    float4 rotation;
    float4 scaleBase;
};

struct RendererPreviousTransformData
{
    float4 previousPosition;
    float4 previousRotation;
    float4 previousScale;
};
