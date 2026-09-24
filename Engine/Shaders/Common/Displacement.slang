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
    result.position = displaceMaterialPosition(material, localPosition, localNormal, uv0, uv1);
    // Vertex shaders cannot recover the UV-to-position metric. Shading normals
    // are reconstructed in the fragment shader using surface derivatives.
    result.normal = normalize(localNormal);
    result.tangent = localTangent;
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

// Fragment-only: screen derivatives recover dP/du and dP/dv for the selected
// UV set after its material transform, including UV1 and rotated UVs.
float3 displacedMaterialWorldNormal(MaterialData material, float3 baseWorldPosition,
                                    float3 baseWorldNormal, float3 worldDisplacementDirection,
                                    float2 uv0, float2 uv1)
{
    const float3 normal = normalize(baseWorldNormal);
    if (!materialHasDisplacement(material)) return normal;

    const float2 uv = materialTextureUv(material, 6, uv0, uv1);
    const float2 uvDx = ddx(uv);
    const float2 uvDy = ddy(uv);
    const float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    const float uvAreaSquared = dot(uvDx, uvDx) * dot(uvDy, uvDy);
    if (determinant * determinant < uvAreaSquared * 1e-8 ||
        uvAreaSquared < 1e-24) return normal;

    const float3 positionDx = ddx(baseWorldPosition);
    const float3 positionDy = ddy(baseWorldPosition);
    const float3 directionDx = ddx(worldDisplacementDirection);
    const float3 directionDy = ddy(worldDisplacementDirection);
    const float3 positionDu = (positionDx * uvDy.y - positionDy * uvDx.y) / determinant;
    const float3 positionDv = (positionDy * uvDx.x - positionDx * uvDy.x) / determinant;
    const float3 directionDu = (directionDx * uvDy.y - directionDy * uvDx.y) / determinant;
    const float3 directionDv = (directionDy * uvDx.x - directionDx * uvDy.x) / determinant;

    const int displacementTexture = material.auxiliaryTextureIndices.w;
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
    const float height = materialTextures[NonUniformResourceIndex(displacementTexture)].SampleLevel(
        uv, 0.0).r;
    const float amount = height * material.displacementParams.x + material.displacementParams.y;
    const float heightDu = (hR - hL) * material.displacementParams.x / max(2.0 * texel.x, 1e-6);
    const float heightDv = (hU - hD) * material.displacementParams.x / max(2.0 * texel.y, 1e-6);
    const float3 displacedDu = positionDu + worldDisplacementDirection * heightDu + directionDu * amount;
    const float3 displacedDv = positionDv + worldDisplacementDirection * heightDv + directionDv * amount;
    const float3 surfaceNormal = cross(displacedDu, displacedDv);
    if (dot(surfaceNormal, surfaceNormal) < 1e-12) return normal;
    return normalize(surfaceNormal) * (dot(surfaceNormal, normal) < 0.0 ? -1.0 : 1.0);
}
