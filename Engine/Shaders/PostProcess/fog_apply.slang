// Volumetric fog, step 3: composites the integrated volume over the lit HDR
// image, colour * transmittance + in-scattered light, at each pixel's depth.
// The sky takes the whole volume.
import Common.SceneData;
import Common.VolumetricFog;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(3, 1)]] Sampler3D integratedVolume;
[[vk::binding(4, 1)]] Sampler2D sceneDepth;
[[vk::binding(5, 1), vk::image_format("rgba16f")]] RWTexture2D<float4> hdr;

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdr.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height)))
        return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    const float deviceDepth = sceneDepth.Load(int3(id.xy, 0)).r;
    float slice = 1.0;
    if (deviceDepth < 0.99999)
    {
        const float4 view = mul(float4(uv * 2.0 - 1.0, deviceDepth, 1.0), frame.inverseProjection);
        slice = fogSliceFromDepth(-view.z / view.w);
    }
    // Texel z stores the integral to the far edge of slice z: shift by half a slice.
    const float sliceCount = float(fogParams.size.z);
    const float w = clamp(slice - 0.5 / sliceCount, 0.5 / sliceCount, 1.0 - 0.5 / sliceCount);
    float4 fog = integratedVolume.SampleLevel(float3(uv, w), 0.0);
    // Closer than the first slice: fade the first slice's fog in from the camera.
    if (slice * sliceCount < 1.0)
    {
        const float fraction = saturate(slice * sliceCount);
        fog = float4(fog.rgb * fraction, lerp(1.0, fog.a, fraction));
    }
    const float4 color = hdr[id.xy];
    hdr[id.xy] = float4(min(color.rgb * fog.a + fog.rgb, 65000.0), color.a);
}
