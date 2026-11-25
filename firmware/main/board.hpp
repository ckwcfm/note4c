#pragma once
#include "note4c/buttons.hpp"
#include "note4c/ssd2683.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace note4c::idf {
// PCB v1.0 NOTE4C only. GPIO0 is a boot-strapping input, never an output.
namespace pin {
constexpr gpio_num_t confirm=GPIO_NUM_0, up=GPIO_NUM_39, down=GPIO_NUM_18;
constexpr gpio_num_t panel_power=GPIO_NUM_6, busy=GPIO_NUM_8, reset=GPIO_NUM_9;
constexpr gpio_num_t dc=GPIO_NUM_10, cs=GPIO_NUM_11, clock=GPIO_NUM_12, mosi=GPIO_NUM_13;
constexpr gpio_num_t latch=GPIO_NUM_17, audio_power=GPIO_NUM_42, amp=GPIO_NUM_46;
constexpr gpio_num_t led=GPIO_NUM_3, charge=GPIO_NUM_2, full=GPIO_NUM_1;
constexpr gpio_num_t sda=GPIO_NUM_47, scl=GPIO_NUM_48;
}
struct RtcTime { int year, month, day, hour, minute, second; };
enum class WakeReason { PowerOn, Timer, ConfirmButton, DownButton };

class Board {
public:
    void init();
    bool pressed(Button button) const;
    bool charging() const { return gpio_get_level(pin::charge)==0; }
    bool usb_host_connected() const;
    void led(bool on) { gpio_set_level(pin::led,on ? 0 : 1); }
    bool read_rtc(RtcTime& time); // Refuses RTC low-voltage flag.
    bool write_rtc(const RtcTime& time); // Writes UTC time to PCF8563, clearing VL flag.
    bool battery_raw(int& value); // Uncalibrated ADC raw reading.
    bool read_battery(uint16_t& mv, uint8_t& percent); // Calibrated 2:1 divider reading.
    std::string sta_mac() const; // Wi-Fi STA MAC address (XX:XX:XX:XX:XX:XX).
    WakeReason wake_reason() const;
    bool deep_sleep(uint32_t wakeup_seconds = 0); // Holds battery latch; wakes on Confirm/Down + optional timer.
private:
    i2c_master_bus_handle_t i2c_=nullptr;
    i2c_master_dev_handle_t rtc_=nullptr;
    adc_oneshot_unit_handle_t adc_=nullptr;
    adc_cali_handle_t adc_cali_=nullptr;
};
class PanelTransport final : public PanelIo {
public:
    void init();
    bool power(bool on) override;
    bool reset(bool high) override;
    bool write(bool data,const uint8_t* bytes,size_t size) override;
    bool wait_ready(uint32_t timeout_ms) override;
    void delay_ms(uint32_t ms) override;
private:
    spi_device_handle_t spi_=nullptr;
};
class Display final : public DisplayPort {
public:
    void init();
    bool start_full_refresh(const Frame& frame) override;
    Transfer poll() override { return result_.load(std::memory_order_acquire); }
private:
    static void worker(void* context);
    PanelTransport transport_;
    QueueHandle_t jobs_=nullptr;
    TaskHandle_t task_=nullptr;
    std::atomic<Transfer> result_{Transfer::Busy};
};
Millis now_ms();
} // namespace note4c::idf
