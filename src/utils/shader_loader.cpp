/**
 * @file shader_loader.cpp
 * @brief Implements SPIR-V loading and Vulkan shader-module lifetime management.
 */

#include "shader_loader.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace vkutil {

/**
 * @brief Takes ownership of a Vulkan shader module.
 * @param device Logical device that created @p module.
 * @param module Shader module to destroy when this object is reset or destroyed.
 */
ShaderModule::ShaderModule(VkDevice device, VkShaderModule module) noexcept
    : device_(device), module_(module) {}

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
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      module_(std::exchange(other.module_, VK_NULL_HANDLE)) {}

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
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error(
            "Couldnt open SPIR-V shader: " + path.string());
    }

    const std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) {
        throw std::runtime_error(
            "Shader SPIR-V is empty: " + path.string());
    }
    if (fileSize % static_cast<std::streamsize>(sizeof(uint32_t)) != 0) {
        throw std::runtime_error(
            "Invalid SPIR-V shader size: " + path.string());
    }

    std::vector<uint32_t> code(
        static_cast<std::size_t>(fileSize) / sizeof(uint32_t));

    file.seekg(0, std::ios::beg);
    if (!file.read(
            reinterpret_cast<char*>(code.data()),
            fileSize)) {
        throw std::runtime_error(
            "Couldnt read SPIR-V shader: " + path.string());
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
ShaderModule loadShaderModule(
    VkDevice device,
    const std::filesystem::path& path) {
    if (device == VK_NULL_HANDLE) {
        throw std::invalid_argument(
            "Couldnt create shader for null VkDevice");
    }

    const std::vector<uint32_t> code = loadSpirv(path);

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error(
            "Couldnt create VkShaderModule: " + path.string());
    }

    return {device, module};
}

}
