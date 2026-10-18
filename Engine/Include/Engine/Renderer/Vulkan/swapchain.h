#pragma once

#include "vulkan_device.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <vector>
#include <vulkan/vulkan.h>

namespace Engine {
class Swapchain final {
  public:
    Swapchain() = default;

    ~Swapchain();

    Swapchain(const Swapchain&) = delete;

    Swapchain& operator=(const Swapchain&) = delete;

    Swapchain(Swapchain&&) = delete;

    Swapchain& operator=(Swapchain&&) = delete;

    void create(SDL_Window* window, VkSurfaceKHR surface, const VulkanDevice& device);

    void recreate();

    void logPresentationState() const;

    void destroy() noexcept;

    [[nodiscard]] VkSwapchainKHR handle() const noexcept {
        return swapchain_;
    }

    [[nodiscard]] VkFormat format() const noexcept {
        return imageFormat_;
    }

    [[nodiscard]] VkExtent2D extent() const noexcept {
        return extent_;
    }

    [[nodiscard]] const std::vector<VkImage>& images() const noexcept {
        return images_;
    }

    [[nodiscard]] const std::vector<VkImageView>& imageViews() const noexcept {
        return imageViews_;
    }

    [[nodiscard]] std::size_t imageCount() const noexcept {
        return images_.size();
    }

  private:
    SDL_Window* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    const VulkanDevice* device_ = nullptr;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkPresentModeKHR> supportedPresentModes_;
    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;

    void createResources(VkSwapchainKHR oldSwapchain);

    void destroyImageViews() noexcept;
};
} // namespace Engine
