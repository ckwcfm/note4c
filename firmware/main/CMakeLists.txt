idf_component_register(
  SRCS main.cpp board.cpp services.cpp cal_storage.cpp wifi_portal.cpp ics_fetcher.cpp github_ota.cpp web_ui.cpp
  INCLUDE_DIRS .
  REQUIRES note4c_core esp_driver_gpio esp_driver_spi esp_driver_i2c esp_adc
           esp_driver_usb_serial_jtag esp_timer esp_wifi esp_netif esp_event
           nvs_flash esp_psram esp_hw_support esp_http_server esp_http_client
           esp-tls mbedtls lwip app_update)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
