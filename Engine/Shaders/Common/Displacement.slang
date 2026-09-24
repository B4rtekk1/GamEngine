import Common.SceneData;

[[vk::binding(13, 0)]] Sampler2D materialTextures[];

bool materialHasDisplacement(MaterialData material)
{
    return material.auxiliaryTextureIndices.w >= 0 &&
           (abs(material.displacementParams.x) > 1e-6 ||
            abs(material.displacementParams.y) > 1e-6);
}

struct DisplacedVertex
{
    float3 position;
    float3 normal;
    float4 tangent;
};

DisplacedVertex displaceMaterialVertex(MaterialData material, float3 localPosition,
                                      float3 localNormal, float4 localTangent,
                                      float2 uv0, float2 uv1)
{
    DisplacedVertex result;
    result.position = localPosition;
    result.normal = normalize(localNormal);
    result.tangent = localTangent;
    if (!materialHasDisplacement(material)) return result;

    const int displacementTexture = material.auxiliaryTextureIndices.w;
    const float2 uv = materialTextureUv(material, 6, uv0, uv1);
    const float height = materialTextures[
        NonUniformResourceIndex(displacementTexture)].SampleLevel(uv, 0.0).r;
    const float amount = height * material.displacementParams.x +
                         material.displacementParams.y;
    result.position += result.normal * amount;

    uint width, heightPixels, mipCount;
    materialTextures[NonUniformResourceIndex(displacementTexture)].GetDimensions(
        0, width, heightPixels, mipCount);
    const float2 texel = 1.0 / float2(max(width, 1u), max(heightPixels, 1u));
    const float hL = materialTextures[NonUniformResourceIndex(displacementTexture)].SampleLevel(
        uv - float2(texel.x, 0.0), 0.0).r;
    const float hR = materialTextures[NonUniformResourceIndex(displacementTexture)].SampleLevel(
        uv + float2(texel.x, 0.0), 0.0).r;
    const float hD = materialTextures[NonUniformResourceIndex(displacementTexture)].SampleLevel(
        uv - float2(0.0, texel.y), 0.0).r;
    const float hU = materialTextures[NonUniformResourceIndex(displacementTexture)].SampleLevel(
        uv + float2(0.0, texel.y), 0.0).r;
    const float dhdu = (hR - hL) * material.displacementParams.x;
    const float dhdv = (hU - hD) * material.displacementParams.x;

    const float3 tangent = normalize(localTangent.xyz -
                                     result.normal * dot(localTangent.xyz, result.normal));
    const float3 bitangent = cross(result.normal, tangent) * localTangent.w;
    result.normal = normalize(result.normal - tangent * dhdu - bitangent * dhdv);
    result.tangent = float4(normalize(tangent - result.normal * dot(tangent, result.normal)),
                            localTangent.w);
    return result;
}

float3 displaceMaterialPosition(MaterialData material, float3 localPosition,
                                float3 localNormal, float2 uv0, float2 uv1)
{
    if (!materialHasDisplacement(material)) return localPosition;

    const int displacementTexture = material.auxiliaryTextureIndices.w;
    const float2 uv = materialTextureUv(material, 6, uv0, uv1);
    const float height = materialTextures[
        NonUniformResourceIndex(displacementTexture)].SampleLevel(uv, 0.0).r;
    const float amount = height * material.displacementParams.x +
                         material.displacementParams.y;
    return localPosition + normalize(localNormal) * amount;
}
