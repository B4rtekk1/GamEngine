// Accumulates the noisy per-frame SSGI estimate. History is reprojected with
// the camera and rejected where its stored depth disagrees (disocclusion);
// variance clipping against the current neighbourhood bounds ghosting from
// moving objects and changing lights.
import PostProcess.SsgiCommon;

[[vk::binding(0,0)]] Sampler2D currentGi;
[[vk::binding(1,0)]] Sampler2D historyGi;
[[vk::binding(2,0)]] RWTexture2D<float4> outputGi;
struct Settings {
    float4x4 viewToPreviousClip;
    float2 projectionScale;
    uint historyValid;
    float currentWeight;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

bool similarDepth(float a, float b, float tolerance) {
    return abs(a - b) <= tolerance * b + 0.02;
}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id:SV_DispatchThreadID){
    uint w, h;
    outputGi.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;
    const int2 p = int2(id.xy), maxPixel = int2(w, h) - 1;
    const float4 current = currentGi.Load(int3(p, 0));
    const float depth = current.a;
    if (depth >= SsgiBackgroundDepth) {
        outputGi[p] = current;
        return;
    }

    float3 sum = 0.0, sumSquares = 0.0;
    float count = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float4 neighbour = currentGi.Load(int3(clamp(p + int2(x, y), int2(0, 0), maxPixel), 0));
            if (!similarDepth(neighbour.a, depth, 0.1)) continue;
            sum += neighbour.rgb;
            sumSquares += neighbour.rgb * neighbour.rgb;
            count += 1.0;
        }
    const float3 mean = sum / count;
    const float3 deviation = sqrt(max(sumSquares / count - mean * mean, 0.0));

    float3 result = current.rgb;
    if (settings.historyValid != 0u) {
        const float2 halfSize = float2(w, h);
        const float3 position = ssgiViewPosition((float2(p) + 0.5) / halfSize, depth, settings.projectionScale);
        const float4 previousClip = mul(float4(position, 1.0), settings.viewToPreviousClip);
        const float2 previousUv = previousClip.xy / max(previousClip.w, 1e-4) * 0.5 + 0.5;
        if (previousClip.w > 1e-4 && all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            // Bilinear history restricted to texels on the same surface.
            const float2 texel = previousUv * halfSize - 0.5;
            const int2 base = int2(floor(texel));
            const float2 f = frac(texel);
            float3 history = 0.0;
            float historyWeight = 0.0;
            [unroll] for (int y = 0; y <= 1; ++y)
                [unroll] for (int x = 0; x <= 1; ++x) {
                    const float4 tap = historyGi.Load(int3(clamp(base + int2(x, y), int2(0, 0), maxPixel), 0));
                    if (!similarDepth(tap.a, previousClip.w, 0.05)) continue;
                    const float weight = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
                    history += tap.rgb * weight;
                    historyWeight += weight;
                }
            if (historyWeight > 0.05) {
                history /= historyWeight;
                history = clamp(history, mean - 2.0 * deviation, mean + 2.0 * deviation);
                result = lerp(history, current.rgb, settings.currentWeight);
            }
        }
    }
    outputGi[p] = float4(result, depth);
}
