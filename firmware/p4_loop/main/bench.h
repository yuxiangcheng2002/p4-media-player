#pragma once
// Headless decode bench (experiment 01, tests 3 and 4): decodes JPEG frames
// embedded in flash, first with no scan-out, then with DSI scan-out running,
// so decode speed and the PSRAM cost of scan-out are measured without a
// working HDMI bridge or any storage.
// have_display: the LT8912B came up, scan-out is already running.
void bench_run(bool have_display);
// Show the embedded ffmpeg testsrc frame (known colour bars) on screen: the
// idle screen, and the reference for checking colour order with the 'c' key.
bool bench_show_pattern();
