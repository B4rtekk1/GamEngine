#pragma once

struct SDL_Window;

namespace Editor {

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

    // SDL and ImGui use the same window-relative physical-pixel coordinates
    // as Win32/DWM on Windows.
    // The area contains UI which must receive mouse input rather than drag the window.
    void setInteractiveArea(float left, float right, float height);

    [[nodiscard]] float contentRight() const;
    [[nodiscard]] int captionButtonHitTest(int x, int y) const noexcept;
    [[nodiscard]] bool isInteractiveClientPoint(int x, int y) const noexcept;
    [[nodiscard]] int nativeHeight() const noexcept;

private:
    void* windowHandle_ = nullptr;
    int interactiveLeft_ = 0;
    int interactiveRight_ = 0;
    int titleBarHeight_ = 36;
};

} // namespace Editor
