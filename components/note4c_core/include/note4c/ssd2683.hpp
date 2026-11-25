#pragma once
#include "note4c/frame.hpp"

namespace note4c {
// Synchronous protocol boundary, invoked only by a dedicated display worker.
class PanelIo {
public:
    virtual ~PanelIo() = default;
    virtual bool power(bool on) = 0;
    virtual bool reset(bool high) = 0;
    virtual bool write(bool data, const uint8_t* bytes, size_t size) = 0;
    virtual bool wait_ready(uint32_t timeout_ms) = 0;
    virtual void delay_ms(uint32_t ms) = 0;
};
// Four-color NOTE4C only. Adapted protocol sequence; see THIRD_PARTY_NOTICES.md.
// Intentionally exposes no monochrome mode, partial refresh, or LUT upload.
class Ssd2683 {
public:
    explicit Ssd2683(PanelIo& io) : io_(io) {}
    bool refresh(const Frame& frame);
private:
    bool run(const Frame& frame);
    bool command(uint8_t c) { return io_.write(false, &c, 1); }
    bool data(uint8_t d) { return io_.write(true, &d, 1); }
    bool ready() { return io_.wait_ready(120000); }
    PanelIo& io_;
};
} // namespace note4c
