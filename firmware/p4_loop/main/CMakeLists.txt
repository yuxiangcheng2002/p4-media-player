# Bench frames match the HDMI mode (tools/make_bench_frames.sh writes both sets).
if(P4LOOP_720P)
    set(BENCH_DIR "bench/720")
else()
    set(BENCH_DIR "bench/1080")
endif()
# Production image embeds only the colour-bar frame (idle screen, 'c' colour
# check): the full set is 1.46 MB, and the bootloader reads and hashes every
# byte of the image on each boot (~0.2 s, 2026-11-23). Full decode bench:
#   idf.py -B build.bench -DP4LOOP_BENCH_FULL=1 build
set(BENCH_FRAMES "${BENCH_DIR}/flat_422.jpg")
if(P4LOOP_BENCH_FULL)
    list(APPEND BENCH_FRAMES "${BENCH_DIR}/photo_422.jpg" "${BENCH_DIR}/typical_422.jpg"
                             "${BENCH_DIR}/noisy_422.jpg" "${BENCH_DIR}/typical_444.jpg")
endif()

idf_component_register(
    SRCS "main.cpp" "avi_index.cpp" "usb_stick.cpp" "player.cpp" "sd_card.cpp" "bench.cpp" "raw_reader.cpp" "config.cpp" "update.cpp"
    INCLUDE_DIRS "."
    EMBED_FILES ${BENCH_FRAMES}
    REQUIRES esp_driver_jpeg esp_lcd esp_timer usb fatfs vfs driver esp_driver_usb_serial_jtag esp_driver_gpio esp_driver_i2c esp_app_format esp_psram sdmmc esp_driver_sdmmc esp_mm app_update nvs_flash mbedtls
)

# Frame-buffer format (see player.h): idf.py -B build.fmt4 -DP4LOOP_FB_FMT=4 build
if(DEFINED P4LOOP_FB_FMT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4LOOP_FB_FMT=${P4LOOP_FB_FMT})
endif()

if(DEFINED P4LOOP_YUV_ORDER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4LOOP_YUV_ORDER=${P4LOOP_YUV_ORDER})
endif()
if(P4LOOP_BENCH_FULL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4LOOP_BENCH_FULL=1)
endif()

# 720p60 fallback build: idf.py -B build.720p -DP4LOOP_720P=1 build
if(P4LOOP_720P)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE P4LOOP_WIDTH=1280 P4LOOP_HEIGHT=720 P4LOOP_REFRESH=60)
endif()
