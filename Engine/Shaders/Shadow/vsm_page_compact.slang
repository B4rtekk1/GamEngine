// Compacts the receiver-marked virtual-page bitset into page keys. Allocation
// can consume this list directly; no CPU scan over VirtualPageCount is needed.
[[vk::binding(0, 0)]] StructuredBuffer<uint> requestedPageBits;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> compactedPageKeys;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> compactedPageCount;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 threadId : SV_DispatchThreadID)
{
    const uint wordIndex = threadId.x;
    const uint wordCount = (7168u + 31u) / 32u;
    if (wordIndex >= wordCount) return;

    uint bits = requestedPageBits[wordIndex];
    while (bits != 0u)
    {
        const uint bit = firstbitlow(bits);
        const uint pageKey = wordIndex * 32u + bit;
        if (pageKey < 7168u)
        {
            uint destination;
            InterlockedAdd(compactedPageCount[0], 1u, destination);
            compactedPageKeys[destination] = pageKey;
        }
        bits &= bits - 1u;
    }
}
