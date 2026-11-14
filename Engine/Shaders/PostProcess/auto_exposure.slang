// Meters the scene-referred HDR frame and adapts the exposure like an eye.
// One workgroup reads a sparse 64x36 grid (2304 bilinear taps of a frame that
// TAA already filtered), builds a log2-luminance histogram in shared memory
// and averages its middle 80%, so a small specular highlight or a dark corner
// cannot swing the exposure. The adapted luminance lives in a 1x1 image that
// the tonemap pass reads; it is never round-tripped through the CPU.
[[vk::binding(0, 0)]] Sampler2D sourceImage;
[[vk::binding(1, 0), vk::image_format("r32f")]] RWTexture2D<float> adaptedLuminance;

struct Settings {
    float minLuminanceEv;
    float maxLuminanceEv;
    float adaptationSpeedUp;
    float adaptationSpeedDown;
    float deltaTime;
    // 1 on the first frame or after a reset: jump to the metered value.
    float reset;
};
[[vk::push_constant]] ConstantBuffer<Settings> settings;

static const uint GridWidth = 64u;
static const uint GridHeight = 36u;
static const uint BinCount = 64u;
// Histogram range in log2 luminance; the clamp range in Settings lies inside.
static const float HistogramMinEv = -12.0;
static const float HistogramMaxEv = 16.0;
static const float LowPercentile = 0.1;
static const float HighPercentile = 0.9;

groupshared uint histogram[BinCount];

float binCentreEv(uint bin) {
    return HistogramMinEv + (float(bin) + 0.5) * (HistogramMaxEv - HistogramMinEv) / float(BinCount);
}

[numthreads(16, 16, 1)] [shader("compute")]
void main(uint3 threadId : SV_GroupThreadID) {
    const uint lane = threadId.y * 16u + threadId.x;
    if (lane < BinCount)
        histogram[lane] = 0u;
    GroupMemoryBarrierWithGroupSync();

    for (uint index = lane; index < GridWidth * GridHeight; index += 256u) {
        const float2 cell = float2(index % GridWidth, index / GridWidth);
        const float2 uv = (cell + 0.5) / float2(GridWidth, GridHeight);
        const float3 color = sourceImage.SampleLevel(uv, 0.0).rgb;
        const float luminance = dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
        if (isnan(luminance) || isinf(luminance))
            continue;
        const float ev = log2(max(luminance, 1.0e-6));
        const float normalized = saturate((ev - HistogramMinEv) / (HistogramMaxEv - HistogramMinEv));
        const uint bin = min(uint(normalized * float(BinCount)), BinCount - 1u);
        // Centre-weighted metering: the middle of the frame counts up to
        // three times as much as its corners.
        const float2 fromCentre = (uv - 0.5) * float2(2.0, 2.0);
        const uint weight = 1u + uint(2.0 * saturate(1.0 - dot(fromCentre, fromCentre)) + 0.5);
        InterlockedAdd(histogram[bin], weight);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane != 0u)
        return;

    uint total = 0u;
    for (uint bin = 0u; bin < BinCount; ++bin)
        total += histogram[bin];
    const float low = float(total) * LowPercentile;
    const float high = float(total) * HighPercentile;
    float below = 0.0;
    float evSum = 0.0;
    float weightSum = 0.0;
    for (uint bin = 0u; bin < BinCount; ++bin) {
        const float count = float(histogram[bin]);
        // The part of this bin that lies inside [low, high].
        const float inside = max(min(below + count, high) - max(below, low), 0.0);
        evSum += binCentreEv(bin) * inside;
        weightSum += inside;
        below += count;
    }
    const float meteredEv = weightSum > 0.0 ? evSum / weightSum : 0.0;
    const float targetEv = clamp(meteredEv, settings.minLuminanceEv, settings.maxLuminanceEv);

    const float previous = adaptedLuminance[uint2(0, 0)];
    const bool previousValid = previous > 0.0 && !isnan(previous) && !isinf(previous);
    float adaptedEv = targetEv;
    if (settings.reset < 0.5 && previousValid) {
        // Adapt in stops, so equal brightness ratios take equal time.
        const float previousEv = log2(previous);
        const float speed = targetEv > previousEv ? settings.adaptationSpeedUp : settings.adaptationSpeedDown;
        adaptedEv = previousEv + (targetEv - previousEv) * (1.0 - exp(-settings.deltaTime * speed));
    }
    adaptedLuminance[uint2(0, 0)] = exp2(adaptedEv);
}
