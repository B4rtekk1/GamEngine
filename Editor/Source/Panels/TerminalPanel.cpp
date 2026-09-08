#include "Editor/Panels/TerminalPanel.h"

#include "Platform/Terminal/TerminalSession.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace {

ImU32 toColor(const Platform::TerminalColor color) {
    return IM_COL32(color.red, color.green, color.blue, 255);
}

void appendUtf8(std::string& output, const char32_t character) {
    if (character <= 0x7F) output.push_back(static_cast<char>(character));
    else if (character <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (character >> 6)));
        output.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else if (character <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (character >> 12)));
        output.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (character >> 18)));
        output.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    }
}

} // namespace

namespace Editor {

TerminalPanel::TerminalPanel(std::filesystem::path projectRoot, ImFont* terminalFont)
    : projectRoot_{std::move(projectRoot)}, terminalFont_{terminalFont} {
    restart();
}

TerminalPanel::~TerminalPanel() = default;

void TerminalPanel::restart() {
    shell_ = Platform::defaultTerminalShell();
    session_ = Platform::createTerminalSession();
    startFailed_ = !session_ || !session_->start(shell_, projectRoot_);
    if (!startFailed_) session_->resize(buffer_.columns(), buffer_.rows());
}

void TerminalPanel::handleKeyboard() {
    if (!hasKeyboardFocus_ || !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || !session_ ||
        !session_->running()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.WantCaptureKeyboard = true;
    io.WantTextInput = true;
    // This panel is not an ImGui text widget, so the SDL backend will not
    // request text input for it. Enable it explicitly to receive SDL text
    // events (including IME and AltGr) in InputQueueCharacters.
    if (SDL_Window* window = SDL_GetKeyboardFocus(); window != nullptr) SDL_StartTextInput(window);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) session_->write("\x1B");
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) session_->write("\x03");
    else {
        std::string text;
        for (const ImWchar character : io.InputQueueCharacters) appendUtf8(text, character);
        if (!text.empty()) {
            // AltGr is reported as Ctrl+Alt and must remain ordinary Unicode input.
            if (io.KeyAlt && !io.KeyCtrl) text.insert(text.begin(), '\x1B');
            session_->write(text);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) session_->write("\r");
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) session_->write("\x7F");
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) session_->write("\t");
    const char* cursorPrefix = buffer_.applicationCursorMode() ? "\x1BO" : "\x1B[";
    const char* modifiedCursorPrefix = io.KeyCtrl ? "\x1B[1;5" : cursorPrefix;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) session_->write(std::string{modifiedCursorPrefix} + "A");
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) session_->write(std::string{modifiedCursorPrefix} + "B");
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) session_->write(std::string{modifiedCursorPrefix} + "C");
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) session_->write(std::string{modifiedCursorPrefix} + "D");
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) session_->write(std::string{cursorPrefix} + "H");
    if (ImGui::IsKeyPressed(ImGuiKey_End)) session_->write(std::string{cursorPrefix} + "F");
    if (ImGui::IsKeyPressed(ImGuiKey_Insert)) session_->write("\x1B[2~");
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) session_->write("\x1B[3~");
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) session_->write("\x1B[5~");
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) session_->write("\x1B[6~");
    if (ImGui::IsKeyPressed(ImGuiKey_F1)) session_->write("\x1BOP");
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) session_->write("\x1BOQ");
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) session_->write("\x1BOR");
    if (ImGui::IsKeyPressed(ImGuiKey_F4)) session_->write("\x1BOS");
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) session_->write("\x1B[15~");
    if (ImGui::IsKeyPressed(ImGuiKey_F6)) session_->write("\x1B[17~");
    if (ImGui::IsKeyPressed(ImGuiKey_F7)) session_->write("\x1B[18~");
    if (ImGui::IsKeyPressed(ImGuiKey_F8)) session_->write("\x1B[19~");
    if (ImGui::IsKeyPressed(ImGuiKey_F9)) session_->write("\x1B[20~");
    if (ImGui::IsKeyPressed(ImGuiKey_F10)) session_->write("\x1B[21~");
    if (ImGui::IsKeyPressed(ImGuiKey_F11)) session_->write("\x1B[23~");
    if (ImGui::IsKeyPressed(ImGuiKey_F12)) session_->write("\x1B[24~");
}

void TerminalPanel::drawScreen() {
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const auto columns = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(area.x / cellWidth_), 1, 32767));
    const auto rows = static_cast<std::uint16_t>(std::clamp(
        static_cast<int>(area.y / cellHeight_), 1, 32767));
    if (columns != buffer_.columns() || rows != buffer_.rows()) {
        buffer_.resize(columns, rows);
        if (session_) session_->resize(columns, rows);
    }
    ImGui::InvisibleButton("##terminal-screen", area);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        hasKeyboardFocus_ = true;
    }
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(origin, {origin.x + area.x, origin.y + area.y}, true);
    const ImU32 background = IM_COL32(14, 16, 22, 255);
    drawList->AddRectFilled(origin, {origin.x + area.x, origin.y + area.y}, background);
    ImFont* font = terminalFont_ != nullptr ? terminalFont_ : ImGui::GetFont();
    const float fontSize = font->LegacySize;
    for (std::uint16_t y = 0; y < buffer_.rows(); ++y) {
        for (std::uint16_t x = 0; x < buffer_.columns(); ++x) {
            const Platform::TerminalCell& cell = buffer_.cell(x, y);
            const ImVec2 position{origin.x + static_cast<float>(x) * cellWidth_,
                                  origin.y + static_cast<float>(y) * cellHeight_};
            if (cell.background.red != 14 || cell.background.green != 16 || cell.background.blue != 22) {
                drawList->AddRectFilled(position, {position.x + cellWidth_, position.y + cellHeight_},
                                        toColor(cell.background));
            }
            if (cell.character != U' ') {
                std::string character;
                appendUtf8(character, cell.character);
                drawList->AddText(font, fontSize, position, toColor(cell.foreground), character.c_str());
                if (cell.underline) drawList->AddLine({position.x, position.y + cellHeight_ - 2.0F},
                                                      {position.x + cellWidth_, position.y + cellHeight_ - 2.0F},
                                                      toColor(cell.foreground));
            }
        }
    }
    if (hasKeyboardFocus_ && buffer_.cursorVisible() && session_ && session_->running()) {
        const float cursorX = origin.x + static_cast<float>(buffer_.cursorColumn()) * cellWidth_;
        const float cursorY = origin.y + static_cast<float>(buffer_.cursorRow()) * cellHeight_;
        drawList->AddRectFilled({cursorX, cursorY}, {cursorX + 2.0F, cursorY + cellHeight_},
                                IM_COL32(110, 210, 235, 190));
    }
    drawList->PopClipRect();
}

void TerminalPanel::draw(bool& isOpen) {
    if (!ImGui::Begin("Terminal", &isOpen)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Restart")) restart();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", shell_.string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled(session_ && session_->running() ? "running" : "stopped");
    ImGui::Separator();
    if (startFailed_) {
        ImGui::TextColored({0.95F, 0.35F, 0.35F, 1.0F}, "Could not start %s.", shell_.string().c_str());
        ImGui::TextDisabled("Windows 10 version 1809 or newer is required for ConPTY.");
    } else if (session_) {
        buffer_.feed(session_->readAvailable());
        ImGui::PushFont(terminalFont_ != nullptr ? terminalFont_ : ImGui::GetFont());
        cellWidth_ = ImGui::CalcTextSize("M").x;
        cellHeight_ = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##terminal-output", {0.0F, 0.0F}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        handleKeyboard();
        drawScreen();
        ImGui::EndChild();
        ImGui::PopFont();
    }
    ImGui::End();
}

} // namespace Editor
