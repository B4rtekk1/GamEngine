// Stochastic local lighting (MegaLights-style), denoise and composite.
// Accumulates the shading step's noisy estimate over frames: last frame's
// result is reprojected by camera motion, rejected where the stored depth
// disagrees, and clamped to this frame's local neighbourhood (mean +- a few
// standard deviations) so moving shadows do not smear. Diffuse lighting is
// accumulated without the surface colour, so texture detail stays sharp;
// the result is added to the HDR target.
import Common.SceneData;
import Common.DeferredLocalLighting;

[[vk::binding(0, 0)]] Sampler2D depthTexture;
[[vk::binding(1, 0), vk::image_format("rgba32ui")]] RWTexture2D<uint4> surfaceData;
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> noisyDiffuse;
[[vk::binding(3, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> noisySpecular;
// Last frame's accumulation: diffuse.a holds the view depth, specular.a the
// history length.
[[vk::binding(5, 0)]] Sampler2D historyDiffuse;
[[vk::binding(6, 0)]] Sampler2D historySpecular;
[[vk::binding(7, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> accumulatedDiffuse;
[[vk::binding(8, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> accumulatedSpecular;
[[vk::binding(9, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> hdrColor;

// Shortest effective history (~16 frames); stochastic lighting needs more
// than the binary-visibility denoiser.
static const float MinTemporalWeight = 1.0 / 16.0;
static const float ClampSigma = 2.5;

float3 reconstructViewPosition(int2 pixel, float depth, int2 extent)
{
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);
    const float4 position = mul(float4(uv * 2.0 - 1.0, depth, 1.0), frame.inverseProjection);
    return position.xyz / position.w;
}

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    hdrColor.GetDimensions(width, height);
    const int2 extent = int2(width, height);
    const int2 pixel = int2(id.xy);
    if (any(pixel >= extent))
        return;
    DeferredLocalSurface surface;
    if (!unpackDeferredLocalSurface(surfaceData[pixel], surface))
    {
        accumulatedDiffuse[pixel] = 0.0;
        accumulatedSpecular[pixel] = 0.0;
        return;
    }
    const float depth = depthTexture.Load(int3(pixel, 0)).x;
    const float3 viewPosition = reconstructViewPosition(pixel, depth, extent);
    const float viewDepth = -viewPosition.z;

    // Neighbourhood statistics of this frame's estimate (same surface only).
    const float4 centerDiffuse = noisyDiffuse[pixel];
    const float3 centerSpecular = noisySpecular[pixel].rgb;
    float3 diffuseMean = 0.0, diffuseSquare = 0.0, specularMean = 0.0, specularSquare = 0.0;
    float count = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 neighbour = clamp(pixel + int2(dx, dy), int2(0, 0), extent - 1);
            const float4 d = noisyDiffuse[neighbour];
            if (d.a <= 0.0)
                continue;
            const float3 s = noisySpecular[neighbour].rgb;
            diffuseMean += d.rgb;
            diffuseSquare += d.rgb * d.rgb;
            specularMean += s;
            specularSquare += s * s;
            count += 1.0;
        }
    }
    const float inverseCount = count > 0.0 ? 1.0 / count : 0.0;
    diffuseMean *= inverseCount;
    specularMean *= inverseCount;
    const float3 diffuseSigma = sqrt(max(diffuseSquare * inverseCount - diffuseMean * diffuseMean, 0.0));
    const float3 specularSigma = sqrt(max(specularSquare * inverseCount - specularMean * specularMean, 0.0));
    // The neighbourhood mean is a better current estimate than one pixel.
    const float3 currentDiffuse = count > 0.0 ? lerp(centerDiffuse.rgb, diffuseMean, 0.5) : 0.0;
    const float3 currentSpecular = count > 0.0 ? lerp(centerSpecular, specularMean, 0.5) : 0.0;

    // Reproject by camera motion; geometry and lights are static.
    const float3 worldPosition = mul(float4(viewPosition, 1.0), frame.inverseView).xyz;
    const float4 previousView = mul(float4(worldPosition, 1.0), frame.previousView);
    const float4 previousClip = mul(previousView, frame.previousProjection);
    float historyLength = 0.0;
    float3 previousDiffuse = 0.0;
    float3 previousSpecular = 0.0;
    if (previousClip.w > 0.0)
    {
        const float2 previousUv = previousClip.xy / previousClip.w * 0.5 + 0.5;
        const int2 previousPixel = int2(floor(previousUv * float2(extent)));
        if (all(previousUv >= 0.0) && all(previousPixel < extent))
        {
            const float4 d = historyDiffuse.Load(int3(previousPixel, 0));
            const float4 s = historySpecular.Load(int3(previousPixel, 0));
            const float expectedDepth = -previousView.z / previousView.w;
            if (abs(d.a - expectedDepth) < 0.03 * expectedDepth + 0.02)
            {
                historyLength = s.a;
                previousDiffuse = d.rgb;
                previousSpecular = s.rgb;
            }
        }
    }
    if (historyLength > 0.0 && count > 0.0)
    {
        previousDiffuse = clamp(previousDiffuse, diffuseMean - ClampSigma * diffuseSigma,
                                diffuseMean + ClampSigma * diffuseSigma);
        previousSpecular = clamp(previousSpecular, specularMean - ClampSigma * specularSigma,
                                 specularMean + ClampSigma * specularSigma);
    }
    float3 diffuse = currentDiffuse;
    float3 specular = currentSpecular;
    if (historyLength > 0.0)
    {
        // Without a fresh estimate here, keep the history.
        const float weight = count > 0.0 ? max(1.0 / (historyLength + 1.0), MinTemporalWeight) : 0.0;
        diffuse = lerp(previousDiffuse, currentDiffuse, weight);
        specular = lerp(previousSpecular, currentSpecular, weight);
    }
    const float newLength = count > 0.0 || historyLength > 0.0 ? min(historyLength + 1.0, 255.0) : 0.0;
    accumulatedDiffuse[pixel] = float4(diffuse, viewDepth);
    accumulatedSpecular[pixel] = float4(specular, newLength);

    const float3 lighting = surface.diffuseColor * diffuse + specular;
    if (any(lighting > 0.0))
    {
        const float4 color = hdrColor[pixel];
        hdrColor[pixel] = float4(min(color.rgb + lighting, 65000.0), color.a);
    }
}
