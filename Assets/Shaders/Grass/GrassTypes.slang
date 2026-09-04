// Shared GPU data for the chunk-driven grass path.  All per-blade records
// are exactly 16 bytes; transform matrices must never enter this path.
struct GrassChunkData
{
    float4 boundsMin;
    float4 boundsMax;
    float2 origin;
    float2 size;
    uint instanceOffset;
    uint candidateCount;
    uint grassType;
    uint seed;
    float density;
    float maxDistance;
    uint biomeId;
    uint _padding;
};

struct GrassInstance
{
    float2 positionXZ;
    // low/high uint16: terrain-relative height / yaw
    uint heightRotation;
    // low/high uint16: scale / grass type
    uint scaleType;
};

struct GrassDrawCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

float grassRandom(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return float(state >> 8u) * (1.0 / 16777216.0);
}

uint grassPackUnorm16(float value)
{
    return uint(round(saturate(value) * 65535.0));
}
