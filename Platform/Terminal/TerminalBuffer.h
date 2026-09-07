#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace Platform {

struct TerminalColor final {
    std::uint8_t red{220};
    std::uint8_t green{220};
    std::uint8_t blue{220};
};

struct TerminalCell final {
    char32_t character{U' '};
    TerminalColor foreground{};
    TerminalColor background{20, 22, 28};
    bool bold{};
    bool underline{};
};

/** A small VT screen model for the escape sequences emitted by common shells. */
class TerminalBuffer final {
public:
    explicit TerminalBuffer(std::uint16_t columns = 120, std::uint16_t rows = 30);

    void resize(std::uint16_t columns, std::uint16_t rows);
    void feed(std::string_view utf8);

    [[nodiscard]] std::uint16_t columns() const { return columns_; }
    [[nodiscard]] std::uint16_t rows() const { return rows_; }
    [[nodiscard]] std::uint16_t cursorColumn() const { return cursorX_; }
    [[nodiscard]] std::uint16_t cursorRow() const { return cursorY_; }
    [[nodiscard]] const TerminalCell& cell(std::uint16_t x, std::uint16_t y) const;

private:
    void put(char32_t character);
    void lineFeed();
    void scroll();
    void eraseDisplay(int mode);
    void eraseLine(int mode);
    void applySgr(const std::vector<int>& parameters);
    void resetStyle();

    std::uint16_t columns_{};
    std::uint16_t rows_{};
    std::uint16_t cursorX_{};
    std::uint16_t cursorY_{};
    TerminalCell style_{};
    std::vector<TerminalCell> cells_;
};

} // namespace Platform
