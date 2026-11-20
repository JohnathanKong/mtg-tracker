idf_component_register(
    SRCS "mtg_receiver.cpp" "mtg_display.cpp" "mtg_game.cpp"
    EMBED_FILES "ui_skin.lz4"
    PRIV_REQUIRES
        esp_driver_usb_serial_jtag
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c
        nvs_flash
        esp_driver_ledc
        esp_lcd
        esp_timer
        heap
        spiffs
        espressif__cjson
        espressif__esp_lcd_sh8601
)
