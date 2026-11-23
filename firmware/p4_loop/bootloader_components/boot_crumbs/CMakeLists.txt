# Boot breadcrumbs: diagnostic, off by default. Enable with the environment
# variable P4LOOP_BOOT_CRUMBS=1 at configure time, after deleting
# build/bootloader and build/bootloader-prefix (the bootloader is a separate
# CMake project and keeps its cache). See crumbs.c.
if(NOT "$ENV{P4LOOP_BOOT_CRUMBS}" STREQUAL "1")
    idf_component_register()
    return()
endif()
idf_component_register(SRCS "crumbs.c" PRIV_REQUIRES esp_hw_support hal)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include"
    "-u boot_crumbs_rtc_clk_init" "-u boot_crumbs_delay")
# Breadcrumbs around each out-of-line step of bootloader_init (see crumbs.c).
# rtc_clk_init and esp_rom_delay_us are wrapped by dcdc_settle, which calls
# the boot_crumbs_* hooks instead.
foreach(fn bootloader_init_mem bootloader_clock_configure bootloader_console_init cache_hal_init
           mmu_hal_init bootloader_flash_update_id bootloader_flash_xmc_startup
           bootloader_read_bootloader_header bootloader_check_bootloader_validity
           bootloader_init_spi_flash bootloader_config_wdt
           # inside bootloader_clock_configure
           esp_rom_output_tx_wait_idle rtc_clk_xtal_freq_update
           rtc_clk_cpu_freq_get_config rtc_clk_cpu_freq_mhz_to_config rtc_clk_cpu_freq_set_config
           rtc_clk_8m_enable rtc_clk_fast_src_set rtc_clk_slow_src_set)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${fn}")
endforeach()
