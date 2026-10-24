# quirc and the photo image code are shared with the 3DS host.
set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../../engine/host-services")

idf_component_register(
    SRCS "src/camera.c"
         "src/photos.c"
         "${shared}/photos_image.c"
         "${shared}/quirc/quirc.c"
         "${shared}/quirc/decode.c"
         "${shared}/quirc/identify.c"
         "${shared}/quirc/version_db.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${shared}"
    REQUIRES pocketjs_guest pocketjs_ui_core
    PRIV_REQUIRES esp_driver_jpeg esp_driver_ppa esp_mm esp_timer esp_video log
    REQUIRED_IDF_TARGETS esp32p4
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# Third-party code keeps its own warnings out of the build.
set_source_files_properties(
    "${shared}/quirc/quirc.c" "${shared}/quirc/decode.c"
    "${shared}/quirc/identify.c" "${shared}/quirc/version_db.c"
    PROPERTIES COMPILE_OPTIONS "-w")
