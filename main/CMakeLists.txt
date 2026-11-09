idf_component_register(SRCS "wifi_bench_main.c"
                    PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_timer
                    INCLUDE_DIRS ".")
