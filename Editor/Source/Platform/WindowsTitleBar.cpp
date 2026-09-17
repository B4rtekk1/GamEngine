#include "Editor/Platform/WindowsTitleBar.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

[[nodiscard]] UINT windowDpi(const HWND window) {
    return GetDpiForWindow(window);
}

void extendFrame(const HWND window, const int titleBarHeight) {
    const MARGINS margins{
        .cxLeftWidth = 0,
        .cxRightWidth = 0,
        .cyTopHeight = titleBarHeight,
        .cyBottomHeight = 0,
    };
    static_cast<void>(DwmExtendFrameIntoClientArea(window, &margins));
}

[[nodiscard]] int resizeHitTest(const HWND window, const POINT point) {
    if (IsZoomed(window)) return HTNOWHERE;

    RECT bounds{};
    GetWindowRect(window, &bounds);
    const UINT dpi = windowDpi(window);
    const int border = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                       GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const bool left = point.x < bounds.left + border;
    const bool right = point.x >= bounds.right - border;
    const bool top = point.y < bounds.top + border;
    const bool bottom = point.y >= bounds.bottom - border;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    return HTNOWHERE;
}

[[nodiscard]] RECT captionButtons(const HWND window) {
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CAPTION_BUTTON_BOUNDS,
                                     &bounds, sizeof(bounds)))) {
        SetRectEmpty(&bounds);
    }
    return bounds;
}

LRESULT CALLBACK titleBarProc(const HWND window, const UINT message, const WPARAM wParam,
                              const LPARAM lParam, const UINT_PTR, const DWORD_PTR referenceData) {
    auto* const titleBar = reinterpret_cast<Editor::WindowsTitleBar*>(referenceData);
    LRESULT result{};
    // DWM owns the standard minimize/maximize/close buttons.  It must see the
    // message before our drag-region hit testing so Snap Layouts keep working.
    if (DwmDefWindowProc(window, message, wParam, lParam, &result)) return result;

    switch (message) {
        case WM_NCCALCSIZE:
            if (wParam == TRUE) return 0;
            break;

        case WM_NCHITTEST: {
            const POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT clientPoint = screenPoint;
            ScreenToClient(window, &clientPoint);
            if (const int caption = titleBar->captionButtonHitTest(clientPoint.x, clientPoint.y);
                caption != HTNOWHERE) {
                return caption;
            }
            if (const int resize = resizeHitTest(window, screenPoint); resize != HTNOWHERE) return resize;

            if (titleBar->isInteractiveClientPoint(clientPoint.x, clientPoint.y)) {
                return HTCLIENT;
            }
            if (clientPoint.y >= 0 && clientPoint.y < titleBar->nativeHeight()) return HTCAPTION;
            break;
        }

        case WM_DPICHANGED:
            extendFrame(window, titleBar->nativeHeight());
            break;

        default:
            break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace
#endif

namespace Editor {

WindowsTitleBar::~WindowsTitleBar() {
    detach();
}

void WindowsTitleBar::attach(SDL_Window* window) {
    detach();
#ifdef _WIN32
    if (window == nullptr) return;
    const HWND handle = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (handle == nullptr) return;

    windowHandle_ = handle;
    titleBarHeight_ = 36;
    interactiveRight_ = titleBarHeight_;
    extendFrame(handle, titleBarHeight_);
    if (!SetWindowSubclass(handle, titleBarProc, reinterpret_cast<UINT_PTR>(this),
                           reinterpret_cast<DWORD_PTR>(this))) {
        windowHandle_ = nullptr;
    }
#else
    static_cast<void>(window);
#endif
}

void WindowsTitleBar::detach() {
#ifdef _WIN32
    if (windowHandle_ != nullptr) {
        const HWND handle = static_cast<HWND>(windowHandle_);
        static_cast<void>(RemoveWindowSubclass(handle, titleBarProc, reinterpret_cast<UINT_PTR>(this)));
        windowHandle_ = nullptr;
    }
#endif
}

void WindowsTitleBar::setInteractiveArea(const float left, const float right, const float height) {
#ifdef _WIN32
    if (windowHandle_ == nullptr) return;
    const HWND handle = static_cast<HWND>(windowHandle_);
    interactiveLeft_ = static_cast<int>(std::lround(left));
    interactiveRight_ = std::max(interactiveLeft_, static_cast<int>(std::lround(right)));
    const int nativeHeight = std::max(1, static_cast<int>(std::lround(height)));
    if (nativeHeight != titleBarHeight_) {
        titleBarHeight_ = nativeHeight;
        extendFrame(handle, titleBarHeight_);
    }
#else
    static_cast<void>(left);
    static_cast<void>(right);
    static_cast<void>(height);
#endif
}

float WindowsTitleBar::contentRight() const {
#ifdef _WIN32
    if (windowHandle_ == nullptr) return 0.0F;
    const RECT bounds = captionButtons(static_cast<HWND>(windowHandle_));
    if (!IsRectEmpty(&bounds))
        return static_cast<float>(bounds.left);
#endif
    return 0.0F;
}

CaptionButtonBounds WindowsTitleBar::captionButtonBounds() const {
#ifdef _WIN32
    if (windowHandle_ != nullptr) {
        const RECT bounds = captionButtons(static_cast<HWND>(windowHandle_));
        if (!IsRectEmpty(&bounds)) {
            return {
                .left = static_cast<float>(bounds.left),
                .top = static_cast<float>(bounds.top),
                .right = static_cast<float>(bounds.right),
                .bottom = static_cast<float>(bounds.bottom),
            };
        }
    }
#endif
    return {};
}

int WindowsTitleBar::captionButtonHitTest(const int x, const int y) const noexcept {
#ifdef _WIN32
    if (windowHandle_ == nullptr) return HTNOWHERE;
    const RECT bounds = captionButtons(static_cast<HWND>(windowHandle_));
    if (!PtInRect(&bounds, {x, y})) return HTNOWHERE;
    const int buttonWidth = (bounds.right - bounds.left) / 3;
    if (buttonWidth <= 0) return HTNOWHERE;
    if (x < bounds.left + buttonWidth) return HTMINBUTTON;
    if (x < bounds.left + 2 * buttonWidth) return HTMAXBUTTON;
    return HTCLOSE;
#else
    static_cast<void>(x);
    static_cast<void>(y);
    return 0;
#endif
}

bool WindowsTitleBar::isMaximized() const noexcept {
#ifdef _WIN32
    return windowHandle_ != nullptr && IsZoomed(static_cast<HWND>(windowHandle_));
#else
    return false;
#endif
}

bool WindowsTitleBar::isInteractiveClientPoint(const int x, const int y) const noexcept {
    return y >= 0 && y < titleBarHeight_ && x >= interactiveLeft_ && x < interactiveRight_;
}

int WindowsTitleBar::nativeHeight() const noexcept {
    return titleBarHeight_;
}

} // namespace Editor
