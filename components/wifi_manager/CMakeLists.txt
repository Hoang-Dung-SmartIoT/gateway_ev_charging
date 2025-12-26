idf_component_register(
    SRCS
        "wifi_manager.c"
        "wifi_storage.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_wifi
        esp_netif
        esp_http_server
        nvs_flash
        json
        driver
)

