#include "note4c/ssd2683.hpp"

namespace note4c {
bool Ssd2683::refresh(const Frame& frame) {
    const bool ok = run(frame);
    // Always remove panel power, including after SPI errors or BUSY timeout.
    const bool off = io_.power(false);
    return ok && off;
}
bool Ssd2683::run(const Frame& frame) {
    if (!io_.power(true)) return false;
    io_.delay_ms(10);
    if (!io_.reset(true)) return false;
    io_.delay_ms(10);
    if (!io_.reset(false)) return false;
    io_.delay_ms(20);
    if (!io_.reset(true)) return false;
    io_.delay_ms(10);
    if (!ready() || !command(0xE9) || !data(0x01) || !command(0x10) || !ready())
        return false;
    for (int row = 0; row < Frame::height; ++row) {
        if (!io_.write(true, frame.bytes.data() + row * Frame::stride, Frame::stride))
            return false;
        if (row % 16 == 15) io_.delay_ms(1);
    }
    if (!command(0x04) || !ready()) return false;
    io_.delay_ms(10);
    if (!command(0x12) || !data(0x00)) return false;
    io_.delay_ms(10);
    if (!ready() || !command(0x02) || !data(0x00) || !ready()) return false;
    io_.delay_ms(20);
    return command(0x07) && data(0xA5);
}
} // namespace note4c
