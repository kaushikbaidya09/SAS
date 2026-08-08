# ESP32SwissArmy component registration.
#
# Source/include layout: one subfolder per subsystem under main/src, each
# holding exactly one .c/.h pair per module.
#
# TODO: once the HTTP Server's self-signed TLS certificate/key are
# provisioned, embed them with:
#   EMBED_TXTFILES "certs/server_cert.pem" "certs/server_key.pem"

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
)
