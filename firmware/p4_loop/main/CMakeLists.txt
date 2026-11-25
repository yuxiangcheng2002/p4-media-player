# Bench frames (tools/make_bench_frames.sh). The output mode is chosen at run
# time (video_mode.h), so the production image embeds the colour-bar frame for
# both resolutions (idle screen, 'k'); flat720_422.jpg is bench/720/flat_422.jpg
# under its own name, because EMBED_FILES symbols come from the file name. The
# full set (1.46 MB, 1080p) costs boot time: the bootloader reads and hashes
# every byte of the image (~0.2 s, 2026-11-23). Full decode bench:
#   idf.py -B build.bench -DP4LOOP_BENCH_FULL=1 build
set(BENCH_FRAMES "bench/1080/flat_422.jpg" "bench/720/flat720_422.jpg")
if(P4LOOP_BENCH_FULL)
    list(APPEND BENCH_FRAMES "bench/1080/photo_422.jpg" "bench/1080/typical_422.jpg"
                             "bench/1080/noisy_422.jpg" "bench/1080/typical_444.jpg")
endif()

idf_component_register(
    SRCS "main.cpp" "avi_index.cpp" "usb_stick.cpp" "player.cpp" "sd_card.cpp" "bench.cpp" "raw_reader.cpp" "config.cpp" "update.cpp" "hdmi_monitor.cpp" "video_mode.cpp"
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

# The HDMI mode (1080 / 1080p30 / 720) is chosen at run time by mode= in the
# drive's config.txt (video_mode.h); the old P4LOOP_720P and
# P4LOOP_STD_RATES build options are gone.
