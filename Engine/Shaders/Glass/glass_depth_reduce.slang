// One level of GlassPass's closest-surface pyramid: each texel stores the
// MIN hardware depth (0 near, 1 far) of the source texels it covers. Rays
// that stay in front of a texel's minimum cannot hit anything inside it,
// which lets the Hi-Z trace in glass_surface.slang skip whole regions.
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
    const int2 sourceSize = int2(sourceWidth, sourceHeight);
    const int2 destinationSize = int2(destinationWidth, destinationHeight);
    // Cover exactly the source texels inside this texel's UV range (up to
    // 3x3 on odd-sized levels), so the hierarchy stays conservative at the
    // uv * size lookups the trace makes.
    const int2 texel = int2(dispatchThreadId.xy);
    const int2 first = (texel * sourceSize) / destinationSize;
    const int2 last = min(((texel + 1) * sourceSize + destinationSize - 1) / destinationSize - 1, sourceSize - 1);
    float depth = 1.0;
    for (int y = first.y; y <= last.y; ++y)
        for (int x = first.x; x <= last.x; ++x)
            depth = min(depth, sourceMip.Load(int3(x, y, 0)).r);
    destinationMip[dispatchThreadId.xy] = depth;
}
