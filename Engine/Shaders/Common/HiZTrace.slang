// Hierarchical (Hi-Z) screen-space ray tracing, shared by GlassPass
// (reflection and refraction) and Forward PBR (screen-space reflections).
//
// Inputs are a closest-surface pyramid (MIN hardware depth, 0 near, 1 far)
// whose extents follow HiZBuffer, floor(size / 2^mip), and the exact
// hardware depth for level 0. A caller whose pyramid stores level 0 itself
// passes the pyramid twice. Screen positions are (uv, hardware depth);
// perspective keeps rays straight in that space.

static const float HIZ_FLT_MAX = 3.402823466e+38;
static const float HIZ_PI = 3.14159265359;

// Mip extents follow HiZBuffer: floor(size / 2^mip), at least one texel.
float2 hizMipResolution(float2 size, int mip)
{
    return max(floor(size * exp2(-float(mip))), float2(1.0, 1.0));
}

float3 hizScreenFromView(float3 view, float4x4 projection)
{
    const float4 clip = mul(float4(view, 1.0), projection);
    return float3(clip.xy / clip.w * 0.5 + 0.5, clip.z / clip.w);
}

float3 hizViewFromScreen(float2 uv, float depth, float4x4 inverseProjection)
{
    const float4 view = mul(float4(uv * 2.0 - 1.0, depth, 1.0), inverseProjection);
    return view.xyz / view.w;
}

// mipSize: the level's extent (hizMipResolution), which callers already
// know; a size query per lookup would cost a texture instruction of its own.
float hizLoadClosestDepth(Sampler2D levelZero, Sampler2D pyramid, int2 texel, int mip, float2 mipSize)
{
    const int2 clamped = clamp(texel, int2(0, 0), int2(mipSize) - 1);
    return mip == 0 ? levelZero.Load(int3(clamped, 0)).r : pyramid.Load(int3(clamped, mip)).r;
}

// As hizLoadClosestDepth, for a texel the caller has already clamped.
float hizLoadClosestDepthTexel(Sampler2D levelZero, Sampler2D pyramid, int2 texel, int mip)
{
    return mip == 0 ? levelZero.Load(int3(texel, 0)).r : pyramid.Load(int3(texel, mip)).r;
}

float hizSceneDepthAt(Sampler2D levelZero, float2 size, float2 uv)
{
    const int2 texel = clamp(int2(uv * size), int2(0, 0), int2(size) - 1);
    return levelZero.Load(int3(texel, 0)).r;
}

// Projects a view-space ray to screen space (uv, hardware depth). Perspective
// keeps lines straight, so any end point in front of the camera gives the
// same screen-space line; pull it towards the camera at most half way.
bool hizScreenSpaceRay(float3 originView, float3 directionView, float4x4 projection, out float3 originScreen,
                       out float3 directionScreen)
{
    originScreen = hizScreenFromView(originView, projection);
    directionScreen = float3(0.0, 0.0, 0.0);
    const float originDistance = max(-originView.z, 1.0e-3);
    float rayLength = max(originDistance, 1.0);
    if (directionView.z > 1.0e-5)
        rayLength = min(rayLength, 0.5 * originDistance / directionView.z);
    if (rayLength <= 1.0e-5)
        return false;
    directionScreen = hizScreenFromView(originView + directionView * rayLength, projection) - originScreen;
    return any(abs(directionScreen.xy) > 1.0e-7);
}

// Hierarchical traversal of the closest-depth pyramid (Uludag, GPU Pro 5;
// AMD FidelityFX SSSR). The ray walks cell boundaries: while it stays in
// front of a cell's closest surface it climbs to a coarser level and skips
// the whole cell, otherwise it descends until it meets the surface at mip 0.
// Hardware depth is linear in screen space, so the ray is a straight line
// in (uv, depth) and the surface test is a plane intersection.
bool hizTrace(Sampler2D levelZero, Sampler2D pyramid, float2 size, int maxMip, float3 origin, float3 direction,
              uint maxIterations, out float3 hit)
{
    const float3 inverseDirection = float3(direction.x != 0.0 ? 1.0 / direction.x : HIZ_FLT_MAX,
                                           direction.y != 0.0 ? 1.0 / direction.y : HIZ_FLT_MAX,
                                           direction.z != 0.0 ? 1.0 / direction.z : HIZ_FLT_MAX);
    // Step just past a boundary so the next lookup lands in the next cell.
    const float2 crossOffset = float2(direction.x < 0.0 ? -0.005 : 0.005, direction.y < 0.0 ? -0.005 : 0.005) / size;
    const float2 floorOffset = float2(direction.x < 0.0 ? 0.0 : 1.0, direction.y < 0.0 ? 0.0 : 1.0);

    int mip = 0;
    // The level's extent in integers: max(size >> mip, 1) is
    // hizMipResolution for the integral sizes callers pass, and the shift
    // runs on the ALU pipe where floor and float-to-int conversions would
    // queue on the narrow XU pipe, the SSR trace's limiter.
    const int2 baseSize = int2(size);
    int2 lastTexel = baseSize - 1;
    float2 mipSize = size;
    float2 inverseMipSize = 1.0 / size;
    // Leave the origin texel first: a surface never reflects itself.
    float2 boundary = (floor(origin.xy * mipSize) + floorOffset) * inverseMipSize + crossOffset;
    float2 boundaryT = (boundary - origin.xy) * inverseDirection.xy;
    float t = min(boundaryT.x, boundaryT.y);
    float3 position = origin + direction * t;

    uint iteration = 0u;
    [loop] while (mip >= 0 && iteration < maxIterations)
    {
        if (any(position.xy < 0.0) || any(position.xy > 1.0) || position.z < 0.0 || position.z > 1.0)
        {
            hit = position;
            return false;
        }
        // In bounds, so truncation is the floor.
        const int2 cell = int2(position.xy * mipSize);
        // In bounds, cell is at most the extent itself (position 1.0).
        const float surface = hizLoadClosestDepthTexel(levelZero, pyramid, min(cell, lastTexel), mip);
        boundary = (float2(cell) + floorOffset) * inverseMipSize + crossOffset;
        float3 planeT = (float3(boundary, surface) - origin) * inverseDirection;
        // Only a ray moving away from the camera can meet the surface plane.
        planeT.z = direction.z > 0.0 ? planeT.z : HIZ_FLT_MAX;
        const float nearestT = min(min(planeT.x, planeT.y), planeT.z);
        const bool inFront = surface > position.z;
        // Reached the cell boundary in front of everything in the cell.
        const bool skippedCell = inFront && nearestT != planeT.z;
        if (inFront)
            t = nearestT;
        position = origin + direction * t;
        const bool climb = skippedCell && mip < maxMip;
        if (climb || !skippedCell)
        {
            mip += climb ? 1 : -1;
            const int2 mipExtent = max(baseSize >> max(mip, 0), int2(1, 1));
            lastTexel = mipExtent - 1;
            mipSize = float2(mipExtent);
            inverseMipSize = 1.0 / mipSize;
        }
        ++iteration;
    }
    hit = position;
    return mip < 0;
}

// Accepts a hit only where the ray actually meets the depth surface (not a
// thin object it passed behind) and fades it out at the screen border.
// thickness: smallest geometric thickness assumed for screen-space surfaces
// (metres); thicknessScale: the share of view depth added to it.
float hizHitConfidence(Sampler2D levelZero, float2 size, float4x4 inverseProjection, float3 hit, float3 originScreen,
                       float thickness, float thicknessScale)
{
    if (any(hit.xy <= 0.0) || any(hit.xy >= 1.0))
        return 0.0;
    const float surface = hizSceneDepthAt(levelZero, size, hit.xy);
    if (surface >= 0.99999)
        return 0.0;
    const float3 surfaceView = hizViewFromScreen(hit.xy, surface, inverseProjection);
    const float3 hitView = hizViewFromScreen(hit.xy, hit.z, inverseProjection);
    const float allowed = thickness + thicknessScale * -surfaceView.z;
    const float behind = length(hitView - surfaceView);
    const float depthConfidence = 1.0 - smoothstep(0.5 * allowed, allowed, behind);
    const float2 fade = 0.025 * float2(size.y / size.x, 1.0);
    const float2 border = smoothstep(float2(0.0, 0.0), fade, hit.xy) *
                          (1.0 - smoothstep(1.0 - fade, float2(1.0, 1.0), hit.xy));
    // A ray that never left its own pixel hit the surface it started on.
    return depthConfidence * border.x * border.y * (distance(hit.xy, originScreen.xy) > 0.5 / size.y ? 1.0 : 0.0);
}

// Samples a mip-mapped colour with a footprint of `radius` pixels: a rotated
// four-tap pattern at the matching mip hides the box-filtered mip chain.
float3 hizSampleBlurred(Sampler2D color, float2 size, float2 uv, float radius, float rotation)
{
    if (radius <= 0.75)
        return color.SampleLevel(uv, 0.0).rgb;
    const float lod = max(log2(radius) - 0.5, 0.0);
    const float2 offset = float2(cos(rotation), sin(rotation)) * 0.5 * radius / size;
    const float2 orthogonal = float2(-offset.y, offset.x);
    float3 result = color.SampleLevel(uv + offset, lod).rgb;
    result += color.SampleLevel(uv - offset, lod).rgb;
    result += color.SampleLevel(uv + orthogonal, lod).rgb;
    result += color.SampleLevel(uv - orthogonal, lod).rgb;
    return result * 0.25;
}

// Jimenez's interleaved gradient noise, shifted per TAA frame.
float hizInterleavedGradientNoise(float2 pixel, uint frameIndex)
{
    pixel += 5.588238 * float(frameIndex & 63u);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Visible-normal sampling of the GGX lobe (Heitz 2018) in tangent space.
float3 hizSampleGgxVisibleNormal(float3 viewTangent, float alpha, float2 u)
{
    const float3 stretched = normalize(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));
    const float lengthSquared = dot(stretched.xy, stretched.xy);
    const float3 t1 = lengthSquared > 0.0 ? float3(-stretched.y, stretched.x, 0.0) * rsqrt(lengthSquared)
                                          : float3(1.0, 0.0, 0.0);
    const float3 t2 = cross(stretched, t1);
    const float r = sqrt(u.x);
    const float phi = 2.0 * HIZ_PI * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5 * (1.0 + stretched.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;
    const float3 normal = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * stretched;
    return normalize(float3(alpha * normal.x, alpha * normal.y, max(normal.z, 1.0e-4)));
}
