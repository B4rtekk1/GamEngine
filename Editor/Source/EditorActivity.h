#pragma once

#include "Editor/UI/EditorTheme.h"
#include "Engine/Core/Diagnostics.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Editor {
/**
 * What the editor is busy with, for the status bar and the viewport.
 *
 * Background work (scene decoding, C++ script builds, game builds) is
 * reported every frame with track(); synchronous work that freezes the UI
 * (GPU scene preparation, leaving Play Mode, shader reloads) brackets itself
 * with begin()/finish(). A finished activity stays visible for a few seconds
 * with its duration, and its tooltip lists the slowest renderer stages the
 * engine reported meanwhile ([GPUInit] / [SceneReload] diagnostics).
 *
 * Main-thread only, like the ImGui frame that draws it.
 */
class EditorActivity final {
  public:
    using Clock = std::chrono::steady_clock;

    /** Declares this frame whether background work @p key is running. */
    static void track(const std::string_view key, const bool active, const std::string_view label) {
        auto& self = instance();
        const auto found = self.find(key);
        if (active && found == self.running_.end())
            self.running_.push_back(started(key, label));
        else if (active)
            found->label = label;
        else if (found != self.running_.end())
            self.complete(found);
    }

    /** Starts synchronous work @p key; a repeated begin keeps the first start. */
    static void begin(const std::string_view key, const std::string_view label) {
        auto& self = instance();
        if (self.find(key) == self.running_.end())
            self.running_.push_back(started(key, label));
    }

    /** Ends work @p key, if it is running, and shows how long it took. */
    static void finish(const std::string_view key) {
        auto& self = instance();
        if (const auto found = self.find(key); found != self.running_.end())
            self.complete(found);
    }

    [[nodiscard]] static bool running(const std::string_view key) {
        auto& self = instance();
        return self.find(key) != self.running_.end();
    }

    /** Seconds since @p key started, or 0 when it is not running. */
    [[nodiscard]] static float elapsedSeconds(const std::string_view key) {
        auto& self = instance();
        const auto found = self.find(key);
        return found == self.running_.end() ? 0.0F : secondsSince(found->started);
    }

    /** Right-aligned status bar content; returns its width when @p measureOnly. */
    static float drawStatus(const float barHeight, const bool measureOnly) {
        auto& self = instance();
        const EditorUI::Palette& palette = EditorUI::colors();
        if (self.finished_ && secondsSince(self.finished_->shownAt) > FinishedNoteSeconds)
            self.finished_.reset();
        std::string text;
        if (!self.running_.empty()) {
            const Activity& newest = self.running_.back();
            char line[256]{};
            std::snprintf(line, sizeof(line), "%s... %s", newest.label.c_str(),
                          formatSeconds(secondsSince(newest.started)).c_str());
            text = line;
            if (self.running_.size() > 1)
                text += "  (+" + std::to_string(self.running_.size() - 1) + " more)";
        } else if (self.finished_) {
            text = self.finished_->text;
        }
        if (text.empty())
            return 0.0F;
        constexpr float IconWidth = 18.0F;
        constexpr float Spacing = 18.0F;
        const float width = IconWidth + 4.0F + ImGui::CalcTextSize(text.c_str()).x + Spacing;
        if (measureOnly)
            return width;

        const float textY = std::round((barHeight - ImGui::GetTextLineHeight()) * 0.5F);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float centerY = ImGui::GetWindowPos().y + barHeight * 0.5F;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const bool busy = !self.running_.empty();
        const ImVec4 color = busy ? palette.accent : palette.success;
        if (busy) {
            const float time = static_cast<float>(ImGui::GetTime());
            drawList->PathArcTo({position.x + 7.0F, centerY}, 6.0F, time * 6.0F, time * 6.0F + 4.2F, 14);
            drawList->PathStroke(ImGui::GetColorU32(color), ImDrawFlags_None, 2.0F);
        } else {
            // Check mark.
            const ImU32 check = ImGui::GetColorU32(color);
            drawList->AddLine({position.x + 2.0F, centerY}, {position.x + 6.0F, centerY + 4.0F}, check, 2.0F);
            drawList->AddLine({position.x + 6.0F, centerY + 4.0F}, {position.x + 13.0F, centerY - 4.0F}, check, 2.0F);
        }
        ImGui::Dummy({IconWidth, 1.0F});
        ImGui::SameLine(0.0F, 4.0F);
        ImGui::SetCursorPosY(textY);
        ImGui::TextColored(busy ? color : palette.textSecondary, "%s", text.c_str());
        if (ImGui::IsItemHovered())
            self.drawTooltip();
        ImGui::SameLine(0.0F, Spacing);
        ImGui::SetCursorPosY(textY);
        return width;
    }

    /**
     * Label for the viewport placeholder while the scene is not renderable
     * yet: GPU preparation (a UI freeze follows) before CPU decoding.
     */
    [[nodiscard]] static std::optional<std::string> viewportLabel() {
        auto& self = instance();
        for (const std::string_view key : {std::string_view{GpuPrepareKey}, std::string_view{SceneLoadKey}}) {
            const auto found = self.find(key);
            if (found == self.running_.end())
                continue;
            return found->label + "... " + formatSeconds(secondsSince(found->started));
        }
        return std::nullopt;
    }

    static constexpr std::string_view SceneLoadKey = "scene-load";
    static constexpr std::string_view GpuPrepareKey = "gpu-prepare";

  private:
    struct Activity final {
        std::string key;
        std::string label;
        Clock::time_point started;
        std::chrono::system_clock::time_point startedWall;
    };
    struct Finished final {
        std::string text;
        Clock::time_point shownAt;
        std::vector<std::pair<std::string, long>> slowestStages;
    };
    static constexpr float FinishedNoteSeconds = 6.0F;

    [[nodiscard]] static EditorActivity& instance() {
        static EditorActivity activity;
        return activity;
    }
    [[nodiscard]] static Activity started(const std::string_view key, const std::string_view label) {
        return {std::string{key}, std::string{label}, Clock::now(), std::chrono::system_clock::now()};
    }
    [[nodiscard]] static float secondsSince(const Clock::time_point start) {
        return std::chrono::duration<float>(Clock::now() - start).count();
    }
    [[nodiscard]] static std::string formatSeconds(const float seconds) {
        char text[32]{};
        if (seconds < 10.0F)
            std::snprintf(text, sizeof(text), "%.1f s", static_cast<double>(seconds));
        else
            std::snprintf(text, sizeof(text), "%.0f s", static_cast<double>(seconds));
        return text;
    }
    [[nodiscard]] std::vector<Activity>::iterator find(const std::string_view key) {
        return std::ranges::find_if(running_, [key](const Activity& activity) { return activity.key == key; });
    }

    void complete(const std::vector<Activity>::iterator activity) {
        const float seconds = secondsSince(activity->started);
        Finished finished{activity->label + " - done in " + formatSeconds(seconds), Clock::now(), {}};
        // "[GPUInit] Forward pipelines: 5251 ms" lines reported since start.
        for (const Engine::Diagnostic& entry : Engine::Diagnostics::instance().entries()) {
            if (entry.timestamp < activity->startedWall)
                continue;
            const std::string_view message{entry.message};
            if (!message.starts_with("[GPUInit] ") && !message.starts_with("[SceneReload] "))
                continue;
            const auto colon = message.rfind(": ");
            if (colon == std::string_view::npos || !message.ends_with(" ms"))
                continue;
            const auto number = message.substr(colon + 2, message.size() - colon - 5);
            long milliseconds = 0;
            try {
                milliseconds = std::stol(std::string{number});
            } catch (...) {
                continue;
            }
            const auto stageStart = message.find("] ") + 2;
            finished.slowestStages.emplace_back(std::string{message.substr(stageStart, colon - stageStart)},
                                                milliseconds);
        }
        std::ranges::sort(finished.slowestStages,
                          [](const auto& left, const auto& right) { return left.second > right.second; });
        if (finished.slowestStages.size() > 5)
            finished.slowestStages.resize(5);
        finished_ = std::move(finished);
        running_.erase(activity);
    }

    void drawTooltip() const {
        if (!ImGui::BeginTooltip())
            return;
        if (!running_.empty()) {
            for (const Activity& activity : running_)
                ImGui::Text("%s... %s", activity.label.c_str(), formatSeconds(secondsSince(activity.started)).c_str());
            ImGui::TextDisabled("The editor may stop responding while the GPU is prepared.");
        } else if (finished_) {
            ImGui::TextUnformatted(finished_->text.c_str());
            if (!finished_->slowestStages.empty()) {
                ImGui::Separator();
                ImGui::TextDisabled("Slowest renderer stages");
                for (const auto& [stage, milliseconds] : finished_->slowestStages)
                    ImGui::Text("%-34s %6ld ms", stage.c_str(), milliseconds);
            }
        }
        ImGui::EndTooltip();
    }

    std::vector<Activity> running_;
    std::optional<Finished> finished_;
};
} // namespace Editor
