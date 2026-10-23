idf_component_register(
    SRCS "main.c"
    REQUIRES
        esp_driver_ppa
        esp_lcd
        pocketjs_esp32p4_ppa
        pocketjs_guest
        pocketjs_package
        pocketjs_render_rgb565
        pocketjs_runner
        pocketjs_ui_core
        pocketjs_ui_qjs
)

set(POCKETJS_TAB5_APP "hero" CACHE STRING "Embedded app: hero or clear")
pocketjs_compile_app(
    TARGET ${COMPONENT_LIB}
    NAME app
    MANIFEST "../../../../../apps/${POCKETJS_TAB5_APP}/pocket.tab5.json"
    HOST_PROFILE "../${POCKETJS_TAB5_APP}.host.json"
    PROJECT_ROOT "../../../../..")
