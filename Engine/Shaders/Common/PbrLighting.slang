static const float PBR_PI = 3.14159265359;

// Analytic lights are treated as having a small angular size: below this
// perceptual roughness the GGX peak (1 / (pi * a^2)) would exceed what an
// FP16 HDR target can hold once multiplied by the sun's intensity.
static const float PBR_MIN_ANALYTIC_ROUGHNESS = 0.08;

float distributionGGX(float3 normal, float3 halfway, float roughness)
{
    roughness = max(roughness, PBR_MIN_ANALYTIC_ROUGHNESS);
    float a2 = roughness * roughness * roughness * roughness;
    float nDotH2 = max(dot(normal, halfway), 0.0);
    nDotH2 *= nDotH2;
    float denominator = nDotH2 * (a2 - 1.0) + 1.0;
    // denominator >= a2 > 0. Clamping pi * d^2 to 1e-4 instead capped the
    // peak at a2 * 1e4, which dimmed the highlight of every surface smoother
    // than ~0.35 roughness by up to three orders of magnitude.
    return a2 / (PBR_PI * denominator * denominator);
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

// (1 - cos)^5 by multiplication: pow() compiles to log2/exp2 on the
// special-function units, which per-light shading calls dozens of times.
float schlickWeight(float cosTheta)
{
    const float x = saturate(1.0 - cosTheta);
    const float x2 = x * x;
    return x2 * x2 * x;
}

float3 fresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * schlickWeight(cosTheta);
}

float3 fresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    return f0 + (max(float3(1.0 - roughness), f0) - f0) * schlickWeight(cosTheta);
}
