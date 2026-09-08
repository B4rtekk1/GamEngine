import Common.SceneData;

// A stable value per small patch of ground.  It deliberately does not depend
// on frame time, so individual blades keep their own character instead of
// shimmering as the camera moves.
float grassWindHash(float2 value)
{
    return frac(sin(dot(value, float2(127.1, 311.7))) * 43758.5453);
}

struct GrassWindSample { float2 localDirection; float amount; };

float3 grassWindRotateByQuaternion(float3 value, float4 q)
{
    return value + 2.0 * cross(q.xyz, cross(q.xyz, value) + q.w * value);
}

// Returns the local-space bend target for packed terrain grass. The long
// waves remain coherent while the seed decorrelates neighbouring blades.
GrassWindSample grassWindSample(float2 worldPosition, float seed, float yawSine, float yawCosine,
                                float time, float4 directionStrength,
                             float4 sourcePositionRange,
                             float4 gustFrequencyTime)
{
    GrassWindSample sample = {float2(0.0), 0.0};
    float2 worldDirection = directionStrength.xz;
    float directionLength = length(worldDirection);
    if (directionLength <= 1.0e-5)
        return sample;
    float radius = sourcePositionRange.w;
    if (radius <= 1.0e-5)
        return sample;
    // The inner 65% is steady; the outer ring fades smoothly to zero.  This
    // keeps the source boundary invisible while making its position useful.
    float distanceToSource = length(worldPosition - sourcePositionRange.xz);
    float influence = 1.0 - smoothstep(radius * 0.65, radius, distanceToSource);
    worldDirection /= directionLength;

    float2 crossDirection = float2(-worldDirection.y, worldDirection.x);
    float travel = dot(worldPosition, worldDirection);
    float crossTravel = dot(worldPosition, crossDirection);
    float speed = gustFrequencyTime.y;
    float macro = sin(travel * 0.055 - time * speed * 0.35);
    float gustWave = sin(travel * 0.16 + crossTravel * 0.045 - time * speed);
    float gust = smoothstep(0.25, 0.85, gustWave * 0.5 + 0.5);

    float phase = seed * 6.28318530718;
    float flutter = sin(time * (4.5 + seed * 2.0) + crossTravel * 0.9 + phase);
    float windAmount = influence * (directionStrength.w * (0.65 + 0.35 * macro) +
                                    gustFrequencyTime.x * gust);
    float2 windDirection = worldDirection + crossDirection * flutter * (0.055 + 0.105 * gust);
    sample.localDirection = normalize(float2(windDirection.x * yawCosine + windDirection.y * yawSine,
                                             -windDirection.x * yawSine + windDirection.y * yawCosine));
    sample.amount = max(windAmount, 0.0);
    return sample;
}

// Compatibility path for the generic vegetation renderer. Packed terrain
// grass uses grassWindSample and angle-based deformation instead.
float3 grassWindDisplacement(float2 worldPosition, float4 inverseRotation,
                             float normalizedHeight, float grassHeight,
                             float time, float4 directionStrength,
                             float4 sourcePositionRange,
                             float4 gustFrequencyTime)
{
    float2 worldDirection = directionStrength.xz;
    float directionLength = length(worldDirection);
    float radius = sourcePositionRange.w;
    if (directionLength <= 1.0e-5 || grassHeight <= 0.0 || radius <= 1.0e-5)
        return 0.0;
    worldDirection /= directionLength;
    float influence = 1.0 - smoothstep(radius * 0.65, radius,
                                       length(worldPosition - sourcePositionRange.xz));
    float2 crossDirection = float2(-worldDirection.y, worldDirection.x);
    float travel = dot(worldPosition, worldDirection);
    float crossTravel = dot(worldPosition, crossDirection);
    float macro = sin(travel * 0.055 - time * gustFrequencyTime.y * 0.35);
    float gustWave = sin(travel * 0.16 + crossTravel * 0.045 - time * gustFrequencyTime.y);
    float gust = smoothstep(0.25, 0.85, gustWave * 0.5 + 0.5);
    // This legacy path is intentionally not quantized into visible cells.
    float phase = grassWindHash(worldPosition * 1.7) * 6.28318530718;
    float flutter = sin(time * 4.5 + crossTravel * 0.9 + phase);
    float amount = influence * (directionStrength.w * (0.65 + 0.35 * macro) +
                                gustFrequencyTime.x * gust);
    float2 direction = worldDirection + crossDirection * flutter * (0.055 + 0.105 * gust);
    float2 localDirection = grassWindRotateByQuaternion(
        float3(direction.x, 0.0, direction.y), inverseRotation).xz;
    float weight = normalizedHeight * normalizedHeight * (0.30 + 0.70 * normalizedHeight);
    float2 offset = localDirection * amount * weight * grassHeight;
    return float3(offset.x, -dot(offset, offset) / max(grassHeight, 1.0e-4) * 0.42, offset.y);
}
