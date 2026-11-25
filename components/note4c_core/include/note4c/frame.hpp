#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace note4c {
// NOTE4C wire format: four pixels per byte, leftmost pixel in bits 7:6.
enum class Color : uint8_t { Black = 0, White = 1, Yellow = 2, Red = 3 };
struct Frame {
    static constexpr int width = 400, height = 300;
    static constexpr size_t stride = 100, size = stride * height;
    std::array<uint8_t, size> bytes{};
    Frame() { clear(Color::White); }
    void clear(Color color);
    void pixel(int x, int y, Color color);
    Color pixel(int x, int y) const;
    void rect(int x, int y, int w, int h, Color color);
    void text(int x, int y, std::string_view text, Color color, int scale = 2);
    bool operator==(const Frame& other) const { return bytes == other.bytes; }
    bool operator!=(const Frame& other) const { return !(*this == other); }
};

// A small view model is the boundary between applications and the renderer.
// Applications never see SPI, GPIO, framebuffers, or refresh operations.
struct Screen {
    std::string title;
    std::array<std::string, 5> lines{};
    Color accent = Color::Red;
    bool operator==(const Screen& o) const {
        return title == o.title && lines == o.lines && accent == o.accent;
    }
};
class ScreenRenderer {
public:
    void draw(const Screen& screen, Frame& frame) const;
};
} // namespace note4c
