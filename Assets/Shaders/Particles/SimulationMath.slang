uint particleHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

float particleRandom(inout uint state)
{
    state = particleHash(state);
    return float(state) * (1.0 / 4294967296.0);
}

float3 curlNoise(float3 position)
{
    const float a = 1.73;
    const float b = 2.31;
    const float c = 2.87;
    return float3(cos(position.z * c) * c - cos(position.y * b) * b,
                  cos(position.x * a) * a - cos(position.z * c) * c,
                  cos(position.y * b) * b - cos(position.x * a) * a);
}

float expandedAabbDistance(float3 point, float3 center, float3 extents,
                           out float3 normal)
{
    float3 relative = point - center;
    float3 q = abs(relative) - extents;
    float3 outside = max(q, 0.0);
    float outsideLength = length(outside);
    if (outsideLength > 0.0001)
    {
        normal = sign(relative) * outside / outsideLength;
        return outsideLength;
    }

    if (q.x >= q.y && q.x >= q.z)
        normal = float3(relative.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    else if (q.y >= q.z)
        normal = float3(0.0, relative.y >= 0.0 ? 1.0 : -1.0, 0.0);
    else
        normal = float3(0.0, 0.0, relative.z >= 0.0 ? 1.0 : -1.0);
    return max(q.x, max(q.y, q.z));
}
