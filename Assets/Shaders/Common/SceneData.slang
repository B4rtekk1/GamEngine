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
};

struct MaterialData
{
    float4 baseColorMetallic;
    float4 roughnessAmbientOcclusion;
    int4 textureIndices;
};
