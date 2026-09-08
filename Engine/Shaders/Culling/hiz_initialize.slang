[[vk::binding(0, 0)]] Sampler2D sourceDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> destinationMip;

[numthreads(8, 8, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    destinationMip.GetDimensions(width, height);
    if (any(dispatchThreadId.xy >= uint2(width, height)))
        return;

    float depth = sourceDepth.Load(int3(dispatchThreadId.xy, 0)).r;
    destinationMip[dispatchThreadId.xy] = depth;
}
