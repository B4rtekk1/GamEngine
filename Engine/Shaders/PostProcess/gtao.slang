// Ground-truth ambient occlusion at half resolution.  This module is deliberately
// independent from material data: its output is a visibility term consumed by
// the lighting pass, never a multiplier for the final HDR colour.
struct VertexOutput { float4 position : SV_Position; [[vk::location(0)]] float2 uv; };
[shader("vertex")]
VertexOutput vertexMain(uint index : SV_VertexID) {
    VertexOutput result; float2 p = float2((index << 1) & 2, index & 2);
    result.uv = p; result.position = float4(p * 2.0 - 1.0, 0.0, 1.0); return result;
}

[[vk::binding(0, 0)]] Sampler2D depthTexture;
struct Settings { float4x4 inverseProjection; float2 fullResolution; float radius; float falloff; };
[[vk::push_constant]] ConstantBuffer<Settings> settings;
struct FragmentInput { [[vk::location(0)]] float2 uv; };
struct FragmentOutput { [[vk::location(0)]] float ao : SV_Target; };

float3 viewPosition(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 view = mul(clip, settings.inverseProjection);
    return view.xyz / max(view.w, 1e-6);
}

float3 reconstructNormal(float2 uv, float3 center) {
    float2 texel = 1.0 / settings.fullResolution;
    float3 dx = viewPosition(uv + float2(texel.x, 0.0), depthTexture.SampleLevel(uv + float2(texel.x, 0.0), 0).r) - center;
    float3 dy = viewPosition(uv + float2(0.0, texel.y), depthTexture.SampleLevel(uv + float2(0.0, texel.y), 0).r) - center;
    return normalize(cross(dx, dy));
}

[shader("fragment")]
FragmentOutput fragmentMain(FragmentInput input) {
    FragmentOutput output;
    float depth = depthTexture.SampleLevel(input.uv, 0).r;
    // Far-plane pixels are sky and must not introduce a dark halo.
    if (depth >= 0.999999) { output.ao = 1.0; return output; }
    float3 center = viewPosition(input.uv, depth);
    float3 normal = reconstructNormal(input.uv, center);
    const uint directions = 8u;
    const uint steps = 4u;
    float occlusion = 0.0;
    const float rotation = frac(sin(dot(floor(input.uv * settings.fullResolution), float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    for (uint direction = 0; direction < directions; ++direction) {
        float angle = rotation + 6.2831853 * (float(direction) / float(directions));
        float2 ray = float2(cos(angle), sin(angle));
        for (uint step = 1; step <= steps; ++step) {
            float t = (float(step) - 0.5) / float(steps);
            float2 sampleUv = input.uv + ray * (t * settings.radius / settings.fullResolution);
            float sampleDepth = depthTexture.SampleLevel(sampleUv, 0).r;
            float3 samplePosition = viewPosition(sampleUv, sampleDepth);
            float3 horizon = samplePosition - center;
            float distanceSquared = dot(horizon, horizon);
            float visibility = saturate(dot(normal, horizon) / sqrt(max(distanceSquared, 1e-5)));
            float attenuation = saturate(1.0 - distanceSquared / max(settings.falloff * settings.falloff, 1e-5));
            occlusion += visibility * attenuation;
        }
    }
    output.ao = saturate(1.0 - occlusion / float(directions * steps));
    return output;
}
