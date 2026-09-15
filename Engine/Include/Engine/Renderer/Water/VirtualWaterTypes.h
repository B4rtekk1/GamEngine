#pragma once

#include <array>
#include <cstdint>

namespace Engine::Water {
    inline constexpr std::uint32_t ClipLevels = 9;
    // The outermost level is replaced by an analytic horizon pass.
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
    inline constexpr std::uint32_t MaxPhysicalStatePages = 256;
    inline constexpr std::uint32_t StateGridResolution = 16;
    inline constexpr std::uint32_t StateCellsPerPage = StateGridResolution * StateGridResolution;
    inline constexpr std::uint32_t MaxStateCells = MaxPhysicalStatePages * StateCellsPerPage;
    inline constexpr std::uint32_t MaxInteractionEvents = 64;
    inline constexpr std::uint32_t InvalidPhysicalPage = 0xffffffffU;
    inline constexpr float MaxInteractionDisplacement = 0.75F;

    inline constexpr std::array<float, ClipLevels> OceanExtents{
        50.0F, 100.0F, 200.0F, 400.0F, 800.0F,
        1600.0F, 3200.0F, 6400.0F, 12800.0F,
    };


    inline constexpr std::uint32_t FarAnalyticPageId = 0x1fffU;
    inline constexpr std::uint32_t AuthoredWaterFlag = 0x10000000U;
    inline constexpr std::uint32_t FarAnalyticFlag = 0x20000000U;

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
        std::uint32_t padding{};
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
    };

    enum StitchBits : std::uint32_t {
        StitchLeft = 1U << 0U,
        StitchRight = 1U << 1U,
        StitchBottom = 1U << 2U,
        StitchTop = 1U << 3U,
    };

    /** Static world-domain record consumed by water_page_cull.slang. */
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
        // Fine-page outer edge that meets the next coarser ring. Unlike stitchMask,
        // this controls spectral Gerstner filtering rather than index topology.
        std::uint32_t spectralEdgeMask{};

        // Optional authored local-space flow used by river state advection.
        // Oceans leave this at zero and fall back to frame wind/flow maps.
        float flowX{};
        float flowZ{};
        float flowSpeed{};
        float reservedFlow{};

        // Two neighbours per edge support 2:1 clipmap transitions without a
        // global page search in the state solver. Same-LOD edges store the
        // same page in both slots. InvalidPhysicalPage means open boundary.
        std::uint32_t neighborLeft0{InvalidPhysicalPage};
        std::uint32_t neighborLeft1{InvalidPhysicalPage};
        std::uint32_t neighborRight0{InvalidPhysicalPage};
        std::uint32_t neighborRight1{InvalidPhysicalPage};
        std::uint32_t neighborBottom0{InvalidPhysicalPage};
        std::uint32_t neighborBottom1{InvalidPhysicalPage};
        std::uint32_t neighborTop0{InvalidPhysicalPage};
        std::uint32_t neighborTop1{InvalidPhysicalPage};
    };
    static_assert(sizeof(GPUVirtualWaterPage) == 128);

    /** One visible page. Buffers are laid out as [drawBin][VisiblePagesPerBin]. */
    struct alignas(16) GPUVisibleWaterPage final {
        std::uint32_t pageIndex{};
        std::uint32_t waveCandidateMask{};
        std::uint32_t packedQuality{};
        std::uint32_t instanceIndex{};
    };
    static_assert(sizeof(GPUVisibleWaterPage) == 16);

    /** Static data used to construct one VkDrawIndexedIndirectCommand per draw bin. */
    struct alignas(16) GPUWaterDrawTemplate final {
        std::uint32_t indexCount{};
        std::uint32_t firstIndex{};
        std::int32_t vertexOffset{};
        std::uint32_t firstInstanceBase{};
    };
    static_assert(sizeof(GPUWaterDrawTemplate) == 16);

    /** Persistent per-page temporal state. One buffer is retained for every frame slot. */
    struct alignas(16) GPUWaterPageHistory final {
        std::uint32_t packedQuality{};
        std::uint32_t visibleAge{};
        float previousImportance{};
        float reserved{};
    };
    static_assert(sizeof(GPUWaterPageHistory) == 16);


    struct alignas(16) GPUWaterCullConfig final {
        std::uint32_t pageCount{};
        std::uint32_t drawBinCount{};
        std::uint32_t visiblePagesPerBin{VisiblePagesPerBin};
        std::uint32_t enableHiZ{1U};
        float reflectionVarianceScale{2.0F};
        float highQualityPixels{160.0F};
        float mediumQualityPixels{48.0F};
        float lowQualityPixels{10.0F};
    };
    static_assert(sizeof(GPUWaterCullConfig) == 32);


    /** One bounded-cache physical state slot shared by foam and local interaction shading. */
    struct alignas(16) GPUWaterPhysicalState final {
        float foam{};
        float interaction{};
        float interactionVelocity{};
        float inactiveSeconds{};
    };
    static_assert(sizeof(GPUWaterPhysicalState) == 16);

    /** One node of the persistent 16x16 local-state grid owned by a physical page. */
    struct alignas(16) GPUWaterStateCell final {
        float height{};
        float velocity{};
        float foam{};
        float reserved{};
    };
    static_assert(sizeof(GPUWaterStateCell) == 16);

    /** World-space disturbance consumed by the sparse state simulation. */
    struct alignas(16) GPUWaterInteractionEvent final {
        float worldX{};
        float worldZ{};
        float radius{1.0F};
        float strength{1.0F};
    };
    static_assert(sizeof(GPUWaterInteractionEvent) == 16);

    /** Per-frame underwater post-process parameters. */
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
    };
    static_assert(sizeof(GPUWaterPageStats) == 16);

    [[nodiscard]] constexpr std::uint32_t reflectionTier(const std::uint32_t packed) noexcept {
        return packed & 3U;
    }
    [[nodiscard]] constexpr std::uint32_t refractionTier(const std::uint32_t packed) noexcept {
        return (packed >> 2U) & 3U;
    }
    [[nodiscard]] constexpr std::uint32_t foamTier(const std::uint32_t packed) noexcept {
        return (packed >> 4U) & 3U;
    }
}
