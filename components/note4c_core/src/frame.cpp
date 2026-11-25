#include "note4c/frame.hpp"
#include <algorithm>
#include <cctype>

namespace note4c {
namespace {
// Original compact 3x5 bitmap alphabet. Each row is three low-order bits.
using Glyph = std::array<uint8_t, 5>;
Glyph glyph(char c) {
    static constexpr Glyph letters[] = {
        {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6},
        {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5},
        {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
        {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2}, {6,5,6,4,4},
        {2,5,5,7,3}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
        {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5},
        {5,5,2,2,2}, {7,1,2,4,7}
    };
    static constexpr Glyph digits[] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {6,1,2,4,7}, {6,1,2,1,6},
        {5,5,7,1,1}, {7,4,6,1,6}, {3,4,6,5,2}, {7,1,2,2,2},
        {2,5,2,5,2}, {2,5,3,1,6}
    };
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case ' ': return {0,0,0,0,0};
    case ':': return {0,2,0,2,0};
    case '-': return {0,0,7,0,0};
    case '+': return {0,2,7,2,0};
    case '.': return {0,0,0,0,2};
    case '/': return {1,1,2,4,4};
    default: return {6,1,2,0,2};
    }
}
}
void Frame::clear(Color c) {
    bytes.fill(static_cast<uint8_t>(static_cast<uint8_t>(c) * 0x55));
}
void Frame::pixel(int x, int y, Color c) {
    if (x < 0 || x >= width || y < 0 || y >= height) return;
    auto& b = bytes[static_cast<size_t>(y) * stride + x / 4];
    const auto shift = (3 - x % 4) * 2;
    b = static_cast<uint8_t>((b & ~(3 << shift)) |
                            ((static_cast<uint8_t>(c) & 3) << shift));
}
Color Frame::pixel(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height) return Color::White;
    const auto b = bytes[static_cast<size_t>(y) * stride + x / 4];
    return static_cast<Color>((b >> ((3 - x % 4) * 2)) & 3);
}
void Frame::rect(int x, int y, int w, int h, Color c) {
    if (w <= 0 || h <= 0) return;
    const auto right = std::min<int64_t>(width, static_cast<int64_t>(x) + w);
    const auto bottom = std::min<int64_t>(height, static_cast<int64_t>(y) + h);
    for (int row = std::max(0, y); row < bottom; ++row)
        for (int col = std::max(0, x); col < right; ++col) pixel(col, row, c);
}
void Frame::text(int x, int y, std::string_view text, Color c, int scale) {
    if (scale < 1 || scale > 8) return;
    int64_t cursor = x;
    for (char ch : text) {
        if (cursor >= width) break;
        const auto g = glyph(ch);
        if (cursor > -4 * scale && y > -5 * scale && y < height) {
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (g[row] & (1 << (2 - col)))
                        rect(static_cast<int>(cursor) + col * scale, y + row * scale,
                             scale, scale, c);
        }
        cursor += 4 * scale;
    }
}
void ScreenRenderer::draw(const Screen& s, Frame& f) const {
    f.clear(Color::White);
    f.rect(0, 0, Frame::width, 56, s.accent);
    f.text(20, 16, s.title, s.accent == Color::Yellow ? Color::Black : Color::White, 5);
    for (size_t i = 0; i < s.lines.size(); ++i)
        f.text(20, 78 + static_cast<int>(i) * 36, s.lines[i], Color::Black, 3);
    f.rect(20, 270, 360, 2, Color::Black);
    f.text(20, 282, "NOTE4C / SLOW DISPLAY / LIVE INPUT", Color::Black, 2);
}
} // namespace note4c
