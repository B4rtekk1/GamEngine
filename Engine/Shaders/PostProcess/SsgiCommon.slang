// Helpers shared by the screen-space global illumination kernels. View space
// follows GTAO: the camera looks down -z and `depth` is the positive linear
// view distance, so a view position is ((uv * 2 - 1) / projectionScale, -1) * depth.
static const float SsgiBackgroundDepth = 65500.0;
static const float SsgiPi = 3.14159265359;

float ssgiLinearizeDepth(float z, float4 depthToView) {
    if (z >= 0.999999) return SsgiBackgroundDepth;
    float viewZ = z * depthToView.x + depthToView.y;
    float viewW = z * depthToView.z + depthToView.w;
    return max(-viewZ / max(viewW, 1e-6), 1e-4);
}

float3 ssgiViewPosition(float2 uv, float depth, float2 projectionScale) {
    return float3((uv * 2.0 - 1.0) / projectionScale, -1.0) * depth;
}

float3 ssgiSafeNormalize(float3 value) {
    return value * rsqrt(max(dot(value, value), 1e-12));
}

float3 ssgiDecodeOct(float2 e) {
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return ssgiSafeNormalize(n);
}

// Two 16-bit snorm octahedral coordinates in one word (see ssgi_prepare).
uint ssgiPackOct(float2 e) {
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xffffu) | (uint(q.y) << 16u);
}

float3 ssgiUnpackNormal(uint packed) {
    return ssgiDecodeOct(float2(int(packed << 16u) >> 16, int(packed) >> 16) / 32767.0);
}

// Abramowitz & Stegun 4.4.45, as in GTAO.
float ssgiFastAcos(float x) {
    float a = abs(x);
    float r = (((-0.0187293 * a + 0.0742610) * a - 0.2121144) * a + 1.5707288) * sqrt(1.0 - a);
    return x >= 0.0 ? r : SsgiPi - r;
}

// Jimenez's interleaved gradient noise, shifted by the R2 sequence each frame
// so TAA and the SSGI history integrate a different slice/step phase.
float2 ssgiNoise(uint2 pixel, uint frame) {
    float ign = frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
    return frac(ign + float(frame % 64u) * float2(0.754877666, 0.569840296));
}

float ssgiLuminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}
