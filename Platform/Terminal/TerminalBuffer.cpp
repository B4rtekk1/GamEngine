#include "Platform/Terminal/TerminalBuffer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>

namespace {

constexpr Platform::TerminalColor defaultForeground{220, 220, 220};
constexpr Platform::TerminalColor defaultBackground{20, 22, 28};
constexpr std::array<Platform::TerminalColor, 8> ansiColors{{
    {0, 0, 0}, {205, 49, 49}, {13, 188, 121}, {229, 229, 16},
    {36, 114, 200}, {188, 63, 188}, {17, 168, 205}, {229, 229, 229},
}};

char32_t decodeUtf8(const std::string_view text, std::size_t& index) {
    const auto byte = static_cast<unsigned char>(text[index++]);
    if (byte < 0x80) return byte;
    const int continuationCount = byte < 0xE0 ? 1 : byte < 0xF0 ? 2 : 3;
    char32_t result = byte & ((1U << (7 - continuationCount - 1)) - 1U);
    for (int count = 0; count < continuationCount && index < text.size(); ++count) {
        const auto continuation = static_cast<unsigned char>(text[index]);
        if ((continuation & 0xC0U) != 0x80U) return U'?';
        ++index;
        result = (result << 6U) | (continuation & 0x3FU);
    }
    return result;
}

} // namespace

namespace Platform {

TerminalBuffer::TerminalBuffer(const std::uint16_t columns, const std::uint16_t rows) {
    resize(columns, rows);
}

void TerminalBuffer::resize(const std::uint16_t columns, const std::uint16_t rows) {
    columns_ = std::max<std::uint16_t>(columns, 1);
    rows_ = std::max<std::uint16_t>(rows, 1);
    cells_.assign(static_cast<std::size_t>(columns_) * rows_, TerminalCell{});
    cursorX_ = 0;
    cursorY_ = 0;
    resetStyle();
}

const TerminalCell& TerminalBuffer::cell(const std::uint16_t x, const std::uint16_t y) const {
    return cells_[static_cast<std::size_t>(y) * columns_ + x];
}

void TerminalBuffer::resetStyle() {
    style_ = TerminalCell{};
    style_.foreground = defaultForeground;
    style_.background = defaultBackground;
}

void TerminalBuffer::scroll() {
    const auto lineWidth = static_cast<std::size_t>(columns_);
    std::move(cells_.begin() + static_cast<std::ptrdiff_t>(lineWidth), cells_.end(), cells_.begin());
    std::fill(cells_.end() - static_cast<std::ptrdiff_t>(lineWidth), cells_.end(), TerminalCell{});
    cursorY_ = static_cast<std::uint16_t>(rows_ - 1);
}

void TerminalBuffer::lineFeed() {
    cursorX_ = 0;
    ++cursorY_;
    if (cursorY_ >= rows_) scroll();
}

void TerminalBuffer::put(const char32_t character) {
    cells_[static_cast<std::size_t>(cursorY_) * columns_ + cursorX_] = style_;
    cells_[static_cast<std::size_t>(cursorY_) * columns_ + cursorX_].character = character;
    ++cursorX_;
    if (cursorX_ >= columns_) lineFeed();
}

void TerminalBuffer::eraseDisplay(const int mode) {
    if (mode == 2 || mode == 3) {
        std::fill(cells_.begin(), cells_.end(), TerminalCell{});
        cursorX_ = 0;
        cursorY_ = 0;
        return;
    }
    const auto cursor = static_cast<std::size_t>(cursorY_) * columns_ + cursorX_;
    if (mode == 1) std::fill(cells_.begin(), cells_.begin() + static_cast<std::ptrdiff_t>(cursor + 1), TerminalCell{});
    else std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(cursor), cells_.end(), TerminalCell{});
}

void TerminalBuffer::eraseLine(const int mode) {
    const auto first = static_cast<std::size_t>(cursorY_) * columns_;
    const auto cursor = first + cursorX_;
    if (mode == 1) std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(first), cells_.begin() + static_cast<std::ptrdiff_t>(cursor + 1), TerminalCell{});
    else if (mode == 2) std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(first), cells_.begin() + static_cast<std::ptrdiff_t>(first + columns_), TerminalCell{});
    else std::fill(cells_.begin() + static_cast<std::ptrdiff_t>(cursor), cells_.begin() + static_cast<std::ptrdiff_t>(first + columns_), TerminalCell{});
}

void TerminalBuffer::applySgr(const std::vector<int>& parameters) {
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const int parameter = parameters[index];
        if (parameter == 0) resetStyle();
        else if (parameter == 1) style_.bold = true;
        else if (parameter == 4) style_.underline = true;
        else if (parameter == 22) style_.bold = false;
        else if (parameter == 24) style_.underline = false;
        else if (parameter >= 30 && parameter <= 37) style_.foreground = ansiColors[parameter - 30];
        else if (parameter == 39) style_.foreground = defaultForeground;
        else if (parameter >= 40 && parameter <= 47) style_.background = ansiColors[parameter - 40];
        else if (parameter == 49) style_.background = defaultBackground;
        else if ((parameter == 38 || parameter == 48) && index + 4 < parameters.size() && parameters[index + 1] == 2) {
            const TerminalColor color{static_cast<std::uint8_t>(std::clamp(parameters[index + 2], 0, 255)),
                                      static_cast<std::uint8_t>(std::clamp(parameters[index + 3], 0, 255)),
                                      static_cast<std::uint8_t>(std::clamp(parameters[index + 4], 0, 255))};
            if (parameter == 38) style_.foreground = color;
            else style_.background = color;
            index += 4;
        }
    }
}

void TerminalBuffer::feed(const std::string_view utf8) {
    for (std::size_t index = 0; index < utf8.size();) {
        const char32_t character = decodeUtf8(utf8, index);
        if (character == U'\x1B' && index < utf8.size() && utf8[index] == '[') {
            ++index;
            const std::size_t begin = index;
            while (index < utf8.size() && (utf8[index] < '@' || utf8[index] > '~')) ++index;
            if (index == utf8.size()) break;
            const char command = utf8[index++];
            std::vector<int> parameters;
            const std::string values{utf8.substr(begin, index - begin - 1)};
            std::size_t valueBegin = 0;
            do {
                const std::size_t separator = values.find(';', valueBegin);
                int value{};
                const auto field = values.substr(valueBegin, separator - valueBegin);
                if (!field.empty()) std::from_chars(field.data(), field.data() + field.size(), value);
                parameters.push_back(value);
                valueBegin = separator == std::string::npos ? values.size() + 1 : separator + 1;
            } while (valueBegin <= values.size());
            const int amount = parameters.empty() || parameters.front() == 0 ? 1 : parameters.front();
            if (command == 'm') applySgr(parameters);
            else if (command == 'H' || command == 'f') {
                cursorY_ = static_cast<std::uint16_t>(std::clamp(parameters.empty() ? 1 : parameters[0], 1, static_cast<int>(rows_)) - 1);
                cursorX_ = static_cast<std::uint16_t>(std::clamp(parameters.size() < 2 ? 1 : parameters[1], 1, static_cast<int>(columns_)) - 1);
            } else if (command == 'A') cursorY_ = static_cast<std::uint16_t>(std::max(0, static_cast<int>(cursorY_) - amount));
            else if (command == 'B') cursorY_ = static_cast<std::uint16_t>(std::min(static_cast<int>(rows_ - 1), static_cast<int>(cursorY_) + amount));
            else if (command == 'C') cursorX_ = static_cast<std::uint16_t>(std::min(static_cast<int>(columns_ - 1), static_cast<int>(cursorX_) + amount));
            else if (command == 'D') cursorX_ = static_cast<std::uint16_t>(std::max(0, static_cast<int>(cursorX_) - amount));
            else if (command == 'J') eraseDisplay(parameters.empty() ? 0 : parameters.front());
            else if (command == 'K') eraseLine(parameters.empty() ? 0 : parameters.front());
            continue;
        }
        if (character == U'\r') cursorX_ = 0;
        else if (character == U'\n') lineFeed();
        else if (character == U'\b') { if (cursorX_ > 0) --cursorX_; }
        else if (character == U'\t') { const auto spaces = static_cast<std::uint16_t>(4 - cursorX_ % 4); for (std::uint16_t space = 0; space < spaces; ++space) put(U' '); }
        else if (character >= U' ') put(character);
    }
}

} // namespace Platform
