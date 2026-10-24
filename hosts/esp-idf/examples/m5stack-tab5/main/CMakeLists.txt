idf_component_register(
    SRCS "main.c"
    REQUIRES
        esp_driver_dma
        esp_driver_ppa
        esp_mm
        esp_lcd
        pocketjs_esp32p4_ppa
        pocketjs_guest
        pocketjs_package
        pocketjs_render_rgb565
        pocketjs_runner
        pocketjs_system
        pocketjs_ui_core
        pocketjs_ui_qjs
)

set(POCKETJS_TAB5_APP "hero" CACHE STRING "Embedded app: hero, clear or home")
if(POCKETJS_TAB5_APP STREQUAL "home")
    # Pocket Home is built in pocket-shell (`bun run --cwd shells/3ds tab5
    # home`) against home.host.json.
    set(POCKETJS_TAB5_HOME_PACKAGE "" CACHE FILEPATH "Pocket Home's home-main.pocket")
    if(NOT POCKETJS_TAB5_HOME_PACKAGE)
        message(FATAL_ERROR "POCKETJS_TAB5_APP=home needs -DPOCKETJS_TAB5_HOME_PACKAGE=<home-main.pocket>")
    endif()
    pocketjs_embed_package(
        TARGET ${COMPONENT_LIB}
        NAME app
        PACKAGE "${POCKETJS_TAB5_HOME_PACKAGE}"
        HOST_PROFILE "../home.host.json")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TAB5_LAUNCHER=1)
else()
    pocketjs_compile_app(
        TARGET ${COMPONENT_LIB}
        NAME app
        MANIFEST "../../../../../apps/${POCKETJS_TAB5_APP}/pocket.tab5.json"
        HOST_PROFILE "../${POCKETJS_TAB5_APP}.host.json"
        PROJECT_ROOT "../../../../..")
endif()
