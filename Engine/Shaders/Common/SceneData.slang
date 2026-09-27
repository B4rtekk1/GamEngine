struct LocalLightData
{
    float4 positionRange;
    float4 directionOuterCos;
    float4 colorIntensity;
    float4 parameters;
};

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
    // 3=no GTAO, 4=geometric-normal direct diffuse. Kept in the former
    // padding slot to preserve UBO layout.
    uint pbrDebugView;
    // xy viewport pixels, z near plane, w far plane.
    float4 clusteredViewportNearFar;
    float4 clusterZScaleBiasEnvironmentMipRtContact;
    LocalLightData localLights[32];
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
