/**
 * @file shader_loader.cpp
 * @brief Implements SPIR-V loading and Vulkan shader-module lifetime management.
 */

#include "Engine/Renderer/shader_loader.h"

#include "Engine/Assets/AssetManager.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Engine {
namespace Vkutil {
/**
 * @brief Takes ownership of a Vulkan shader module.
 * @param device Logical device that created @p module.
 * @param module Shader module to destroy when this object is reset or destroyed.
 */
ShaderModule::ShaderModule(VkDevice device, VkShaderModule module) noexcept : device_(device), module_(module) {}

/**
 * @brief Releases the owned Vulkan shader module, if any.
 */
ShaderModule::~ShaderModule() {
    reset();
}

/**
 * @brief Transfers shader-module ownership from another instance.
 * @param other Instance relinquishing ownership.
 */
ShaderModule::ShaderModule(ShaderModule&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)), module_(std::exchange(other.module_, VK_NULL_HANDLE)),
      vertexInputLocations_(std::move(other.vertexInputLocations_)) {}

/**
 * @brief Replaces the owned shader module with one transferred from another instance.
 * @param other Instance relinquishing ownership.
 * @return Reference to this instance.
 */
ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept {
    if (this != &other) {
        reset();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        module_ = std::exchange(other.module_, VK_NULL_HANDLE);
        vertexInputLocations_ = std::move(other.vertexInputLocations_);
    }
    return *this;
}

/**
 * @brief Destroys the owned shader module and clears the stored handles.
 */
void ShaderModule::reset() noexcept {
    if (device_ != VK_NULL_HANDLE && module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, module_, nullptr);
    }
    module_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
}

/**
 * @brief Reads a SPIR-V binary file into 32-bit words.
 * @param path Path to the compiled SPIR-V shader.
 * @return The shader bytecode represented as SPIR-V words.
 * @throws std::runtime_error If the file cannot be opened, is empty, has an
 *         invalid size, or cannot be read completely.
 */
std::vector<uint32_t> loadSpirv(const std::filesystem::path& path) {
    std::filesystem::path resolvedPath = path;
    if (path.is_relative() && !std::filesystem::exists(path)) {
        if (const char* const basePath = SDL_GetBasePath()) {
            const std::filesystem::path executablePath = std::filesystem::path{basePath} / path;
            if (std::filesystem::exists(executablePath)) {
                resolvedPath = executablePath;
            }
        }
    }

    std::ifstream file(resolvedPath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Couldnt open SPIR-V shader: " + path.string());
    }

    file.seekg(0, std::ios::end);
    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        throw std::runtime_error("Shader SPIR-V is empty: " + path.string());
    }
    if (fileSize % static_cast<std::streamsize>(sizeof(uint32_t)) != 0) {
        throw std::runtime_error("Invalid SPIR-V shader size: " + path.string());
    }

    std::vector<uint32_t> code(static_cast<std::size_t>(fileSize) / sizeof(uint32_t));

    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(code.data()), fileSize)) {
        throw std::runtime_error("Couldnt read SPIR-V shader: " + path.string());
    }

    return code;
}

/**
 * @brief Creates a Vulkan shader module from a SPIR-V binary file.
 * @param device Logical Vulkan device used to create the module.
 * @param path Path to the compiled SPIR-V shader.
 * @return An owning wrapper around the created shader module.
 * @throws std::invalid_argument If @p device is VK_NULL_HANDLE.
 * @throws std::runtime_error If the SPIR-V cannot be loaded or Vulkan fails to
 *         create the shader module.
 */
std::optional<std::vector<uint32_t>> reflectVertexInputLocations(const std::vector<uint32_t>& code,
                                                                  const std::string_view entryPoint) {
    constexpr uint32_t magic = 0x07230203U;
    if (code.size() < 5 || code[0] != magic)
        return std::nullopt;
    std::vector<uint32_t> interface;
    bool foundEntry = false;
    std::unordered_map<uint32_t, uint32_t> locations;  // variable id -> Location
    std::unordered_map<uint32_t, uint32_t> pointee;    // pointer type id -> pointee type id
    std::unordered_set<uint32_t> compositeTypes;       // matrix / array / struct ids
    std::vector<std::pair<uint32_t, uint32_t>> inputs; // variable id, pointer type id
    for (std::size_t offset = 5; offset < code.size();) {
        const uint32_t word = code[offset];
        const uint32_t opcode = word & 0xFFFFU;
        const uint32_t length = word >> 16U;
        if (length == 0 || offset + length > code.size())
            return std::nullopt;
        const uint32_t* operands = code.data() + offset + 1;
        switch (opcode) {
        case 15: { // OpEntryPoint: model, id, name..., interface...
            if (operands[0] != 0 /* Vertex */)
                break;
            const char* name = reinterpret_cast<const char*>(&operands[2]);
            const std::size_t nameBytes = (length - 3) * sizeof(uint32_t);
            const std::size_t nameLength = strnlen(name, nameBytes);
            if (std::string_view{name, nameLength} == entryPoint) {
                foundEntry = true;
                const std::size_t nameWords = nameLength / sizeof(uint32_t) + 1;
                for (std::size_t i = 2 + nameWords; i + 1 < length; ++i)
                    interface.push_back(operands[i]);
            }
            break;
        }
        case 71: // OpDecorate: target, decoration, literal
            if (length >= 4 && operands[1] == 30 /* Location */)
                locations[operands[0]] = operands[2];
            break;
        case 24: // OpTypeMatrix
        case 28: // OpTypeArray
        case 30: // OpTypeStruct
            compositeTypes.insert(operands[0]);
            break;
        case 32: // OpTypePointer: id, storage class, type
            pointee[operands[0]] = operands[2];
            break;
        case 59: // OpVariable: result type, id, storage class
            if (operands[2] == 1 /* Input */)
                inputs.emplace_back(operands[1], operands[0]);
            break;
        default:
            break;
        }
        offset += length;
    }
    if (!foundEntry)
        return std::nullopt;
    std::vector<uint32_t> result;
    for (const auto& [variable, pointerType] : inputs) {
        if (std::ranges::find(interface, variable) == interface.end())
            continue;
        if (compositeTypes.contains(pointee[pointerType]))
            return std::nullopt; // spans several locations; do not guess
        if (const auto found = locations.find(variable); found != locations.end())
            result.push_back(found->second);
    }
    return result;
}

ShaderModule loadShaderModule(VkDevice device, const std::filesystem::path& path) {
    if (device == VK_NULL_HANDLE) {
        throw std::invalid_argument("Couldnt create shader for null VkDevice");
    }

    const std::vector<uint32_t> code = loadSpirv(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Couldnt create VkShaderModule: " + path.string());
    }

    ShaderModule result{device, module};
    result.setVertexInputLocations(reflectVertexInputLocations(code, "vertexMain"));
    return result;
}

ShaderModule loadShaderModule(const VkDevice device, Assets::AssetManager& assets, const std::filesystem::path& path) {
    if (device == VK_NULL_HANDLE) {
        throw std::invalid_argument("Couldnt create shader for null VkDevice");
    }
    // SPIR-V files may have been replaced by the editor while the renderer is
    // alive.  This only reloads when their write timestamp changed and keeps
    // the ordinary cached path fast.
    const auto asset = assets.load_if_changed<Assets::BinaryAsset>(path, Assets::AssetType::Binary);
    if (!asset || asset->bytes.empty() || asset->bytes.size() % sizeof(std::uint32_t) != 0) {
        throw std::runtime_error("Invalid SPIR-V shader asset: " + path.string());
    }

    const std::size_t wordCount = asset->bytes.size() / sizeof(std::uint32_t);
    std::vector<std::uint32_t> code(wordCount);
    std::memcpy(code.data(), asset->bytes.data(), asset->bytes.size());
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size() * sizeof(std::uint32_t);
    createInfo.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Couldnt create VkShaderModule: " + path.string());
    }
    ShaderModule result{device, module};
    result.setVertexInputLocations(reflectVertexInputLocations(code, "vertexMain"));
    return result;
}
} // namespace Vkutil
} // namespace Engine
