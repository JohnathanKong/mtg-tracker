# Extract 13 independently generated transparent art assets at native dimensions.
# Python standard library only. Artwork is supplied in assets/ as compressed RGBA.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(MTG_GENERATED_DIR "${CMAKE_CURRENT_LIST_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_ui_assets.py"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/mtg_ornate_firmware_assets.zip")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../tools/gen_ui_assets.py"
            "${MTG_GENERATED_DIR}"
    RESULT_VARIABLE MTG_ASSET_RESULT
    OUTPUT_VARIABLE MTG_ASSET_OUTPUT
    ERROR_VARIABLE MTG_ASSET_ERROR
)
if(NOT MTG_ASSET_RESULT EQUAL 0)
    message(FATAL_ERROR "MTG HUD asset generation failed: ${MTG_ASSET_ERROR}")
endif()
message(STATUS "${MTG_ASSET_OUTPUT}")

idf_component_register(
    SRCS "mtg_receiver.cpp" "mtg_display.cpp" "mtg_game.cpp"
    EMBED_FILES
        "ui_font_atlas.lz4"
        "hud_chrome.rgba"
        "player_normal.rgba"
        "player_selected.rgba"
        "counter_teal.rgba"
        "counter_brass.rgba"
        "counter_teal_selected.rgba"
        "counter_brass_selected.rgba"
        "pencil.rgba"
        "editor.rgba"
        "editor_row_checked.rgba"
        "editor_row_unchecked.rgba"
        "checkbox_checked.rgba"
        "checkbox_unchecked.rgba"
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
