# ESP32SwissArmy component registration.
#
# Source/include layout: one subfolder per subsystem under main/src, each
# holding exactly one .c/.h pair per module.
#
# certs/server_cert.pem + certs/server_key.pem are a dev-only self-signed
# pair (see main/certs/ - regenerate per-device for anything beyond local
# development); embedded so the HTTP Server can load them at sas_http_start()
# without a filesystem dependency.

idf_component_register(
    SRC_DIRS
        "src"
        "src/core"
        "src/network"
        "src/storage"
        "src/web"
        "src/terminal"
        "src/auth"
    INCLUDE_DIRS
        "src"
        "src/core"
        "src/network"
        "src/storage"
        "src/web"
        "src/terminal"
        "src/auth"
    EMBED_TXTFILES
        "certs/server_cert.pem"
        "certs/server_key.pem"
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_timer
        bt
        esp_http_server
        esp_https_server
        esp_ringbuf
        esp_http_client
        esp_security
        esp_app_format
        app_update
        efuse
        mbedtls
        spi_flash
        joltwallet__littlefs
)
