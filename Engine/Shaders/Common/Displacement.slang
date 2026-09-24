import Common.SceneData;

[[vk::binding(13, 0)]] Sampler2D materialTextures[];

bool materialHasDisplacement(MaterialData material)
{
    return material.auxiliaryTextureIndices.w >= 0 &&
           (abs(material.displacementParams.x) > 1e-6 ||
            abs(material.displacementParams.y) > 1e-6);
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
