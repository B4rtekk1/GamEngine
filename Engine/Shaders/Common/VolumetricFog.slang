// Shared layout of the volumetric fog kernels (Passes/VolumetricFogPass).
//
// The froxel grid is aligned with the Game View frustum: x/y follow screen
// uv, z follows view depth exponentially from FOG_NEAR to params.grid.y, so
// near slices are thin and far ones cover more metres.
import Common.SceneData;

// Must match VolumetricFogPass::PushConstants.
struct VolumetricFogParams
{
    // rgb: scattering albedo, w: extinction (1/m) at and below base height.
    float4 albedoDensity;
    // x: base height, y: height falloff (1/m), z: anisotropy g, w: noise.
    float4 height;
    // x: directional, y: local lights, z: ambient, w: temporal history weight.
    float4 intensity;
    // x: near depth, y: far depth (m), z: 1 when the history is valid, w: depth jitter in [0, 1).
    float4 grid;
    // xyz: froxel counts, w: frame counter.
    uint4 size;
};
[[vk::push_constant]] ConstantBuffer<VolumetricFogParams> fogParams;

static const float FOG_PI = 3.14159265359;

// Slice coordinate in [0, 1] of a view depth, and back.
float fogSliceFromDepth(float depth)
{
    const float near = fogParams.grid.x;
    return saturate(log(max(depth, near) / near) / log(fogParams.grid.y / near));
}

float fogDepthFromSlice(float slice)
{
    const float near = fogParams.grid.x;
    return near * pow(fogParams.grid.y / near, slice);
}

// View-space position on the camera ray through `uv` at view depth `depth`.
float3 fogViewPosition(float2 uv, float depth, float4x4 inverseProjection)
{
    const float4 far = mul(float4(uv * 2.0 - 1.0, 1.0, 1.0), inverseProjection);
    const float3 direction = far.xyz / far.w;
    return direction * (depth / max(-direction.z, 1.0e-6));
}

float fogHenyeyGreenstein(float cosine, float g)
{
    const float g2 = g * g;
    const float denominator = max(1.0 + g2 - 2.0 * g * cosine, 1.0e-4);
    return (1.0 - g2) / (4.0 * FOG_PI * denominator * sqrt(denominator));
}
