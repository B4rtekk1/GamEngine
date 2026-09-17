#pragma once

#include <vector>

struct SDL_Window;

namespace Editor {

struct CaptionButtonBounds {
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;

    [[nodiscard]] bool valid() const noexcept { return right > left && bottom > top; }
};

// Extends the SDL client area into the Win32 title bar while leaving the DWM
// caption buttons (and therefore Windows 11 Snap Layouts) under Windows' control.
class WindowsTitleBar {
public:
    WindowsTitleBar() = default;
    ~WindowsTitleBar();

    WindowsTitleBar(const WindowsTitleBar&) = delete;
    WindowsTitleBar& operator=(const WindowsTitleBar&) = delete;

    void attach(SDL_Window* window);
    void detach();
    void updateNativeHeight();
    void clearHitRegions();
    void addHitRegion(float left, float top, float right, float bottom, long result);
    static constexpr long clientHitTestResult = 1;

    [[nodiscard]] float contentRight() const;
    [[nodiscard]] CaptionButtonBounds captionButtonBounds() const;
    [[nodiscard]] int captionButtonHitTest(int x, int y) const noexcept;
    [[nodiscard]] bool isMaximized() const noexcept;
    [[nodiscard]] long hitTest(int x, int y) const noexcept;
    [[nodiscard]] int nativeHeight() const noexcept;
    [[nodiscard]] float dpiScale() const noexcept;

private:
#ifdef _WIN32
    struct HitRegion {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        long result = 0;
    };

    std::vector<HitRegion> hitRegions_;
#endif
    void* windowHandle_ = nullptr;
    int titleBarHeight_ = 0;
};

} // namespace Editor
