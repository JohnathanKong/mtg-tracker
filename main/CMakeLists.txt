idf_component_register(
    SRCS "mtg_receiver.cpp"
    PRIV_REQUIRES esp_driver_uart spiffs espressif__cjson
)
