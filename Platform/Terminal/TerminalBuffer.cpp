#include "Platform/Terminal/TerminalBuffer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>

namespace {
constexpr Platform::TerminalColor defaultForeground{220, 220, 220};
constexpr Platform::TerminalColor defaultBackground{14, 16, 22};
constexpr std::array<Platform::TerminalColor, 8> ansiColors{{
    {0, 0, 0}, {205, 49, 49}, {13, 188, 121}, {229, 229, 16},
    {36, 114, 200}, {188, 63, 188}, {17, 168, 205}, {229, 229, 229},
}};
constexpr std::array<Platform::TerminalColor, 8> brightAnsiColors{{
    {102, 102, 102}, {241, 76, 76}, {35, 209, 139}, {245, 245, 67},
    {59, 142, 234}, {214, 112, 214}, {41, 184, 219}, {255, 255, 255},
}};

Platform::TerminalColor indexedColor(const int index) {
    if (index < 8) return ansiColors[static_cast<std::size_t>(index)];
    if (index < 16) return brightAnsiColors[static_cast<std::size_t>(index - 8)];
    if (index >= 232) { const auto value = static_cast<std::uint8_t>(8 + (index - 232) * 10); return {value, value, value}; }
    const int cube = index - 16;
    const auto component = [](const int value) -> std::uint8_t { return static_cast<std::uint8_t>(value == 0 ? 0 : 55 + value * 40); };
    return {component(cube / 36), component(cube / 6 % 6), component(cube % 6)};
}
} // namespace

namespace Platform {
TerminalBuffer::TerminalBuffer(const std::uint16_t columns, const std::uint16_t rows) { resetStyle(); resize(columns, rows); }

void TerminalBuffer::resize(const std::uint16_t columns, const std::uint16_t rows) {
    const std::uint16_t newColumns = std::max<std::uint16_t>(columns, 1);
    const std::uint16_t newRows = std::max<std::uint16_t>(rows, 1);
    std::vector<TerminalCell> resized(static_cast<std::size_t>(newColumns) * newRows, TerminalCell{});
    const auto copyColumns = std::min(columns_, newColumns);
    const auto copyRows = std::min(rows_, newRows);
    for (std::uint16_t y = 0; y < copyRows; ++y)
        std::copy_n(cells_.begin() + static_cast<std::ptrdiff_t>(y) * columns_, copyColumns, resized.begin() + static_cast<std::ptrdiff_t>(y) * newColumns);
    columns_ = newColumns; rows_ = newRows; cells_ = std::move(resized);
    cursorX_ = std::min<std::uint16_t>(cursorX_, columns_ - 1); cursorY_ = std::min<std::uint16_t>(cursorY_, rows_ - 1);
    savedCursorX_ = std::min<std::uint16_t>(savedCursorX_, columns_ - 1); savedCursorY_ = std::min<std::uint16_t>(savedCursorY_, rows_ - 1);
}

const TerminalCell& TerminalBuffer::cell(const std::uint16_t x, const std::uint16_t y) const { return cells_[static_cast<std::size_t>(y) * columns_ + x]; }
void TerminalBuffer::resetStyle() { style_ = TerminalCell{}; style_.foreground = defaultForeground; style_.background = defaultBackground; }
void TerminalBuffer::scroll() { const auto width = static_cast<std::size_t>(columns_); std::move(cells_.begin() + static_cast<std::ptrdiff_t>(width), cells_.end(), cells_.begin()); std::fill(cells_.end() - static_cast<std::ptrdiff_t>(width), cells_.end(), TerminalCell{}); cursorY_ = rows_ - 1; }
void TerminalBuffer::lineFeed() { if (++cursorY_ >= rows_) scroll(); }
void TerminalBuffer::put(const char32_t character) { cells_[static_cast<std::size_t>(cursorY_) * columns_ + cursorX_] = style_; cells_[static_cast<std::size_t>(cursorY_) * columns_ + cursorX_].character = character; if (++cursorX_ >= columns_) { cursorX_ = 0; lineFeed(); } }

void TerminalBuffer::eraseDisplay(const int mode) {
    if (mode == 2 || mode == 3) { std::fill(cells_.begin(), cells_.end(), TerminalCell{}); return; }
    const auto cursor = static_cast<std::size_t>(cursorY_) * columns_ + cursorX_;
    if (mode == 1) std::fill(cells_.begin(), cells_.begin() + static_cast<std::ptrdiff_t>(cursor + 1), TerminalCell{});
    else std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(cursor), cells_.end(), TerminalCell{});
}
void TerminalBuffer::eraseLine(const int mode) {
    const auto first = static_cast<std::size_t>(cursorY_) * columns_, cursor = first + cursorX_;
    if (mode == 1) std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(first), cells_.begin() + static_cast<std::ptrdiff_t>(cursor + 1), TerminalCell{});
    else if (mode == 2) std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(first), cells_.begin() + static_cast<std::ptrdiff_t>(first + columns_), TerminalCell{});
    else std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(cursor), cells_.begin() + static_cast<std::ptrdiff_t>(first + columns_), TerminalCell{});
}
void TerminalBuffer::eraseCharacters(const std::uint16_t amount) { const auto count = std::min<std::uint16_t>(amount, columns_ - cursorX_); const auto first = static_cast<std::size_t>(cursorY_) * columns_ + cursorX_; std::fill_n(cells_.begin() + static_cast<std::ptrdiff_t>(first), count, TerminalCell{}); }
void TerminalBuffer::insertCharacters(const std::uint16_t amount) { const auto count = std::min<std::uint16_t>(amount, columns_ - cursorX_); const auto first = static_cast<std::size_t>(cursorY_) * columns_ + cursorX_, end = static_cast<std::size_t>(cursorY_ + 1) * columns_; std::move_backward(cells_.begin() + static_cast<std::ptrdiff_t>(first), cells_.begin() + static_cast<std::ptrdiff_t>(end - count), cells_.begin() + static_cast<std::ptrdiff_t>(end)); std::fill_n(cells_.begin() + static_cast<std::ptrdiff_t>(first), count, TerminalCell{}); }
void TerminalBuffer::deleteCharacters(const std::uint16_t amount) { const auto count = std::min<std::uint16_t>(amount, columns_ - cursorX_); const auto first = static_cast<std::size_t>(cursorY_) * columns_ + cursorX_, end = static_cast<std::size_t>(cursorY_ + 1) * columns_; std::move(cells_.begin() + static_cast<std::ptrdiff_t>(first + count), cells_.begin() + static_cast<std::ptrdiff_t>(end), cells_.begin() + static_cast<std::ptrdiff_t>(first)); std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(end - count), cells_.begin() + static_cast<std::ptrdiff_t>(end), TerminalCell{}); }

void TerminalBuffer::applySgr(const std::vector<int>& parameters) {
    for (std::size_t index = 0; index < parameters.size(); ++index) { const int p = parameters[index];
        if (p == 0) resetStyle(); else if (p == 1) style_.bold = true; else if (p == 4) style_.underline = true; else if (p == 22) style_.bold = false; else if (p == 24) style_.underline = false;
        else if (p >= 30 && p <= 37) style_.foreground = ansiColors[p - 30]; else if (p >= 90 && p <= 97) style_.foreground = brightAnsiColors[p - 90]; else if (p == 39) style_.foreground = defaultForeground;
        else if (p >= 40 && p <= 47) style_.background = ansiColors[p - 40]; else if (p >= 100 && p <= 107) style_.background = brightAnsiColors[p - 100]; else if (p == 49) style_.background = defaultBackground;
        else if ((p == 38 || p == 48) && index + 2 < parameters.size()) { TerminalColor color{}; if (parameters[index + 1] == 5) { color = indexedColor(std::clamp(parameters[index + 2], 0, 255)); index += 2; } else if (parameters[index + 1] == 2 && index + 4 < parameters.size()) { color = {static_cast<std::uint8_t>(std::clamp(parameters[index + 2], 0, 255)), static_cast<std::uint8_t>(std::clamp(parameters[index + 3], 0, 255)), static_cast<std::uint8_t>(std::clamp(parameters[index + 4], 0, 255))}; index += 4; } else continue; if (p == 38) style_.foreground = color; else style_.background = color; }
    }
}

void TerminalBuffer::emitCodepoint(const char32_t character) { if (character == U'\r') cursorX_ = 0; else if (character == U'\n') lineFeed(); else if (character == U'\b') { if (cursorX_ > 0) --cursorX_; } else if (character == U'\t') { const auto spaces = static_cast<std::uint16_t>(8 - cursorX_ % 8); for (std::uint16_t space = 0; space < spaces; ++space) put(U' '); } else if (character >= U' ') put(character); }

void TerminalBuffer::executeCsi(const char command) {
    const bool privateMode = !csiParameters_.empty() && csiParameters_.front() == '?'; std::string values = privateMode ? csiParameters_.substr(1) : csiParameters_; std::vector<int> p; std::size_t begin = 0;
    do { const auto separator = values.find(';', begin); int value{}; const auto field = values.substr(begin, separator - begin); if (!field.empty()) std::from_chars(field.data(), field.data() + field.size(), value); p.push_back(value); begin = separator == std::string::npos ? values.size() + 1 : separator + 1; } while (begin <= values.size());
    const int amount = p.empty() || p.front() == 0 ? 1 : p.front();
    if (privateMode) { if (p.front() == 25 && (command == 'h' || command == 'l')) cursorVisible_ = command == 'h'; if (p.front() == 1 && (command == 'h' || command == 'l')) applicationCursorMode_ = command == 'h'; return; }
    if (command == 'm') applySgr(p); else if (command == 'H' || command == 'f') { cursorY_ = static_cast<std::uint16_t>(std::clamp(p.empty() ? 1 : p[0], 1, static_cast<int>(rows_)) - 1); cursorX_ = static_cast<std::uint16_t>(std::clamp(p.size() < 2 ? 1 : p[1], 1, static_cast<int>(columns_)) - 1); }
    else if (command == 'A') cursorY_ = static_cast<std::uint16_t>(std::max(0, static_cast<int>(cursorY_) - amount)); else if (command == 'B') cursorY_ = static_cast<std::uint16_t>(std::min(static_cast<int>(rows_ - 1), static_cast<int>(cursorY_) + amount)); else if (command == 'C') cursorX_ = static_cast<std::uint16_t>(std::min(static_cast<int>(columns_ - 1), static_cast<int>(cursorX_) + amount)); else if (command == 'D') cursorX_ = static_cast<std::uint16_t>(std::max(0, static_cast<int>(cursorX_) - amount));
    else if (command == 'E') { cursorY_ = static_cast<std::uint16_t>(std::min(static_cast<int>(rows_ - 1), static_cast<int>(cursorY_) + amount)); cursorX_ = 0; } else if (command == 'F') { cursorY_ = static_cast<std::uint16_t>(std::max(0, static_cast<int>(cursorY_) - amount)); cursorX_ = 0; } else if (command == 'G') cursorX_ = static_cast<std::uint16_t>(std::clamp(amount, 1, static_cast<int>(columns_)) - 1);
    else if (command == 'J') eraseDisplay(p.empty() ? 0 : p.front()); else if (command == 'K') eraseLine(p.empty() ? 0 : p.front()); else if (command == 'X') eraseCharacters(static_cast<std::uint16_t>(amount)); else if (command == '@') insertCharacters(static_cast<std::uint16_t>(amount)); else if (command == 'P') deleteCharacters(static_cast<std::uint16_t>(amount)); else if (command == 's') { savedCursorX_ = cursorX_; savedCursorY_ = cursorY_; } else if (command == 'u') { cursorX_ = savedCursorX_; cursorY_ = savedCursorY_; }
}

void TerminalBuffer::processGroundByte(const unsigned char byte) { if (byte == 0x1B) { parserState_ = ParserState::Escape; return; } if (byte < 0x80) { emitCodepoint(byte); return; } utf8Bytes_[0] = byte; utf8Length_ = 1; utf8Expected_ = byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : byte < 0xF8 ? 4 : 1; parserState_ = utf8Expected_ == 1 ? ParserState::Ground : ParserState::Utf8; if (utf8Expected_ == 1) emitCodepoint(U'?'); }
void TerminalBuffer::processByte(const unsigned char byte) {
    if (parserState_ == ParserState::Ground) { processGroundByte(byte); return; }
    if (parserState_ == ParserState::Utf8) { if ((byte & 0xC0U) != 0x80U) { emitCodepoint(U'?'); parserState_ = ParserState::Ground; processGroundByte(byte); return; } utf8Bytes_[utf8Length_++] = byte; if (utf8Length_ == utf8Expected_) { char32_t character = utf8Bytes_[0] & ((1U << (7 - utf8Expected_)) - 1U); for (std::uint8_t i = 1; i < utf8Expected_; ++i) character = (character << 6U) | (utf8Bytes_[i] & 0x3FU); emitCodepoint(character); parserState_ = ParserState::Ground; } return; }
    if (parserState_ == ParserState::Escape) { if (byte == '[') { csiParameters_.clear(); parserState_ = ParserState::Csi; } else if (byte == ']') { oscEscape_ = false; parserState_ = ParserState::Osc; } else { if (byte == '7') { savedCursorX_ = cursorX_; savedCursorY_ = cursorY_; } else if (byte == '8') { cursorX_ = savedCursorX_; cursorY_ = savedCursorY_; } parserState_ = ParserState::Ground; } return; }
    if (parserState_ == ParserState::Csi) { if (byte >= '@' && byte <= '~') { executeCsi(static_cast<char>(byte)); parserState_ = ParserState::Ground; } else if (csiParameters_.size() < 4096) csiParameters_.push_back(static_cast<char>(byte)); return; }
    if (byte == 0x07 || (oscEscape_ && byte == '\\')) { parserState_ = ParserState::Ground; oscEscape_ = false; } else oscEscape_ = byte == 0x1B;
}
void TerminalBuffer::feed(const std::string_view utf8) { for (const unsigned char byte : utf8) processByte(byte); }
} // namespace Platform
