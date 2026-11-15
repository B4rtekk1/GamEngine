// Volumetric fog, step 2: integrates every froxel column front to back.
// Texel z holds the light scattered towards the camera (rgb) and the
// transmittance (a) from the camera to the far edge of slice z, with the
// energy-conserving per-slice integral of Hillaire (Frostbite, 2015).
import Common.SceneData;
import Common.VolumetricFog;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(0, 1), vk::image_format("rgba16f")]] RWTexture3D<float4> scattering;
[[vk::binding(2, 1), vk::image_format("rgba16f")]] RWTexture3D<float4> integrated;

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint3 size = fogParams.size.xyz;
    if (any(id.xy >= size.xy))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(size.xy);
    // Metres along this column's ray per metre of view depth.
    const float rayScale = length(fogViewPosition(uv, 1.0, frame.inverseProjection));
    float3 inScattered = 0.0;
    float transmittance = 1.0;
    float previousDepth = 0.0;
    for (uint z = 0u; z < size.z; ++z)
    {
        const float depth = fogDepthFromSlice(float(z + 1u) / float(size.z));
        const float stepLength = (depth - previousDepth) * rayScale;
        previousDepth = depth;
        const float4 froxel = scattering[uint3(id.xy, z)];
        const float extinction = max(froxel.a, 1.0e-7);
        const float sliceTransmittance = exp(-extinction * stepLength);
        inScattered += transmittance * froxel.rgb * ((1.0 - sliceTransmittance) / extinction);
        transmittance *= sliceTransmittance;
        integrated[uint3(id.xy, z)] = float4(inScattered, transmittance);
    }
}
