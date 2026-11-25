#pragma once
#include "note4c/calendar_config.hpp"

namespace note4c::idf {

class CalendarStorage {
public:
    bool init(); // Never calls nvs_flash_erase on error.
    bool load(CalendarConfig& out);
    bool save(const CalendarConfig& cfg);
    bool ready() const { return ready_; }
private:
    bool ready_ = false;
};

} // namespace note4c::idf
