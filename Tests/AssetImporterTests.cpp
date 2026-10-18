#include "Editor/AssetImporter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <miniz.h>

namespace {
class AssetImporterTest : public testing::Test {
  protected:
    std::filesystem::path root;
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               ("ge-import-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void archive(const std::initializer_list<const char*> names) {
        mz_zip_archive zip{};
        ASSERT_TRUE(mz_zip_writer_init_file(&zip, (root / "assets.zip").string().c_str(), 0));
        for (const auto* name : names) {
            std::string writableName{name};
            // Miniz's writer disallows absolute names; patch an equal-length
            // filename below to exercise archives produced by other tools.
            if (writableName.starts_with('/'))
                writableName.front() = '_';
            EXPECT_TRUE(mz_zip_writer_add_mem(&zip, writableName.c_str(), "asset", 5, MZ_DEFAULT_COMPRESSION));
        }
        EXPECT_TRUE(mz_zip_writer_finalize_archive(&zip));
        EXPECT_TRUE(mz_zip_writer_end(&zip));
        std::ifstream input{root / "assets.zip", std::ios::binary};
        std::string bytes{std::istreambuf_iterator<char>{input}, {}};
        input.close();
        for (const auto* name : names) {
            if (*name != '/')
                continue;
            std::string writableName{name};
            writableName.front() = '_';
            for (auto offset = bytes.find(writableName); offset != std::string::npos;
                 offset = bytes.find(writableName, offset + writableName.size()))
                bytes[offset] = '/';
        }
        std::ofstream{root / "assets.zip", std::ios::binary | std::ios::trunc}.write(bytes.data(), bytes.size());
    }
};

TEST_F(AssetImporterTest, ImportsUnicodeFilesAndPreservesDuplicates) {
    const auto name = std::filesystem::path{u8"żółw.txt"};
    std::ofstream{root / name} << "content";
    std::string error;
    ASSERT_TRUE(Editor::AssetImporter::import(root / name, root / "output", error)) << error;
    ASSERT_TRUE(Editor::AssetImporter::import(root / name, root / "output", error)) << error;
    EXPECT_TRUE(std::filesystem::exists(root / "output" / name));
    EXPECT_TRUE(std::filesystem::exists(root / "output" / std::filesystem::path{u8"żółw (1).txt"}));
}

TEST_F(AssetImporterTest, ExtractsUnicodeAndWindowsSeparators) {
    archive({"models\\nested\\mesh.txt", "textures/żółw.txt"});
    std::string error;
    ASSERT_TRUE(Editor::AssetImporter::import(root / "assets.zip", root / "output", error)) << error;
    EXPECT_TRUE(std::filesystem::exists(root / "output/models/nested/mesh.txt"));
    EXPECT_TRUE(std::filesystem::exists(root / "output/textures" / std::filesystem::path{u8"żółw.txt"}));
    for (const auto& entry : std::filesystem::directory_iterator(root / "output"))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".import-"));
}

TEST_F(AssetImporterTest, RejectsTraversalBeforeCopyingAnyFiles) {
    for (const auto* unsafe : {"../escaped.txt", "..\\escaped.txt", "/absolute.txt", "C:\\escaped.txt"}) {
        std::filesystem::remove_all(root / "output");
        archive({"valid.txt", unsafe});
        std::string error;
        EXPECT_FALSE(Editor::AssetImporter::import(root / "assets.zip", root / "output", error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(std::filesystem::is_empty(root / "output"));
        EXPECT_FALSE(std::filesystem::exists(root / "escaped.txt"));
    }
}

TEST_F(AssetImporterTest, RejectsImportIntoSourceDescendant) {
    std::filesystem::create_directories(root / "source");
    std::string error;
    EXPECT_FALSE(Editor::AssetImporter::import(root / "source", root / "source/output", error));
    EXPECT_FALSE(std::filesystem::exists(root / "source/output"));
}

#ifndef _WIN32
TEST_F(AssetImporterTest, RejectsSymbolicLinks) {
    std::ofstream{root / "file.txt"} << "content";
    std::filesystem::create_symlink(root / "file.txt", root / "link.txt");
    std::string error;
    EXPECT_FALSE(Editor::AssetImporter::import(root / "link.txt", root / "output", error));
}
#endif
} // namespace
