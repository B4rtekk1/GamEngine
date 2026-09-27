// Convert the forward depth prepass's octahedral view normals to the world
// normals and linear depth textures consumed by FidelityFX Brixelizer GI.
[[vk::binding(0, 0)]] Sampler2D depthInput;
[[vk::binding(1, 0)]] Sampler2D octNormalInput;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> worldNormalOutput;
[[vk::binding(3, 0), vk::image_format("r32f")]] RWTexture2D<float> depthOutput;
[[vk::push_constant]] ConstantBuffer<float4x4> inverseView;

float3 decodeOct(float2 encoded)
{
    float3 normal = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (normal.z < 0.0)
        normal.xy = (1.0 - abs(normal.yx)) * select(normal.xy >= 0.0, 1.0, -1.0);
    return normalize(normal);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    depthOutput.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    const float depth = depthInput.Load(int3(id.xy, 0)).r;
    const float3 viewNormal = decodeOct(octNormalInput.Load(int3(id.xy, 0)).xy);
    const float3 worldNormal = normalize(mul(float4(viewNormal, 0.0), inverseView).xyz);
    // GI specular is disabled until the forward prepass exports material roughness.
    worldNormalOutput[id.xy] = float4(worldNormal * 0.5 + 0.5, 1.0);
    depthOutput[id.xy] = depth;
}
