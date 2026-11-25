#include "board.hpp"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace note4c::idf {
Millis now_ms() { return static_cast<Millis>(esp_timer_get_time()/1000); }
static void output(gpio_num_t pin_number,bool high) {
    ESP_ERROR_CHECK(gpio_hold_dis(pin_number));
    ESP_ERROR_CHECK(gpio_set_level(pin_number,high));
    ESP_ERROR_CHECK(gpio_set_direction(pin_number,GPIO_MODE_OUTPUT));
}
void Board::init() {
    // Assert battery power hold before setting up the rest of the board.
    output(pin::latch,true);
    output(pin::panel_power,false); output(pin::amp,false);
    output(pin::audio_power,false); output(pin::led,true);
    gpio_deep_sleep_hold_dis();
    gpio_config_t inputs{};
    inputs.pin_bit_mask=(1ULL<<pin::confirm)|(1ULL<<pin::up)|(1ULL<<pin::down)|
                       (1ULL<<pin::charge)|(1ULL<<pin::full);
    inputs.mode=GPIO_MODE_INPUT; inputs.pull_up_en=GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&inputs));
    i2c_master_bus_config_t bus{};
    bus.i2c_port=I2C_NUM_0; bus.sda_io_num=pin::sda; bus.scl_io_num=pin::scl;
    bus.clk_source=I2C_CLK_SRC_DEFAULT; bus.glitch_ignore_cnt=7;
    bus.flags.enable_internal_pullup=true;
    if (i2c_new_master_bus(&bus,&i2c_)==ESP_OK) {
        i2c_device_config_t dev{};
        dev.dev_addr_length=I2C_ADDR_BIT_LEN_7; dev.device_address=0x51;
        dev.scl_speed_hz=100000;
        if (i2c_master_bus_add_device(i2c_,&dev,&rtc_)!=ESP_OK) rtc_=nullptr;
    }
    adc_oneshot_unit_init_cfg_t unit{}; unit.unit_id=ADC_UNIT_1;
    if (adc_oneshot_new_unit(&unit,&adc_)==ESP_OK) {
        adc_oneshot_chan_cfg_t channel{};
        channel.atten=ADC_ATTEN_DB_12; channel.bitwidth=ADC_BITWIDTH_DEFAULT;
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_,ADC_CHANNEL_3,&channel)); // GPIO4
        adc_cali_curve_fitting_config_t cali{};
        cali.unit_id=ADC_UNIT_1;
        cali.chan=ADC_CHANNEL_3;
        cali.atten=ADC_ATTEN_DB_12;
        cali.bitwidth=ADC_BITWIDTH_DEFAULT;
        if (adc_cali_create_scheme_curve_fitting(&cali,&adc_cali_)!=ESP_OK) {
            adc_cali_=nullptr;
        }
    }
}
bool Board::pressed(Button b) const {
    const auto p=b==Button::Up ? pin::up : b==Button::Down ? pin::down : pin::confirm;
    return gpio_get_level(p)==0;
}
bool Board::usb_host_connected() const {
    return usb_serial_jtag_is_connected();
}
bool Board::battery_raw(int& value) {
    return adc_ && adc_oneshot_read(adc_,ADC_CHANNEL_3,&value)==ESP_OK;
}
bool Board::read_battery(uint16_t& mv, uint8_t& percent) {
    if (!adc_ || !adc_cali_) return false;
    int sum_mv = 0;
    for (int i = 0; i < 10; ++i) {
        int raw = 0, pin_mv = 0;
        if (adc_oneshot_read(adc_, ADC_CHANNEL_3, &raw) != ESP_OK ||
            adc_cali_raw_to_voltage(adc_cali_, raw, &pin_mv) != ESP_OK) {
            return false;
        }
        sum_mv += pin_mv * 2; // ZECTRIX PCB v1.0 2:1 resistor divider
    }
    const int avg_mv = sum_mv / 10;
    if (avg_mv <= 0) return false;
    const int pct = (-avg_mv * avg_mv + 9016 * avg_mv - 19189000) / 10000;
    mv = static_cast<uint16_t>(avg_mv);
    percent = static_cast<uint8_t>(std::clamp(pct, 0, 100));
    return true;
}
std::string Board::sta_mac() const {
    uint8_t m[6]{};
    if (esp_read_mac(m, ESP_MAC_WIFI_STA) != ESP_OK) return {};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  m[0], m[1], m[2], m[3], m[4], m[5]);
    return buf;
}
bool Board::read_rtc(RtcTime& t) {
    if (!rtc_) return false;
    uint8_t reg=0x02, data[7]{};
    if (i2c_master_transmit_receive(rtc_,&reg,1,data,sizeof(data),50)!=ESP_OK ||
        (data[0]&0x80)) return false;
    auto bcd=[](uint8_t v) { return static_cast<int>((v>>4)*10+(v&15)); };
    auto valid_bcd=[](uint8_t v) { return (v&15)<=9 && (v>>4)<=9; };
    const uint8_t fields[]={static_cast<uint8_t>(data[0]&0x7F),
        static_cast<uint8_t>(data[1]&0x7F),static_cast<uint8_t>(data[2]&0x3F),
        static_cast<uint8_t>(data[3]&0x3F),static_cast<uint8_t>(data[5]&0x1F),data[6]};
    for (auto f:fields) if (!valid_bcd(f)) return false;
    // Deliberately limit this adapter to 2000..2099; century flag is not guessed.
    if (data[5]&0x80) return false;
    RtcTime parsed{2000+bcd(fields[5]),bcd(fields[4]),bcd(fields[3]),
                   bcd(fields[2]),bcd(fields[1]),bcd(fields[0])};
    if (parsed.month<1 || parsed.month>12 || parsed.day<1 || parsed.day>31 ||
        parsed.hour>23 || parsed.minute>59 || parsed.second>59) return false;
    t=parsed; return true;
}
bool Board::write_rtc(const RtcTime& t) {
    if (!rtc_ || t.year < 2000 || t.year > 2099 ||
        t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 ||
        t.hour < 0 || t.hour > 23 || t.minute < 0 || t.minute > 59 ||
        t.second < 0 || t.second > 59) return false;
    auto to_bcd = [](int v) -> uint8_t {
        return static_cast<uint8_t>(((v / 10) << 4) | (v % 10));
    };
    const uint8_t buf[8] = {
        0x02,
        static_cast<uint8_t>(to_bcd(t.second) & 0x7F), // clear VL bit
        to_bcd(t.minute),
        to_bcd(t.hour),
        to_bcd(t.day),
        0, // weekday unused
        static_cast<uint8_t>(to_bcd(t.month) & 0x1F),
        to_bcd(t.year - 2000),
    };
    return i2c_master_transmit(rtc_, buf, sizeof(buf), 50) == ESP_OK;
}
WakeReason Board::wake_reason() const {
    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if (causes & (1u << ESP_SLEEP_WAKEUP_TIMER)) return WakeReason::Timer;
    if (causes & (1u << ESP_SLEEP_WAKEUP_EXT1)) {
        const uint64_t mask = esp_sleep_get_ext1_wakeup_status();
        if (mask & (1ULL << pin::down)) return WakeReason::DownButton;
        if (mask & (1ULL << pin::confirm)) return WakeReason::ConfirmButton;
    }
    return WakeReason::PowerOn;
}
bool Board::deep_sleep(uint32_t wakeup_seconds) {
    // Wait briefly if a button is still physically held to avoid immediate re-wake
    const auto wait_start = now_ms();
    while ((pressed(Button::Confirm) || pressed(Button::Down)) && now_ms() - wait_start < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (pressed(Button::Confirm) || pressed(Button::Down)) return false;

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (esp_sleep_enable_ext1_wakeup_io((1ULL<<pin::confirm)|(1ULL<<pin::down),
                                      ESP_EXT1_WAKEUP_ANY_LOW)!=ESP_OK) return false;
    if (wakeup_seconds > 0) {
        const uint64_t us = static_cast<uint64_t>(wakeup_seconds) * 1000000ULL;
        if (esp_sleep_enable_timer_wakeup(us) != ESP_OK) return false;
    }
    led(false);
    ESP_ERROR_CHECK(gpio_hold_en(pin::latch));
    ESP_ERROR_CHECK(gpio_hold_en(pin::panel_power));
    ESP_ERROR_CHECK(gpio_hold_en(pin::amp));
    ESP_ERROR_CHECK(gpio_hold_en(pin::audio_power));
    gpio_deep_sleep_hold_en();
    ESP_LOGI("board", "Entering deep sleep for %lu s (Confirm/Down wake enabled)",
             static_cast<unsigned long>(wakeup_seconds));
    esp_deep_sleep_start();
    return true;
}
void PanelTransport::init() {
    output(pin::reset,true); output(pin::dc,true); output(pin::cs,true);
    ESP_ERROR_CHECK(gpio_set_direction(pin::busy,GPIO_MODE_INPUT));
    ESP_ERROR_CHECK(gpio_set_pull_mode(pin::busy,GPIO_PULLUP_ONLY));
    spi_bus_config_t bus{};
    bus.mosi_io_num=pin::mosi; bus.miso_io_num=-1; bus.sclk_io_num=pin::clock;
    bus.quadwp_io_num=-1; bus.quadhd_io_num=-1; bus.max_transfer_sz=Frame::stride;
    spi_device_interface_config_t dev{};
    dev.spics_io_num=-1; dev.clock_speed_hz=40*1000*1000; dev.mode=0; dev.queue_size=1;
    // One small internal DMA staging buffer isolates SPI from PSRAM ownership.
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST,&dev,&spi_));
}
bool PanelTransport::power(bool on) { return gpio_set_level(pin::panel_power,on)==ESP_OK; }
bool PanelTransport::reset(bool high) { return gpio_set_level(pin::reset,high)==ESP_OK; }
bool PanelTransport::write(bool data,const uint8_t* bytes,size_t size) {
    if (!spi_ || size>Frame::stride) return false;
    alignas(4) uint8_t staging[Frame::stride]{};
    std::memcpy(staging,bytes,size);
    spi_transaction_t t{}; t.length=size*8; t.tx_buffer=staging;
    if (gpio_set_level(pin::dc,data)!=ESP_OK || gpio_set_level(pin::cs,0)!=ESP_OK) return false;
    const auto result=spi_device_polling_transmit(spi_,&t);
    const auto cs_result=gpio_set_level(pin::cs,1);
    return result==ESP_OK && cs_result==ESP_OK;
}
void PanelTransport::delay_ms(uint32_t ms) {
    vTaskDelay(std::max<TickType_t>(1,pdMS_TO_TICKS(ms)));
}
bool PanelTransport::wait_ready(uint32_t timeout_ms) {
    const auto start=now_ms();
    while (gpio_get_level(pin::busy)==0) {
        if (now_ms()-start>=timeout_ms) {
            ESP_LOGE("panel","BUSY timeout; aborting refresh"); return false;
        }
        delay_ms(20);
    }
    return true;
}
void Display::init() {
    transport_.init(); jobs_=xQueueCreate(1,sizeof(const Frame*));
    ESP_ERROR_CHECK(jobs_ ? ESP_OK : ESP_ERR_NO_MEM);
    const auto ok=xTaskCreate(worker,"note4c_display",4096,this,3,&task_);
    ESP_ERROR_CHECK(ok==pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
bool Display::start_full_refresh(const Frame& frame) {
    const Frame* pointer=&frame;
    result_.store(Transfer::Busy,std::memory_order_release);
    return jobs_ && xQueueSend(jobs_,&pointer,0)==pdTRUE;
}
void Display::worker(void* context) {
    auto& self=*static_cast<Display*>(context);
    Ssd2683 panel(self.transport_); const Frame* frame=nullptr;
    for (;;) {
        if (xQueueReceive(self.jobs_,&frame,portMAX_DELAY)==pdTRUE) {
            const bool ok=panel.refresh(*frame);
            self.result_.store(ok ? Transfer::Success : Transfer::Failure,std::memory_order_release);
        }
    }
}
} // namespace note4c::idf
