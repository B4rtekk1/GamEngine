// Edge-continuity 5x5 spatial filter for GTAO.  The guide contains the
// continuity links generated from the depth pyramid by gtao_main.
[[vk::binding(0,0)]] Sampler2D inputAo;
[[vk::binding(1,0)]] Sampler2D inputGuide;
[[vk::binding(2,0)]] RWTexture2D<float> outputAo;
struct Settings { float blurAmount; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

// Four 2-bit links, packed as left, right, top, bottom by gtao_main.
float4 unpackEdges(float payload) {
    uint packed = uint(payload * 255.5);
    return saturate(float4((packed >> 6) & 3, (packed >> 4) & 3,
                           (packed >> 2) & 3, packed & 3) / 3.0);
}
float4 edgesAt(int2 p, int2 size) {
    return unpackEdges(inputGuide.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).z);
}
float aoAt(int2 p, int2 size) {
    return inputAo.Load(int3(clamp(p, int2(0, 0), size - 1), 0)).r;
}

// Product of the continuity links crossed by an axis-aligned path. A 5x5
// footprint smooths flat regions while never jumping a depth discontinuity.
float pathContinuity(int2 p, int2 delta, int2 size) {
    float continuity = 1.0;
    int2 cursor = p;
    const int stepX = delta.x < 0 ? -1 : 1;
    const int stepY = delta.y < 0 ? -1 : 1;
    [unroll] for (int i = 0; i < 2; ++i) {
        if (i < abs(delta.x)) {
            float4 edge = edgesAt(cursor, size);
            continuity *= stepX < 0 ? edge.x : edge.y;
            cursor.x += stepX;
        }
    }
    [unroll] for (int i = 0; i < 2; ++i) {
        if (i < abs(delta.y)) {
            float4 edge = edgesAt(cursor, size);
            continuity *= stepY < 0 ? edge.z : edge.w;
            cursor.y += stepY;
        }
    }
    return continuity;
}

[numthreads(8,8,1)] [shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    outputAo.GetDimensions(w, h);
    if (any(id.xy >= uint2(w, h))) return;


    const int2 size = int2(w, h);
    const int2 p = int2(id.xy);
    float sum = aoAt(p, size) * settings.blurAmount;
    float weightSum = settings.blurAmount;
    [unroll] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            if (x == 0 && y == 0) continue;
            const int2 offset = int2(x, y);
            const float distanceSquared = float(x * x + y * y);
            const float weight = exp(-0.65 * distanceSquared) *
                                 pathContinuity(p, offset, size);
            sum += aoAt(p + offset, size) * weight;
            weightSum += weight;
        }
    }
    outputAo[p] = sum / max(weightSum, 1e-5);
}
