#pragma once

#include "Engine/Renderer/Water/WaterRenderWorld.h"

#include <array>
#include <cstdint>
#include <functional>

namespace Engine::Water {
    inline constexpr std::uint32_t ClipLevels = 9;
    inline constexpr std::uint32_t GeometryClipLevels = ClipLevels - 1U;
    inline constexpr std::uint32_t PagesPerAxis = 8;
    inline constexpr std::uint32_t PageCells = 8;
    inline constexpr std::uint32_t PageVertexAxis = PageCells + 1;
    inline constexpr std::uint32_t ClipmapResolution = PagesPerAxis * PageCells;
    inline constexpr std::uint32_t VirtualSlotsPerBody = ClipLevels * PagesPerAxis * PagesPerAxis;
    inline constexpr std::uint32_t ActivePagesPerBody = 64U + (ClipLevels - 1U) * 48U;
    inline constexpr std::uint32_t StitchVariantCount = 16;
    inline constexpr std::uint32_t MaxVirtualWaterBodies = 16;
    inline constexpr std::uint32_t MaxPages = MaxVirtualWaterBodies * (ActivePagesPerBody + 1U);
    inline constexpr std::uint32_t MaxDrawBins = MaxVirtualWaterBodies * StitchVariantCount;
    inline constexpr std::uint32_t VisiblePagesPerBin = ActivePagesPerBody;
    inline constexpr std::uint32_t MaxVisiblePageSlots = MaxDrawBins * VisiblePagesPerBin;

    // Persistent state intentionally uses a world-stable identity.  The current
    // baseline stores every physical slot as a 16x16 backing tile; resolution
    // tiers select a 4/8/12/16 active sub-grid, preserving state in-place during
    // promotion/demotion and avoiding an extra copy/retirement path.
    inline constexpr std::uint32_t MaxPhysicalStatePages = 256;
    inline constexpr std::uint32_t StateGridResolution = 16;
    inline constexpr std::uint32_t StateCellsPerPage = StateGridResolution * StateGridResolution;
    inline constexpr std::uint32_t MaxStateCells = MaxPhysicalStatePages * StateCellsPerPage;
    inline constexpr std::array<std::uint32_t, 4> StateTierResolution{4U, 8U, 12U, 16U};
    inline constexpr std::uint32_t MaxInteractionEvents = 64;
    inline constexpr std::uint32_t InvalidPhysicalPage = 0xffffffffU;
    inline constexpr std::uint32_t WaterSimpleShadingFlag = 0x08000000U;
    inline constexpr float MaxInteractionDisplacement = 0.75F;

    inline constexpr std::array<float, ClipLevels> OceanExtents{
        50.0F, 100.0F, 200.0F, 400.0F, 800.0F,
        1600.0F, 3200.0F, 6400.0F, 12800.0F,
    };

    struct WaterPageKey final {
        WaterBodyId body{};
        std::uint32_t level{};
        std::int32_t worldPageX{};
        std::int32_t worldPageZ{};
        [[nodiscard]] constexpr bool operator==(const WaterPageKey&) const noexcept = default;
    };

    struct PhysicalWaterPageHandle final {
        std::uint32_t slot{InvalidPhysicalPage};
        std::uint32_t generation{};
        [[nodiscard]] constexpr bool valid() const noexcept { return slot != InvalidPhysicalPage; }
    };

    struct WaterStatePageKey final {
        WaterBodyId body{};
        std::int32_t x{};
        std::int32_t z{};
        [[nodiscard]] constexpr bool operator==(const WaterStatePageKey&) const noexcept = default;
    };

    struct WaterStatePhysicalRef final {
        std::uint32_t tier{};
        std::uint32_t slot{InvalidPhysicalPage};
        std::uint32_t generation{};
        std::uint32_t reserved{};
        [[nodiscard]] constexpr bool valid() const noexcept { return slot != InvalidPhysicalPage; }
    };
    static_assert(sizeof(WaterStatePhysicalRef) == 16);

    struct WaterPageKeyHash final {
        [[nodiscard]] std::size_t operator()(const WaterPageKey& key) const noexcept {
            std::size_t h = key.body.index;
            const auto mix = [&h](const std::uint64_t value) {
                h ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
            };
            mix(key.body.generation); mix(key.level);
            mix(static_cast<std::uint32_t>(key.worldPageX));
            mix(static_cast<std::uint32_t>(key.worldPageZ));
            return h;
        }
    };

    enum class WaterCoverageClass : std::uint32_t { Dry = 0U, Wet = 1U, Partial = 2U };

    struct WaterExecutionMetrics final {
        float projectedCoverage{};
        float projectedPixelCount{};
        float occlusionPotential{};
        float interactionImportance{};
        std::uint32_t estimatedVisiblePages{};
        float predictedSimpleGpuMs{};
        float predictedVirtualGpuMs{};
    };

    struct WaterFrameBudget final {
        float targetGpuMs{0.8F};
        float qualityScale{1.0F};
        std::uint32_t maxFullRateSamples{512U * 1024U};
        std::uint32_t maxReflectionWork{2U * 1024U * 1024U};
        std::uint32_t maxRefractionWork{1024U * 1024U};
        std::uint32_t maxStateSimulationCells{96U * StateCellsPerPage};
        std::uint32_t maxHighGeometryPages{128U};
        std::uint32_t budgetSaturated{};
        std::uint32_t reserved{};
    };

    struct WaterDynamicThresholds final {
        std::uint32_t high{192U};
        std::uint32_t medium{112U};
        std::uint32_t cheap{40U};
        std::uint32_t reserved{};
    };

    struct WaterRayBudget final {
        std::uint32_t reflectionWorkUnits{2U * 1024U * 1024U};
        std::uint32_t refractionWorkUnits{1024U * 1024U};
        std::uint32_t reflectionUsed{};
        std::uint32_t refractionUsed{};
    };

    struct WaterUpdateRate final {
        std::uint8_t period{1U};
        std::uint8_t phase{};
        std::uint16_t reserved{};
    };

    inline constexpr std::uint32_t FarAnalyticPageId = 0x1fffU;
    inline constexpr std::uint32_t AuthoredWaterFlag = 0x10000000U;
    inline constexpr std::uint32_t FarAnalyticFlag = 0x20000000U;
    inline constexpr std::uint32_t WaterReactiveFlag = 0x40000000U;
    inline constexpr std::uint32_t WaterValidFlag = 0x80000000U;

    struct alignas(16) GPUFarOceanBody final {
        std::uint32_t instanceIndex{};
        float startDistance{10000.0F};
        float normalCellSize{6.25F};
        std::uint32_t waveMask{0xffU};
    };
    static_assert(sizeof(GPUFarOceanBody) == 16);

    struct alignas(16) GPUFarOceanConfig final {
        std::uint32_t bodyCount{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
        std::array<GPUFarOceanBody, MaxVirtualWaterBodies> bodies{};
    };
    static_assert(sizeof(GPUFarOceanConfig) == 16 + 16 * MaxVirtualWaterBodies);

    struct alignas(16) GPUAuthoredWaterBody final {
        std::uint32_t instanceIndex{};
        std::uint32_t statePageIndex{InvalidPhysicalPage};
        std::uint32_t bodyType{};
        std::uint32_t executionFlags{};
    };
    static_assert(sizeof(GPUAuthoredWaterBody) == 16);

    struct alignas(16) GPUAuthoredWaterConfig final {
        std::uint32_t bodyCount{};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
        std::array<GPUAuthoredWaterBody, MaxVirtualWaterBodies> bodies{};
    };
    static_assert(sizeof(GPUAuthoredWaterConfig) == 16 + 16 * MaxVirtualWaterBodies);

    enum PageFlagBits : std::uint32_t {
        PageActive = 1U << 0U,
        PageInteraction = 1U << 1U,
        PageFoamResident = 1U << 2U,
        PageNewlyVisible = 1U << 3U,
        PageStateOnly = 1U << 4U,
        PageShoreline = 1U << 5U,
        PageSparseState = 1U << 6U,
        PageCameraRelative = 1U << 7U,
        PageSpline = 1U << 8U,
    };

    enum WaterExecutionFlagBits : std::uint32_t {
        ExecutionBudgetedShading = 1U << 0U,
        ExecutionSparseState = 1U << 1U,
        ExecutionSimpleShading = 1U << 2U,
    };

    enum StitchBits : std::uint32_t {
        StitchLeft = 1U << 0U,
        StitchRight = 1U << 1U,
        StitchBottom = 1U << 2U,
        StitchTop = 1U << 3U,
    };

    /** Static clip-slot record. World origin/identity is derived from the current snapped camera. */
    struct alignas(16) GPUVirtualWaterPage final {
        float originX{};
        float originZ{};
        float baseHeight{};
        float cellSize{};

        float minX{};
        float minZ{};
        float maxX{};
        float maxZ{};

        float verticalBound{};
        float horizontalBound{};
        float unresolvedSlopeVariance{};
        float reservedFloat{};

        std::uint32_t level{};
        std::uint32_t stitchMask{};
        std::uint32_t waveCandidateMask{};
        std::uint32_t flags{};

        std::uint32_t instanceIndex{};
        std::uint32_t drawBin{};
        std::uint32_t bodyIndex{};
        std::uint32_t spectralEdgeMask{};

        float flowX{};
        float flowZ{};
        float flowSpeed{};
        float reservedFlow{};

        std::uint32_t neighborLeft0{InvalidPhysicalPage};
        std::uint32_t neighborLeft1{InvalidPhysicalPage};
        std::uint32_t neighborRight0{InvalidPhysicalPage};
        std::uint32_t neighborRight1{InvalidPhysicalPage};
        std::uint32_t neighborBottom0{InvalidPhysicalPage};
        std::uint32_t neighborBottom1{InvalidPhysicalPage};
        std::uint32_t neighborTop0{InvalidPhysicalPage};
        std::uint32_t neighborTop1{InvalidPhysicalPage};

        std::uint32_t bodyIdIndex{};
        std::uint32_t bodyIdGeneration{};
        std::int32_t localPageX{};
        std::int32_t localPageZ{};

        std::uint32_t spectrumRevision{};
        std::uint32_t coverageClass{static_cast<std::uint32_t>(WaterCoverageClass::Wet)};
        std::uint32_t cellCoverageMaskLow{0xffffffffU};
        std::uint32_t cellCoverageMaskHigh{0xffffffffU};

        float unresolvedSlopeXX{};
        float unresolvedSlopeXZ{};
        float unresolvedSlopeZZ{};
        float normalDetailImportance{};

        // Stable SplinePages mapping. Ignored for ordinary clipmap pages.
        float splineStartX{};
        float splineStartZ{};
        float splineEndX{};
        float splineEndZ{};
        float splineHalfWidth0{};
        float splineHalfWidth1{};
        std::uint32_t executionFlags{ExecutionBudgetedShading | ExecutionSparseState};
        std::uint32_t stateWorldSizeClass{};
    };
    static_assert(sizeof(GPUVirtualWaterPage) == 208);

    struct alignas(16) GPUVisibleWaterPage final {
        std::uint32_t pageIndex{};
        std::uint32_t waveCandidateMask{};
        std::uint32_t packedQuality{};
        std::uint32_t instanceIndex{};
    };
    static_assert(sizeof(GPUVisibleWaterPage) == 16);

    struct alignas(16) GPUWaterDrawTemplate final {
        std::uint32_t indexCount{};
        std::uint32_t firstIndex{};
        std::int32_t vertexOffset{};
        std::uint32_t firstInstanceBase{};
    };
    static_assert(sizeof(GPUWaterDrawTemplate) == 16);

    struct alignas(16) GPUWaterPageHistory final {
        std::uint32_t packedQuality{};
        std::uint32_t visibleAge{};
        std::uint32_t framesInTier{};
        std::uint32_t occludedFrames{};
        float previousImportance{};
        std::int32_t worldPageX{};
        std::int32_t worldPageZ{};
        std::uint32_t reserved{};
    };
    static_assert(sizeof(GPUWaterPageHistory) == 32);

    struct alignas(16) GPUWaterCullConfig final {
        std::uint32_t pageCount{};
        std::uint32_t drawBinCount{};
        std::uint32_t visiblePagesPerBin{VisiblePagesPerBin};
        std::uint32_t enableHiZ{1U};
        float reflectionVarianceScale{2.0F};
        float highQualityPixels{160.0F};
        float mediumQualityPixels{48.0F};
        float lowQualityPixels{10.0F};
        float qualityScale{1.0F};
        std::uint32_t currentFrame{};
        std::uint32_t occlusionGraceFrames{2U};
        std::uint32_t occludedFramesRequired{2U};
        std::uint32_t maxHighGeometryPages{128U};
        std::uint32_t padding0{};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
    };
    static_assert(sizeof(GPUWaterCullConfig) == 64);

    struct alignas(16) GPUWaterStatePhysicalRef final {
        std::uint32_t tier{};
        std::uint32_t slot{InvalidPhysicalPage};
        std::uint32_t generation{};
        std::uint32_t reserved{};
    };
    static_assert(sizeof(GPUWaterStatePhysicalRef) == 16);

    /** Metadata for one bounded physical slot. Owner is a stable world-space state key. */
    struct alignas(16) GPUWaterPhysicalState final {
        float foam{};
        float interaction{};
        float maxVelocity{};
        float inactiveSeconds{};

        std::uint32_t ownerBodyIndex{InvalidPhysicalPage};
        std::uint32_t ownerBodyGeneration{};
        std::int32_t ownerX{};
        std::int32_t ownerZ{};

        std::uint32_t generation{1U};
        std::uint32_t tier{};
        std::uint32_t pinCount{};
        std::uint32_t flags{};

        std::uint32_t lastVisibleFrame{};
        std::uint32_t lastSimulatedFrame{};
        std::uint32_t lastInteractionFrame{};
        std::uint32_t updateRatePacked{1U};
    };
    static_assert(sizeof(GPUWaterPhysicalState) == 64);

    /** height perturbation + two-component world/tangent velocity + persistent foam. */
    struct alignas(16) GPUWaterStateCell final {
        float height{};
        float velocityX{};
        float velocityZ{};
        float foam{};
    };
    static_assert(sizeof(GPUWaterStateCell) == 16);

    struct alignas(16) GPUWaterInteractionEvent final {
        float worldX{};
        float worldZ{};
        float radius{1.0F};
        float strength{1.0F};
    };
    static_assert(sizeof(GPUWaterInteractionEvent) == 16);

    struct alignas(16) GPUWaterUnderwaterConfig final {
        float shallowR{}, shallowG{}, shallowB{}, immersion{};
        float deepR{}, deepG{}, deepB{}, maxDepth{20.0F};
        float absorptionR{}, absorptionG{}, absorptionB{}, time{};
        float scatteringR{}, scatteringG{}, scatteringB{}, surfaceHeight{};
        std::uint32_t active{};
        std::uint32_t causticsEnabled{};
        std::uint32_t materialIndex{};
        std::uint32_t padding{};
    };
    static_assert(sizeof(GPUWaterUnderwaterConfig) == 80);

    struct alignas(16) GPUWaterPageStats final {
        std::uint32_t visiblePages{};
        std::uint32_t frustumRejected{};
        std::uint32_t hiZRejected{};
        std::uint32_t distanceRejected{};
        std::uint32_t newlyVisible{};
        std::uint32_t highQualityPages{};
        std::uint32_t stateResidentPages{};
        std::uint32_t budgetDrops{};
    };
    static_assert(sizeof(GPUWaterPageStats) == 32);

    struct alignas(16) GPUWaterFrameBudget final {
        float targetGpuMs{0.8F};
        float measuredGpuMs{};
        float qualityScale{1.0F};
        float feedbackGain{0.08F};
        std::uint32_t maxFullRateSamples{512U * 1024U};
        std::uint32_t maxReflectionWork{2U * 1024U * 1024U};
        std::uint32_t maxRefractionWork{1024U * 1024U};
        std::uint32_t maxStateSimulationCells{96U * StateCellsPerPage};
        std::uint32_t reflectionUsed{};
        std::uint32_t refractionUsed{};
        std::uint32_t fullRateUsed{};
        std::uint32_t stateSimulationUsed{};
        std::uint32_t noRayReflection{};
        std::uint32_t noRayRefraction{};
        std::uint32_t rayRefreshCounter{};
        std::uint32_t budgetSaturated{};
    };
    static_assert(sizeof(GPUWaterFrameBudget) == 64);

    struct alignas(16) GPUWaterTileSettings final {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t tilesX{};
        std::uint32_t maxTiles{};
        std::uint32_t denseMode{1U};
        std::uint32_t frameIndex{};
        std::uint32_t highThreshold{192U};
        std::uint32_t mediumThreshold{112U};
        std::uint32_t cheapThreshold{40U};
        std::uint32_t candidateTileCount{};
        std::uint32_t historyValid{};
        std::uint32_t padding0{};
        float temporalBlend{0.18F};
        float temporalMissBlend{0.42F};
        std::uint32_t padding1{};
        std::uint32_t padding2{};
    };
    static_assert(sizeof(GPUWaterTileSettings) == 64);

    [[nodiscard]] constexpr std::uint32_t reflectionTier(const std::uint32_t packed) noexcept { return packed & 3U; }
    [[nodiscard]] constexpr std::uint32_t refractionTier(const std::uint32_t packed) noexcept { return (packed >> 2U) & 3U; }
    [[nodiscard]] constexpr std::uint32_t foamTier(const std::uint32_t packed) noexcept { return (packed >> 4U) & 3U; }
}
