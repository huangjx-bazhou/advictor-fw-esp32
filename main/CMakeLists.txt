idf_component_register(SRCS "main.c"
        PRIV_REQUIRES esp_wifi esp_pm nvs_flash esp_http_server
        INCLUDE_DIRS ".")

