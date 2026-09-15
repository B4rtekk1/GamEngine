import Common.SceneData;

static const uint WATER_INVALID_PHYSICAL_PAGE = 0xffffffffu;
static const uint WATER_FAR_ANALYTIC_PAGE_ID = 0x1fffu;
static const uint WATER_SIMPLE_SHADING_FLAG = 0x08000000u;
static const uint WATER_AUTHORED_FLAG = 0x10000000u;
static const uint WATER_FAR_ANALYTIC_FLAG = 0x20000000u;
static const uint WATER_REACTIVE_FLAG = 0x40000000u;
static const uint WATER_VALID_FLAG = 0x80000000u;
static const uint WATER_PAGE_ACTIVE = 1u << 0u;
static const uint WATER_PAGE_INTERACTION = 1u << 1u;
static const uint WATER_PAGE_FOAM_RESIDENT = 1u << 2u;
static const uint WATER_PAGE_NEWLY_VISIBLE = 1u << 3u;
static const uint WATER_PAGE_STATE_ONLY = 1u << 4u;
static const uint WATER_PAGE_SHORELINE = 1u << 5u;
static const uint WATER_PAGE_SPARSE_STATE = 1u << 6u;
static const uint WATER_PAGE_CAMERA_RELATIVE = 1u << 7u;
static const uint WATER_PAGE_SPLINE = 1u << 8u;
static const uint WATER_EXEC_BUDGETED_SHADING = 1u << 0u;
static const uint WATER_EXEC_SPARSE_STATE = 1u << 1u;
static const uint WATER_EXEC_SIMPLE_SHADING = 1u << 2u;
static const uint WATER_STATE_RESOLUTION = 16u;
static const uint WATER_STATE_CELLS_PER_PAGE = WATER_STATE_RESOLUTION * WATER_STATE_RESOLUTION;
static const uint WATER_MAX_PHYSICAL_STATE_PAGES = 256u;
static const float WATER_TWO_PI = 6.28318530718;
static const uint WATER_META_PAGE_SHIFT = 8u;
static const uint WATER_META_PAGE_MASK = 0x1fffu;
// Compatibility aliases kept local to Water shaders while the codebase migrates
// to the explicit WATER_* naming convention.
static const uint WaterMetaValid = WATER_VALID_FLAG;
static const uint WaterMetaPageShift = WATER_META_PAGE_SHIFT;
static const uint WaterMetaPageMask = WATER_META_PAGE_MASK;
static const uint FarAnalyticPageId = WATER_FAR_ANALYTIC_PAGE_ID;
static const uint FarAnalyticFlag = WATER_FAR_ANALYTIC_FLAG;
static const uint AuthoredWaterFlag = WATER_AUTHORED_FLAG;
static const uint MaxPages = 7184u;
static const uint MaxPhysicalStatePages = WATER_MAX_PHYSICAL_STATE_PAGES;
static const uint StateGridResolution = WATER_STATE_RESOLUTION;
static const uint StateCellsPerPage = WATER_STATE_CELLS_PER_PAGE;

struct GPUVirtualWaterPage {
    float originX, originZ, baseHeight, cellSize;
    float minX, minZ, maxX, maxZ;
    float verticalBound, horizontalBound, unresolvedSlopeVariance, reservedFloat;
    uint level, stitchMask, waveCandidateMask, flags;
    uint instanceIndex, drawBin, bodyIndex, spectralEdgeMask;
    float flowX, flowZ, flowSpeed, reservedFlow;
    uint neighborLeft0, neighborLeft1, neighborRight0, neighborRight1;
    uint neighborBottom0, neighborBottom1, neighborTop0, neighborTop1;
    uint bodyIdIndex, bodyIdGeneration;
    int localPageX, localPageZ;
    uint spectrumRevision, coverageClass, cellCoverageMaskLow, cellCoverageMaskHigh;
    float unresolvedSlopeXX, unresolvedSlopeXZ, unresolvedSlopeZZ, normalDetailImportance;
    float splineStartX, splineStartZ, splineEndX, splineEndZ;
    float splineHalfWidth0, splineHalfWidth1;
    uint executionFlags, stateWorldSizeClass;
};

struct GPUVisibleWaterPage { uint pageIndex, waveCandidateMask, packedQuality, instanceIndex; };
struct GPUWaterDrawTemplate { uint indexCount, firstIndex; int vertexOffset; uint firstInstanceBase; };

struct GPUWaterPageHistory {
    uint packedQuality, visibleAge, framesInTier, occludedFrames;
    float previousImportance;
    int worldPageX, worldPageZ;
    uint reserved;
};

struct GPUWaterPageStats {
    uint visiblePages, frustumRejected, hiZRejected, distanceRejected;
    uint newlyVisible, highQualityPages, stateResidentPages, budgetDrops;
};

struct GPUWaterCullConfig {
    uint pageCount, drawBinCount, visiblePagesPerBin, enableHiZ;
    float reflectionVarianceScale, highQualityPixels, mediumQualityPixels, lowQualityPixels;
    float qualityScale;
    uint currentFrame, occlusionGraceFrames, occludedFramesRequired;
    uint maxHighGeometryPages, padding0, padding1, padding2;
};

struct GPUWaterStatePhysicalRef { uint tier, slot, generation, reserved; };

struct GPUWaterPhysicalState {
    float foam, interaction, maxVelocity, inactiveSeconds;
    uint ownerBodyIndex, ownerBodyGeneration;
    int ownerX, ownerZ;
    uint generation, tier, pinCount, flags;
    uint lastVisibleFrame, lastSimulatedFrame, lastInteractionFrame, updateRatePacked;
};

struct GPUWaterStateCell { float height, velocityX, velocityZ, foam; };
struct GPUWaterInteractionEvent { float worldX, worldZ, radius, strength; };

struct GPUWaterFrameBudget {
    float targetGpuMs, measuredGpuMs, qualityScale, feedbackGain;
    uint maxFullRateSamples, maxReflectionWork, maxRefractionWork, maxStateSimulationCells;
    uint reflectionUsed, refractionUsed, fullRateUsed, stateSimulationUsed;
    uint noRayReflection, noRayRefraction, rayRefreshCounter, budgetSaturated;
};

struct GPUWaterTileSettings {
    uint width, height, tilesX, maxTiles;
    uint denseMode, frameIndex, highThreshold, mediumThreshold;
    uint cheapThreshold, candidateTileCount, historyValid, padding0;
    float temporalBlend, temporalMissBlend;
    uint padding1, padding2;
};

float3 waterRotateQuaternion(float3 p, float4 q) {
    return p + 2.0 * cross(q.xyz, cross(q.xyz, p) + q.w * p);
}

float3 waterInverseRotateQuaternion(float3 p, float4 q) {
    return p + 2.0 * cross(-q.xyz, cross(-q.xyz, p) + q.w * p);
}

float3 waterLocalToWorld(float3 p, RendererInstanceData instance) {
    return waterRotateQuaternion(p * instance.scaleBase.xyz, instance.rotation) + instance.positionMaterial.xyz;
}

float3 waterWorldToLocal(float3 p, RendererInstanceData instance) {
    float3 local = waterInverseRotateQuaternion(p - instance.positionMaterial.xyz, instance.rotation);
    float3 safeScale = max(abs(instance.scaleBase.xyz), float3(1e-5));
    return local / safeScale;
}

float waterPageSize(GPUVirtualWaterPage page) { return page.cellSize * 8.0; }

float2 waterSnappedOffset(GPUVirtualWaterPage page, RendererInstanceData instance, float3 cameraWorld) {
    if ((page.flags & WATER_PAGE_CAMERA_RELATIVE) == 0u) return float2(0.0);
    const float pageSize = max(waterPageSize(page), 1e-4);
    const float3 localCamera = waterWorldToLocal(cameraWorld, instance);
    return floor(localCamera.xz / pageSize) * pageSize;
}

float2 waterPageOrigin(GPUVirtualWaterPage page, RendererInstanceData instance, float3 cameraWorld) {
    return float2(page.originX, page.originZ) + waterSnappedOffset(page, instance, cameraWorld);
}

int2 waterPageWorldCoord(GPUVirtualWaterPage page, RendererInstanceData instance, float3 cameraWorld) {
    if ((page.flags & WATER_PAGE_CAMERA_RELATIVE) == 0u) return int2(page.localPageX, page.localPageZ);
    const float pageSize = max(waterPageSize(page), 1e-4);
    const float3 localCamera = waterWorldToLocal(cameraWorld, instance);
    const int2 center = int2(floor(localCamera.xz / pageSize));
    return center + int2(page.localPageX, page.localPageZ);
}

float2 waterPageUvFromWorld(GPUVirtualWaterPage page, RendererInstanceData instance,
                            float3 cameraWorld, float3 worldPosition) {
    const float3 local = waterWorldToLocal(worldPosition, instance);
    const float2 origin = waterPageOrigin(page, instance, cameraWorld);
    const float span = max(waterPageSize(page), 1e-5);
    return (local.xz - origin) / span;
}


float waterStateWorldSize(GPUVirtualWaterPage page) {
    float size=asfloat(page.stateWorldSizeClass);
    return isfinite(size)&&size>1e-4?size:max(waterPageSize(page),1e-4);
}

int2 waterStateWorldCoord(GPUVirtualWaterPage page, RendererInstanceData instance, float3 cameraWorld) {
    if ((page.flags & WATER_PAGE_SPLINE) != 0u) return int2(page.localPageX, page.localPageZ);
    const float size=waterStateWorldSize(page);
    const float2 pageOrigin=waterPageOrigin(page,instance,cameraWorld);
    const float2 center=pageOrigin+float2(waterPageSize(page)*0.5);
    return int2(floor(center/size));
}

float2 waterStateUvFromWorld(GPUVirtualWaterPage page, RendererInstanceData instance,
                             GPUWaterPhysicalState physical, float3 cameraWorld, float3 worldPosition) {
    if ((page.flags & WATER_PAGE_SPLINE) != 0u) {
        const float3 local=waterWorldToLocal(worldPosition,instance);
        const float2 a=float2(page.splineStartX,page.splineStartZ);
        const float2 b=float2(page.splineEndX,page.splineEndZ);
        const float2 d=b-a;
        const float len2=max(dot(d,d),1e-6);
        const float along=saturate(dot(local.xz-a,d)/len2);
        const float2 side=normalize(float2(-d.y,d.x)+float2(1e-8,0.0));
        const float halfWidth=max(lerp(page.splineHalfWidth0,page.splineHalfWidth1,along),1e-4);
        const float across=saturate(dot(local.xz-lerp(a,b,along),side)/(2.0*halfWidth)+0.5);
        return float2(along,across);
    }
    const float3 local=waterWorldToLocal(worldPosition,instance);
    const float size=waterStateWorldSize(page);
    const float2 origin=float2(physical.ownerX,physical.ownerZ)*size;
    return (local.xz-origin)/size;
}


GPUWaterStatePhysicalRef waterInvalidStateRef() {
    GPUWaterStatePhysicalRef ref = (GPUWaterStatePhysicalRef)0;
    ref.slot = WATER_INVALID_PHYSICAL_PAGE;
    return ref;
}

// Cheap phase-delta rotation used to reconstruct previous-frame Gerstner
// samples without issuing a second transcendental sincos per wave/vertex.
// The input is range-reduced to [-pi, pi] and evaluated with an odd/even
// polynomial pair. For frame-to-frame deltas this is substantially more
// accurate than required by the motion-vector buffer while remaining stable
// for hitches and authored-object motion.
void waterSinCosApprox(float angle, out float s, out float c) {
    const float invTwoPi = 0.15915494309189535;
    float x = angle - round(angle * invTwoPi) * WATER_TWO_PI;
    const float x2 = x * x;
    s = x * (1.0 + x2 * (-0.1666666716 + x2 * (0.0083333477 + x2 * (-0.0001984090 + x2 * 0.0000027526))));
    c = 1.0 + x2 * (-0.5 + x2 * (0.0416666418 + x2 * (-0.0013888378 + x2 * 0.0000247609)));
}

bool waterStateRefValid(GPUWaterStatePhysicalRef ref) {
    return ref.slot != WATER_INVALID_PHYSICAL_PAGE && ref.slot < WATER_MAX_PHYSICAL_STATE_PAGES;
}

uint waterStateTierResolution(uint tier) {
    return tier == 0u ? 4u : (tier == 1u ? 8u : (tier == 2u ? 12u : 16u));
}

uint waterPackUpdateRate(uint period, uint phase) {
    return (period & 0xffu) | ((phase & 0xffu) << 8u);
}
uint waterUpdatePeriod(uint packed) { return max(packed & 0xffu, 1u); }
uint waterUpdatePhase(uint packed) { return (packed >> 8u) & 0xffu; }
