import Common.SceneData;

// A stable value per small patch of ground.  It deliberately does not depend
// on frame time, so individual blades keep their own character instead of
// shimmering as the camera moves.
float grassWindHash(float2 value)
{
    return frac(sin(dot(value, float2(127.1, 311.7))) * 43758.5453);
}

float3 grassWindRotateByQuaternion(float3 value, float4 q)
{
    return value + 2.0 * cross(q.xyz, cross(q.xyz, value) + q.w * value);
}

// Returns the local-space bend to apply to a grass blade.  The long waves
// remain coherent over the field, while the finer movement is decorrelated in
// small ground patches.  Keeping the complete deformation here makes the
// forward, shadow, and motion-vector passes agree exactly.
float3 grassWindDisplacement(float2 worldPosition, float4 inverseRotation,
                             float normalizedHeight, float grassHeight,
                             float time, float4 directionStrength,
                             float4 sourcePositionRange,
                             float4 gustFrequencyTime)
{
    float2 worldDirection = directionStrength.xz;
    float directionLength = length(worldDirection);
    if (directionLength <= 1.0e-5 || grassHeight <= 0.0)
        return 0.0;
    float radius = sourcePositionRange.w;
    if (radius <= 1.0e-5)
        return 0.0;
    // The inner 65% is steady; the outer ring fades smoothly to zero.  This
    // keeps the source boundary invisible while making its position useful.
    float distanceToSource = length(worldPosition - sourcePositionRange.xz);
    float influence = 1.0 - smoothstep(radius * 0.65, radius, distanceToSource);
    worldDirection /= directionLength;

    float2 crossDirection = float2(-worldDirection.y, worldDirection.x);
    float travel = dot(worldPosition, worldDirection);
    float crossTravel = dot(worldPosition, crossDirection);
    float frequency = gustFrequencyTime.y;
    float broadWave = sin(travel * 0.19 - time * frequency);
    float detailWave = sin(travel * 0.47 + crossTravel * 0.13 - time * frequency * 1.43 + 1.7);
    float gust = smoothstep(0.18, 0.88, 0.5 + 0.5 * lerp(broadWave, detailWave, 0.32));

    float patchPhase = grassWindHash(floor(worldPosition * 1.7));
    float flutter = sin(time * (3.6 + patchPhase * 2.1) + crossTravel * 1.9 + patchPhase * 6.2831853);
    float windAmount = influence * (directionStrength.w * (0.28 + 0.72 * gust) +
                                    gustFrequencyTime.x * gust);
    float2 windDirection = worldDirection + crossDirection * flutter * (0.055 + 0.105 * gust);
    float2 localDirection = grassWindRotateByQuaternion(
        float3(windDirection.x, 0.0, windDirection.y), inverseRotation).xz;
    float bendWeight = normalizedHeight * normalizedHeight * (0.30 + 0.70 * normalizedHeight);
    float2 offset = localDirection * windAmount * bendWeight * grassHeight;
    return float3(offset.x, -dot(offset, offset) / max(grassHeight, 1.0e-4) * 0.42, offset.y);
}
