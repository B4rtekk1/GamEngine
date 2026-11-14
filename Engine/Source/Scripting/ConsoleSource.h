#pragma once

#include "Engine/Core/Diagnostics.h"

namespace Engine {
/**
 * The script whose callback is running on this thread, so Console messages
 * name their object. Built lazily: a frame of script updates only swaps a
 * pointer, the name lookup happens when something is actually logged.
 */
struct ConsoleSource final {
    const void* owner{};
    DiagnosticContext (*context)(const void* owner){};
};

class ScopedConsoleSource final {
  public:
    explicit ScopedConsoleSource(const ConsoleSource& source) noexcept;
    ~ScopedConsoleSource();
    ScopedConsoleSource(const ScopedConsoleSource&) = delete;
    ScopedConsoleSource& operator=(const ScopedConsoleSource&) = delete;

  private:
    const ConsoleSource* previous_;
    // A callback that leaves a console.group() open does not indent the next one.
    int previousGroupDepth_;
};
} // namespace Engine
