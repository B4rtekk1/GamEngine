struct LocalLightData
{
    float4 positionRange;
    float4 directionOuterCos;
    float4 colorIntensity;
    float4 parameters;
};

struct RendererFrameData
{
    float4x4 view;
    float4x4 projection;
    float4x4 previousView;
    float4x4 previousProjection;
    float4x4 shadowClipMatrices[7];
    float4 cameraPosition;
    float4 lightDirectionIntensity;
    float4 lightColor;
    float4 windDirectionStrength;
    float4 windSourcePositionRange;
    float4 windGustFrequencyTime;
    uint shadowEnabled;
    uint shadowQuality;
    uint shadowDebugView;
    uint materialSlots;
    uint selectedInstance;
    uint localLightCount;
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
    int4 extensionTextureIndices;
    float4 emissiveColorIntensity;
    int4 textureCoordinateSets0;
    int4 textureCoordinateSets1;
    int4 textureCoordinateSets2;
    float4 textureTransforms[9];
    float4 textureTransformRotations[3];
};

float2 materialTextureUv(MaterialData material, uint slot, float2 uv0, float2 uv1)
{
    int texCoord = slot < 4 ? material.textureCoordinateSets0[slot] :
                   slot < 8 ? material.textureCoordinateSets1[slot - 4] :
                              material.textureCoordinateSets2[slot - 8];
    float2 uv = texCoord == 1 ? uv1 : uv0;
    float4 transform = material.textureTransforms[slot];
    float sine; float cosine;
    sincos(material.textureTransformRotations[slot / 4][slot % 4], sine, cosine);
    float2 scaled = uv * transform.zw;
    float2 rotated = float2(cosine * scaled.x - sine * scaled.y,
                            sine * scaled.x + cosine * scaled.y);
    return rotated + transform.xy;
}

// Mirrors Engine::RendererInstanceData.  The renderer binds this table as an
// SSBO so instance fetch is driven by the indirect command's firstInstance,
// not a per-instance vertex-input binding.
struct RendererInstanceData
{
    float4 positionMaterial;
    float4 rotation;
    float4 scaleBase;
    float4 previousPosition;
    float4 previousRotation;
    float4 previousScale;
};
