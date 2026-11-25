#pragma once
// HDMI link monitor: watches what the P4 can observe of the HDMI side through
// the LT8912B and prints it on the console, so a screen that stays dark can be
// told apart from a cable, a sleeping monitor, or a mode the monitor refuses.
//
// Lines it prints (grep for "HDMI"):
//   HDMI_OUT  ...  the mode we send (fixed at build time) and its line/frame rates
//   HDMI hpd=...   hot-plug detect plus the bridge's own input/clock readbacks,
//                  on every change of hot-plug and every 10 s
//   HDMI_EDID ...  the monitor's EDID, if the DDC lines reach our I2C bus
//                  (probed at start and on every hot-plug rise)
//   HDMI_WARN ...  our mode lies outside the range limits the monitor declares

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

void hdmi_monitor_start(i2c_master_bus_handle_t bus, esp_lcd_panel_io_handle_t io_main,
                        esp_lcd_panel_io_handle_t io_cec_dsi, int hact, int vact, int htotal, int vtotal,
                        int pclk_mhz, int vic);

// Last hot-plug state seen (-1 before the first read); for STAT and the console.
int hdmi_monitor_hpd();
