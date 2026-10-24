idf_component_register(
    SRCS "main.c"
    REQUIRES
        esp_driver_dma
        esp_driver_ppa
        esp_lcd
        esp_mm
        esp_timer
        pocket_display_esp_dpi
        pocket_guest_quickjs
        pocket_render_esp_ppa
        pocket_runner
        pocket_runtime
)

set(POCKETJS_TAB5_APP "hero" CACHE STRING "Embedded app: hero or clear")
pocketjs_compile_app(
    TARGET ${COMPONENT_LIB}
    NAME app
    MANIFEST "../../../apps/${POCKETJS_TAB5_APP}/pocket.tab5.json"
    RUNTIME_CONFIG "../${POCKETJS_TAB5_APP}.runtime-config.ts"
    PROJECT_ROOT "../../..")
