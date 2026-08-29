static const float PBR_PI = 3.14159265359;

float distributionGGX(float3 normal, float3 halfway, float roughness)
{
    float a2 = roughness * roughness * roughness * roughness;
    float nDotH2 = max(dot(normal, halfway), 0.0);
    nDotH2 *= nDotH2;
    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    return a2 / max(PBR_PI * denominator * denominator, 0.0001);
}

float geometrySchlickGGX(float nDotV, float roughness)
{
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float3 normal, float3 viewDirection,
                    float3 lightDirection, float roughness)
{
    return geometrySchlickGGX(max(dot(normal, viewDirection), 0.0), roughness) *
           geometrySchlickGGX(max(dot(normal, lightDirection), 0.0), roughness);
}

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
}
