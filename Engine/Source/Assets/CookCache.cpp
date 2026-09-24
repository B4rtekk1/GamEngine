#include "CookCache.h"

#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifndef GAMEENGINE_COMPRESSONATOR_REVISION
#define GAMEENGINE_COMPRESSONATOR_REVISION "unknown"
#endif
#ifndef GAMEENGINE_BC7_ENCODER
#define GAMEENGINE_BC7_ENCODER "compressonator"
#endif

namespace Engine::Assets::CookCache {
    void Hash64::add(const std::span<const std::uint8_t> bytes) noexcept {
        for (const std::uint8_t byte: bytes) {
            value ^= byte;
            value *= 1099511628211ULL;
        }
    }

    void Hash64::add(const std::string_view text) noexcept {
        add({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
    }

    void Hash64::add(const std::uint64_t number) noexcept {
        std::array<std::uint8_t, 8> bytes{};
        for (unsigned i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::uint8_t>(number >> (i * 8U));
        add(bytes);
    }

    std::optional<std::uint64_t> hash_file(const std::filesystem::path &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        Hash64 hash;
        std::array<std::uint8_t, 64 * 1024> buffer{};
        while (file) {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const auto count = file.gcount();
            if (count > 0) hash.add(std::span{buffer.data(), static_cast<std::size_t>(count)});
        }
        return file.eof() ? std::optional{hash.value} : std::nullopt;
    }

    std::uint64_t texture_settings_key() noexcept {
        Hash64 hash;
        hash.add("texture-cook-settings-v1");
        hash.add(GAMEENGINE_COMPRESSONATOR_REVISION);
        hash.add(GAMEENGINE_BC7_ENCODER);
        hash.add(cookerVersion);
        hash.add(mipPolicy);
        hash.add(normalPolicy);
        hash.add(std::bit_cast<std::uint32_t>(compressionQuality));
        return hash.value;
    }

    std::uint64_t texture_key(const TextureCookKey &key) noexcept {
        Hash64 hash;
        hash.add("gtex-v1");
        hash.add(GAMEENGINE_COMPRESSONATOR_REVISION);
        hash.add(GAMEENGINE_BC7_ENCODER);
        hash.add(key.sourceHash);
        hash.add(static_cast<std::uint64_t>(key.format));
        hash.add(static_cast<std::uint64_t>(key.srgb));
        hash.add(key.cookerVersion);
        hash.add(key.mipPolicy);
        hash.add(key.normalPolicy);
        hash.add(std::bit_cast<std::uint32_t>(key.quality));
        return hash.value;
    }

    std::string hex_key(const std::uint64_t key) {
        std::ostringstream text;
        text << std::hex << std::setfill('0') << std::setw(16) << key;
        return text.str();
    }

    std::filesystem::path root_for_source(const std::filesystem::path &source) {
        for (auto parent = source.parent_path(); !parent.empty(); parent = parent.parent_path()) {
            if (parent.filename() == "Assets") return parent.parent_path() / "Library" / "DDC";
            if (parent == parent.root_path()) break;
        }
        return source.parent_path() / "Library" / "DDC";
    }

    std::filesystem::path texture_path(const std::filesystem::path &root, const std::uint64_t key) {
        const auto name = hex_key(key);
        return root / "Textures" / name.substr(0, 2) / (name + ".gtex");
    }

    namespace {
        [[nodiscard]] std::filesystem::path marker_path(const std::filesystem::path &path) {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(path, error);
            Hash64 hash;
            hash.add((error ? path : absolute).lexically_normal().generic_string());
            const auto name = hex_key(hash.value);
            return root_for_source(path) / "Manifests" / name.substr(0, 2) / (name + ".key");
        }
    }

    bool artifact_has_key(const std::filesystem::path &path, const std::uint64_t key) {
        std::ifstream marker(marker_path(path), std::ios::binary);
        std::string stored;
        return marker && (marker >> stored) && stored == hex_key(key);
    }

    bool write_artifact_key(const std::filesystem::path &path, const std::uint64_t key) {
        const auto name = marker_path(path);
        std::error_code error;
        std::filesystem::create_directories(name.parent_path(), error);
        if (error) return false;
        std::ofstream marker(name, std::ios::binary | std::ios::trunc);
        marker << hex_key(key) << '\n';
        return static_cast<bool>(marker);
    }

    bool publish(const std::filesystem::path &cached, const std::filesystem::path &output,
                 const std::uint64_t key) {
        std::error_code error;
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path(), error);
        if (error) return false;
        std::filesystem::copy_file(cached, output, std::filesystem::copy_options::overwrite_existing, error);
        return !error && write_artifact_key(output, key);
    }
} // namespace Engine::Assets::CookCache
