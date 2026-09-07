#pragma once

#include "imgui.h"

namespace EditorUI {

/** Collapsible inspector component section with an overflow removal action. */
class ComponentCard final {
public:
    ComponentCard(const char* id, const char* name, bool removable = true);

    [[nodiscard]] bool begin();
    void end();
    [[nodiscard]] bool removeRequested() const { return removeRequested_; }

private:
    const char* id_;
    const char* name_;
    bool removable_;
    bool open_{};
    bool removeRequested_{};
};

} // namespace EditorUI
