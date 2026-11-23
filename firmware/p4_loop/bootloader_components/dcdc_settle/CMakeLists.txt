idf_component_register(SRCS "dcdc_settle.c" PRIV_REQUIRES esp_hw_support hal)
# The wait is added by wrapping two calls made by IDF 5.4.2's rtc_clk_init
# (see dcdc_settle.c), so ESP-IDF itself stays unmodified. -u pulls the
# wrappers in whatever the archive order.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=rtc_clk_init" "-Wl,--wrap=esp_rom_delay_us"
    "-u __wrap_rtc_clk_init" "-u __wrap_esp_rom_delay_us")
# The fix depends on the order of calls inside rtc_clk_init, which upstream
# has changed since 5.4 (PVT-based supply setup before the DCDC switch).
if(NOT "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" STREQUAL "5.4")
    message(WARNING "dcdc_settle was written and tested against ESP-IDF 5.4.2; "
                    "re-check rtc_clk_init (esp_hw_support/port/esp32p4/rtc_clk_init.c) and the cold-boot test")
endif()
