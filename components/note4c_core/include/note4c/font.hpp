#pragma once
#include "note4c/frame.hpp"
#include <string>
#include <string_view>

namespace note4c {

// Crisp proportional 10px-tall full-ASCII bitmap font (0x20..0x7E)
// with proper lowercase descenders, bold support, measurement, and truncation.
namespace font {

static constexpr int kLineHeight = 10;

int glyph_width(char c);
int measure_text(std::string_view text, int scale = 1, bool bold = false);
std::string truncate_text(std::string_view text, int max_width_px, int scale = 1, bool bold = false);
void draw_text(Frame& frame, int x, int y, std::string_view text,
               Color color, int scale = 1, bool bold = false);

} // namespace font
} // namespace note4c
