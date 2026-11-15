// Falling rain (Passes/RainPass): thin streaks in a box that follows the
// Game View camera. Nothing is simulated or stored: instance i is drop i,
// whose world position is a hash of i advanced by time and wrapped into the
// box, so drops stay fixed in the world while the camera moves through them.
// Streaks are depth tested against the opaque scene and lit like the fog:
// sky ambient plus every local light of the fragment's light cluster, so
// rain glows in the cone of each street lamp.
import Common.SceneData;

[[vk::binding(1, 0)]] ConstantBuffer<RendererFrameData> frame;
[[vk::binding(3, 0)]] StructuredBuffer<ClusterLightRange> clusterRanges;
[[vk::binding(7, 0)]] StructuredBuffer<uint> clusterLightIndices;
[[vk::binding(10, 0)]] SamplerCube irradianceMap;
[[vk::binding(24, 0)]] StructuredBuffer<LocalLightData> localLights;

// Must match RainPass::PushConstants.
struct RainParams
{
    // x: box half width (m), y: box half height (m), z: fall speed (m/s), w: streak length (m).
    float4 box;
    // x: streak width (m), y: opacity, z: time (s), w: brightness.
    float4 look;
};
[[vk::push_constant]] ConstantBuffer<RainParams> rain;

static const float RAIN_PI = 3.14159265359;

float rainHash(uint n)
{
    n = (n << 13u) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return float(n & 0x7fffffffu) / float(0x7fffffff);
}

struct RainVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 worldPosition;
    // x: -1..1 across the streak, y: 0 head .. 1 tail.
    [[vk::location(1)]] float2 streak;
    [[vk::location(2)]] float alpha;
};

[shader("vertex")]
RainVertex vertexMain(uint vertexIndex : SV_VertexID, uint instanceIndex : SV_InstanceID)
{
    const float3 camera = frame.cameraPosition.xyz;
    const float halfWidth = rain.box.x;
    const float halfHeight = rain.box.y;
    const float time = rain.look.z;
    const uint seed = instanceIndex * 3u;
    // Wind slants the fall; gusts shift every drop alike.
    const float3 wind = frame.windDirectionStrength.xyz * frame.windDirectionStrength.w;
    const float speed = rain.box.z * (0.85 + 0.3 * rainHash(seed + 2u));
    const float3 velocity = float3(wind.x * 2.0, -speed, wind.z * 2.0);

    // World-fixed drop, wrapped into the box around the camera.
    float3 drop = float3(rainHash(seed), rainHash(seed + 1u), rainHash(seed + 2u) * 0.5 + rainHash(seed + 7u) * 0.5);
    drop = float3(drop.x * 2.0 * halfWidth, drop.y * 2.0 * halfHeight, drop.z * 2.0 * halfWidth) + velocity * time;
    const float3 extent = float3(2.0 * halfWidth, 2.0 * halfHeight, 2.0 * halfWidth);
    const float3 local = frac((drop - camera) / extent + 0.5) - 0.5;
    const float3 head = camera + local * extent;

    const float3 direction = normalize(velocity);
    const float3 tail = head - direction * rain.box.w;
    const float3 toCamera = camera - head;
    const float distance = max(length(toCamera), 1.0e-3);
    float3 side = cross(direction, toCamera / distance);
    side = length(side) > 1.0e-4 ? normalize(side) : float3(1.0, 0.0, 0.0);
    // Never thinner than about one pixel: thin far streaks fade instead of aliasing.
    const float pixelAngle = 2.0 / (abs(frame.projection[1][1]) * frame.clusteredViewportNearFar.y);
    const float width = max(rain.look.x, distance * pixelAngle * 1.2);

    // Two triangles: corner c of the quad from the vertex index.
    const uint corner = vertexIndex % 6u;
    const float2 corners[6] = {float2(-1, 0), float2(1, 0), float2(1, 1), float2(-1, 0), float2(1, 1), float2(-1, 1)};
    const float2 c = corners[corner];
    const float3 world = lerp(head, tail, c.y) + side * (c.x * width * 0.5);

    RainVertex output;
    output.position = mul(mul(float4(world, 1.0), frame.view), frame.projection);
    output.worldPosition = world;
    output.streak = c;
    // Fade near the camera (no drops through the lens), at the box edges
    // (wrapping must not pop) and by the width the pixel floor added.
    const float edge = saturate((0.5 - max(abs(local.x), abs(local.z))) * 8.0) *
                       saturate((0.5 - abs(local.y)) * 8.0);
    output.alpha = rain.look.y * edge * smoothstep(0.4, 1.5, distance) * (rain.look.x / width);
    return output;
}

[shader("fragment")]
float4 fragmentMain(RainVertex input) : SV_Target0
{
    // Soft edges across the streak, brighter towards the leading drop.
    const float across = 1.0 - abs(input.streak.x);
    const float along = 1.0 - input.streak.y;
    const float coverage = across * across * (0.35 + 0.65 * along) * input.alpha;
    if (coverage <= 1.0e-4)
        discard;

    float3 light = irradianceMap.SampleLevel(float3(0.0, 1.0, 0.0), 0.0).rgb * (1.0 / RAIN_PI);
    if (frame.localLightCount != 0u)
    {
        const float depth = max(-mul(float4(input.worldPosition, 1.0), frame.view).z, frame.clusteredViewportNearFar.z);
        const uint2 grid = (uint2(frame.clusteredViewportNearFar.xy) + ClusterTilePixelSize - 1u) / ClusterTilePixelSize;
        const uint slice = min(uint(max(log2(depth) * frame.clusterZScaleBiasEnvironmentMipRtContact.x +
                                            frame.clusterZScaleBiasEnvironmentMipRtContact.y,
                                        0.0)),
                               23u);
        const uint2 tile = min(uint2(input.position.xy) / ClusterTilePixelSize, grid - 1u);
        const ClusterLightRange range = clusterRanges[(slice * grid.y + tile.y) * grid.x + tile.x];
        for (uint i = 0u; i < range.lightCount; ++i)
        {
            const uint lightIndex = clusterLightIndices[range.lightOffset + i];
            if (lightIndex >= frame.localLightCount)
                continue;
            const LocalLightData lamp = localLights[lightIndex];
            const float3 toLight = lamp.positionRange.xyz - input.worldPosition;
            const float distanceSquared = dot(toLight, toLight);
            const float radius = lamp.positionRange.w;
            if (distanceSquared <= 1.0e-8 || distanceSquared >= radius * radius)
                continue;
            const float inverseDistance = rsqrt(distanceSquared);
            const float3 lightDirection = toLight * inverseDistance;
            const float rangeFactor = saturate(1.0 - distanceSquared * inverseDistance / radius);
            float attenuation = rangeFactor * rangeFactor / max(distanceSquared, LocalLightSourceRadiusSquared);
            attenuation *= localLightProfile(lamp, -lightDirection);
            if (lamp.parameters.y > 1.5) // Spot
                attenuation *= smoothstep(lamp.directionOuterCos.w, lamp.parameters.x,
                                          dot(normalize(-lamp.directionOuterCos.xyz), lightDirection));
            light += lamp.colorIntensity.rgb * (lamp.colorIntensity.w * attenuation);
        }
    }
    if (frame.lightDirectionIntensity.w > 0.0)
        light += frame.lightColor.rgb * frame.lightDirectionIntensity.w * 0.25;
    // A drop refracts and reflects the light around it: a quarter-sphere
    // worth of the incident light reaches the eye.
    return float4(min(light * rain.look.w, 64.0), coverage);
}
