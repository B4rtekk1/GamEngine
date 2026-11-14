#include "Engine/Assets/Gtex.h"
#include "Engine/Assets/TextureCooker.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

namespace {
// A 4x4 DDS whose payload is `blocks` (one block per mip, 4x4 -> 2x2 -> 1x1).
void writeDds(const std::filesystem::path& path, const char fourcc[4], const bool mipmaps,
              const std::vector<std::uint8_t>& blocks) {
    std::array<std::uint8_t, 128> header{};
    const auto put = [&](const std::size_t offset, const std::uint32_t value) {
        std::memcpy(header.data() + offset, &value, sizeof(value));
    };
    std::memcpy(header.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1U | 0x2U | 0x4U | 0x1000U | (mipmaps ? 0x20000U : 0U));
    put(12, 4); // height
    put(16, 4); // width
    put(28, mipmaps ? 3U : 1U);
    put(76, 32);
    put(80, 0x4U); // DDPF_FOURCC
    std::memcpy(header.data() + 84, fourcc, 4);
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    file.write(reinterpret_cast<const char*>(blocks.data()), static_cast<std::streamsize>(blocks.size()));
}

std::vector<std::uint8_t> payload(const Engine::Assets::GtexTexture& texture) {
    std::ifstream file{texture.path, std::ios::binary};
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{file}, {}};
    return {bytes.begin() + static_cast<std::ptrdiff_t>(texture.payloadOffset), bytes.end()};
}

class TextureCookerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               ("gameengine-texture-cooker-" + std::string{::testing::UnitTest::GetInstance()->current_test_info()->name()});
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "cache");
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};
} // namespace

TEST_F(TextureCookerTest, CopiesCompressedDdsMipChainWithoutReencoding) {
    // DXT1 blocks: two RGB565 endpoints and 32 bits of indices each.
    std::vector<std::uint8_t> blocks(24);
    for (std::size_t index = 0; index < blocks.size(); ++index)
        blocks[index] = static_cast<std::uint8_t>(index * 11 + 3);
    writeDds(root / "albedo.dds", "DXT1", true, blocks);

    using namespace Engine::Assets;
    EXPECT_EQ(cook_source_texture_cached(root / "albedo.dds", root / "albedo.gtex", TextureFormat::BC7_SRGB,
                                         root / "cache"),
              TextureCookResult::Cooked);
    const auto cooked = load_gtex(root / "albedo.gtex");
    ASSERT_TRUE(cooked.has_value());
    EXPECT_EQ(cooked->format, TextureFormat::BC1_SRGB);
    ASSERT_EQ(cooked->mips.size(), 3U);
    EXPECT_EQ(cooked->mips[2].width, 1U);
    EXPECT_EQ(payload(*cooked), blocks);
    // The passthrough artifact is recognised as current.
    EXPECT_TRUE(current_source_texture(root / "albedo.dds", root / "albedo.gtex", TextureFormat::BC7_SRGB));
    EXPECT_EQ(cook_source_texture_cached(root / "albedo.dds", root / "albedo.gtex", TextureFormat::BC7_SRGB,
                                         root / "cache"),
              TextureCookResult::Reused);
}

TEST_F(TextureCookerTest, ReencodesDdsWithoutMipChainOrForOtherChannels) {
    using namespace Engine::Assets;
    writeDds(root / "flat.dds", "DXT1", false, std::vector<std::uint8_t>(8, 0x55));
    ASSERT_EQ(cook_source_texture_cached(root / "flat.dds", root / "flat.gtex", TextureFormat::BC7_UNORM,
                                         root / "cache"),
              TextureCookResult::Cooked);
    EXPECT_EQ(load_gtex(root / "flat.gtex")->format, TextureFormat::BC7_UNORM);

    // Two-channel BC5 serves normal maps only.
    writeDds(root / "normal.dds", "ATI2", true, std::vector<std::uint8_t>(48, 0x80));
    ASSERT_EQ(cook_source_texture_cached(root / "normal.dds", root / "normal.gtex", TextureFormat::BC5_UNORM,
                                         root / "cache"),
              TextureCookResult::Cooked);
    EXPECT_EQ(load_gtex(root / "normal.gtex")->format, TextureFormat::BC5_UNORM);
    ASSERT_EQ(cook_source_texture_cached(root / "normal.dds", root / "normal-as-color.gtex",
                                         TextureFormat::BC7_SRGB, root / "cache"),
              TextureCookResult::Cooked);
    EXPECT_EQ(load_gtex(root / "normal-as-color.gtex")->format, TextureFormat::BC7_SRGB);
}
