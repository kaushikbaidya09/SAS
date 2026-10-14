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
        espressif__mdns
)

# The dashboard (web/ at the project root) is gzip-compressed and built into
# the firmware, so one OTA image updates firmware and dashboard together and
# flashing never touches the filesystem. Each file is linked as
# _binary_<name>_gz_start / _end (see sas_http_server.c).
idf_build_get_property(python PYTHON)
foreach(file index.html css/style.css js/dashboard.js js/terminal.js)
    get_filename_component(name ${file} NAME)
    set(src ${PROJECT_DIR}/web/${file})
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz)
    add_custom_command(
        OUTPUT ${gz}
        COMMAND ${python} ${PROJECT_DIR}/tools/gzip_web.py ${src} ${gz}
        DEPENDS ${src} ${PROJECT_DIR}/tools/gzip_web.py
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY)
endforeach()
