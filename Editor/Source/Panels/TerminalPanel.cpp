#include "Editor/Panels/TerminalPanel.h"

#include "Platform/Terminal/TerminalSession.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
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

std::string asciiKeyInput(const bool shift) {
    for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key))) {
            char character = static_cast<char>('a' + key - ImGuiKey_A);
            if (shift) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
            return {character};
        }
    }
    constexpr std::array<std::pair<ImGuiKey, std::pair<char, char>>, 21> keys{{
        {ImGuiKey_0, {'0', ')'}}, {ImGuiKey_1, {'1', '!'}}, {ImGuiKey_2, {'2', '@'}},
        {ImGuiKey_3, {'3', '#'}}, {ImGuiKey_4, {'4', '$'}}, {ImGuiKey_5, {'5', '%'}},
        {ImGuiKey_6, {'6', '^'}}, {ImGuiKey_7, {'7', '&'}}, {ImGuiKey_8, {'8', '*'}},
        {ImGuiKey_9, {'9', '('}}, {ImGuiKey_Space, {' ', ' '}}, {ImGuiKey_Minus, {'-', '_'}},
        {ImGuiKey_Equal, {'=', '+'}}, {ImGuiKey_LeftBracket, {'[', '{'}},
        {ImGuiKey_RightBracket, {']', '}'}}, {ImGuiKey_Backslash, {'\\', '|'}},
        {ImGuiKey_Semicolon, {';', ':'}}, {ImGuiKey_Apostrophe, {'\'', '"'}},
        {ImGuiKey_Comma, {',', '<'}}, {ImGuiKey_Period, {'.', '>'}}, {ImGuiKey_Slash, {'/', '?'}},
    }};
    for (const auto [key, characters] : keys) {
        if (ImGui::IsKeyPressed(key)) return {shift ? characters.second : characters.first};
    }
    return {};
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
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        hasKeyboardFocus_ = false;
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) session_->write("\x03");
    else {
        std::string text;
        for (const ImWchar character : io.InputQueueCharacters) appendUtf8(text, character);
        if (text.empty()) text = asciiKeyInput(io.KeyShift);
        if (!text.empty()) session_->write(text);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)) session_->write("\r");
    // ConPTY forwards terminal control characters directly; PSReadLine maps
    // the Windows Backspace key to BS (0x08), not DEL (0x7F).
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) session_->write("\b");
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) session_->write("\t");
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) session_->write("\x1B[A");
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) session_->write("\x1B[B");
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) session_->write("\x1B[C");
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) session_->write("\x1B[D");
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) session_->write("\x1B[H");
    if (ImGui::IsKeyPressed(ImGuiKey_End)) session_->write("\x1B[F");
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) session_->write("\x1B[3~");
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
    const ImU32 background = IM_COL32(20, 22, 28, 255);
    drawList->AddRectFilled(origin, {origin.x + area.x, origin.y + area.y}, background);
    ImFont* font = terminalFont_ != nullptr ? terminalFont_ : ImGui::GetFont();
    const float fontSize = font->LegacySize;
    for (std::uint16_t y = 0; y < buffer_.rows(); ++y) {
        for (std::uint16_t x = 0; x < buffer_.columns(); ++x) {
            const Platform::TerminalCell& cell = buffer_.cell(x, y);
            const ImVec2 position{origin.x + static_cast<float>(x) * cellWidth_,
                                  origin.y + static_cast<float>(y) * cellHeight_};
            if (cell.background.red != 20 || cell.background.green != 22 || cell.background.blue != 28) {
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
    if (hasKeyboardFocus_ && session_ && session_->running()) {
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
