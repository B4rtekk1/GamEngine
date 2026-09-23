[[vk::binding(0, 0)]] Sampler2D hardwareDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> linearDepth;
struct Settings { float4x4 inverseProjection; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height; linearDepth.GetDimensions(width, height);
    if (any(id.xy >= uint2(width, height))) return;
    float z = hardwareDepth.Load(int3(id.xy, 0)).r;
    if (z >= 0.999999) { linearDepth[id.xy] = 65500.0; return; }
    float4 view = mul(float4(0, 0, z, 1), settings.inverseProjection);
    linearDepth[id.xy] = max(-view.z / max(view.w, 1e-6), 1e-4);
}
