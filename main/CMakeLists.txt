idf_component_register(
    SRCS "mtg_receiver.cpp" "mtg_display.cpp"
    PRIV_REQUIRES
        esp_driver_uart
        esp_driver_spi
        esp_driver_ledc
        esp_lcd
        esp_timer
        heap
        spiffs
        espressif__cjson
        espressif__esp_lcd_sh8601
)
