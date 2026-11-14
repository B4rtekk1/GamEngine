// Shading inputs that forward shading leaves for the deferred local light
// pass (Lighting/deferred_local_lights.slang), one RGBA32_UINT texel:
//   x: octahedral world normal, 2 x 16-bit unorm
//   y: diffuse colour (1 - metallic) * albedo / pi, red | green as half floats
//   z: blue as a half float | roughness (8-bit) << 16 | energy term (8-bit) << 24
//   w: sqrt(F0) as 3 x 9-bit unorm | N.L mode << 27 | valid << 31
// The energy term is 1 / Ess - 1 (scaled by 1/2), so the specular energy
// compensation 1 + F0 * (1 / Ess - 1) can be rebuilt. N.L mode: 0 one-sided,
// 1 two-sided (|N.L|), 2 wrapped foliage. A zero texel (sky, unlit, shader
// graph or grass surfaces, which light themselves) adds nothing.
import Common.SceneData;
import Common.PbrLighting;

static const uint LocalNDotLOneSided = 0u;
static const uint LocalNDotLTwoSided = 1u;
static const uint LocalNDotLWrapped = 2u;
// MATERIAL_VARIANT 5's diffuse wrap (forward_pbr.slang).
static const float LocalFoliageDiffuseWrap = 0.4;

struct DeferredLocalSurface
{
    float3 normal;
    float3 diffuseColor;
    float roughness;
    float3 f0;
    float energyTerm;
    uint nDotLMode;
};

float2 octEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0.0)
        e = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return e;
}

float3 octDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

uint4 packDeferredLocalSurface(float3 normal, float3 diffuseColor, float roughness, float3 f0, float energyTerm,
                               uint nDotLMode)
{
    const uint2 oct = uint2(saturate(octEncode(normal) * 0.5 + 0.5) * 65535.0 + 0.5);
    const uint3 f0Bits = uint3(saturate(sqrt(f0)) * 511.0 + 0.5);
    uint4 packed;
    packed.x = oct.x | (oct.y << 16u);
    packed.y = f32tof16(diffuseColor.r) | (f32tof16(diffuseColor.g) << 16u);
    packed.z = f32tof16(diffuseColor.b) | (uint(saturate(roughness) * 255.0 + 0.5) << 16u) |
               (uint(saturate(energyTerm * 0.5) * 255.0 + 0.5) << 24u);
    packed.w = f0Bits.x | (f0Bits.y << 9u) | (f0Bits.z << 18u) | (nDotLMode << 27u) | 0x80000000u;
    return packed;
}

bool unpackDeferredLocalSurface(uint4 packed, out DeferredLocalSurface surface)
{
    surface = (DeferredLocalSurface)0;
    if ((packed.w & 0x80000000u) == 0u)
        return false;
    const float2 oct = float2(packed.x & 0xffffu, packed.x >> 16u) / 65535.0 * 2.0 - 1.0;
    surface.normal = octDecode(oct);
    surface.diffuseColor = float3(f16tof32(packed.y & 0xffffu), f16tof32(packed.y >> 16u), f16tof32(packed.z & 0xffffu));
    surface.roughness = float((packed.z >> 16u) & 0xffu) / 255.0;
    surface.energyTerm = float(packed.z >> 24u) / 255.0 * 2.0;
    const float3 sqrtF0 = float3(packed.w & 0x1ffu, (packed.w >> 9u) & 0x1ffu, (packed.w >> 18u) & 0x1ffu) / 511.0;
    surface.f0 = sqrtF0 * sqrtF0;
    surface.nDotLMode = (packed.w >> 27u) & 3u;
    return true;
}

float localNDotL(uint mode, float3 normal, float3 lightDirection)
{
    const float cosine = dot(normal, lightDirection);
    if (mode == LocalNDotLWrapped)
        return saturate((cosine + LocalFoliageDiffuseWrap) / (1.0 + LocalFoliageDiffuseWrap));
    return mode == LocalNDotLTwoSided ? abs(cosine) : max(cosine, 0.0);
}

// One local light's unshadowed contribution, split as forward shading
// computes it: `diffuse` still to be multiplied by the surface's diffuse
// colour (so the stochastic path can denoise it without the albedo), and
// the specular radiance.
struct LocalLightContribution
{
    float3 diffuse;
    float3 specular;
};

bool evaluateLocalLight(DeferredLocalSurface surface, float3 worldPosition, float3 viewDirection, float nDotV,
                        float geometryView, float3 energyCompensation, LocalLightData light,
                        out LocalLightContribution contribution)
{
    contribution = (LocalLightContribution)0;
    const float3 toLight = light.positionRange.xyz - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    const float radius = light.positionRange.w;
    if (distanceSquared <= 1e-8 || distanceSquared >= radius * radius)
        return false;
    const float inverseDistance = rsqrt(distanceSquared);
    const float distanceToLight = distanceSquared * inverseDistance;
    const float3 lightDirection = toLight * inverseDistance;
    const float rangeFactor = saturate(1.0 - distanceToLight / radius);
    float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
    attenuation *= localLightProfile(light, -lightDirection);
    if (light.parameters.y > 1.5) // Spot
    {
        const float cone = dot(normalize(-light.directionOuterCos.xyz), lightDirection);
        attenuation *= smoothstep(light.directionOuterCos.w, light.parameters.x, cone);
    }
    const float nDotL = localNDotL(surface.nDotLMode, surface.normal, lightDirection);
    if (attenuation <= 0.0 || nDotL <= 0.0)
        return false;
    const float roughness = max(surface.roughness, 0.045);
    const float3 halfway = normalize(viewDirection + lightDirection);
    const float3 fresnel = fresnelSchlick(max(dot(halfway, viewDirection), 0.0), surface.f0);
    const float geometry = geometryView * geometrySchlickGGX(nDotL, roughness);
    const float3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * nDotL * attenuation;
    contribution.specular = distributionGGX(surface.normal, halfway, roughness) * geometry * fresnel /
                            max(4.0 * nDotV * nDotL, 0.0001) * energyCompensation * radiance;
    contribution.diffuse = (1.0 - fresnel) * radiance;
    return true;
}
