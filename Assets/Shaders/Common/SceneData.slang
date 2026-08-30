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
    float4x4 shadowClipMatrices[4];
    float4 cameraPosition;
    float4 lightDirectionIntensity;
    float4 lightColor;
    uint shadowEnabled;
    uint materialSlots;
    uint selectedInstance;
    uint materialSlotsPadding;
    uint localLightCount;
    LocalLightData localLights[32];
};

struct MaterialData
{
    float4 baseColorMetallic;
    float4 roughnessAmbientOcclusion;
    int4 textureIndices;
    int4 terrainLayerTextures;
};
