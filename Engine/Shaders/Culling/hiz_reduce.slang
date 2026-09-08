[[vk::binding(0, 0)]] Sampler2D sourceMip;
[[vk::binding(1, 0)]] RWTexture2D<float> destinationMip;

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint destinationWidth;
    uint destinationHeight;
    destinationMip.GetDimensions(destinationWidth, destinationHeight);
    if (any(dispatchThreadId.xy >= uint2(destinationWidth, destinationHeight)))
        return;

    uint sourceWidth;
    uint sourceHeight;
    sourceMip.GetDimensions(sourceWidth, sourceHeight);
    int2 sourceSize = int2(sourceWidth, sourceHeight);
    int2 sourceBase = int2(dispatchThreadId.xy) * 2;
    int2 p0 = min(sourceBase + int2(0, 0), sourceSize - 1);
    int2 p1 = min(sourceBase + int2(1, 0), sourceSize - 1);
    int2 p2 = min(sourceBase + int2(0, 1), sourceSize - 1);
    int2 p3 = min(sourceBase + int2(1, 1), sourceSize - 1);

    float d0 = sourceMip.Load(int3(p0, 0)).r;
    float d1 = sourceMip.Load(int3(p1, 0)).r;
    float d2 = sourceMip.Load(int3(p2, 0)).r;
    float d3 = sourceMip.Load(int3(p3, 0)).r;
    destinationMip[dispatchThreadId.xy] = max(max(d0, d1), max(d2, d3));
}
