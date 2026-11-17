idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_adc nvs_flash esp_timer esp_wifi esp_netif esp_event esp_http_server)
