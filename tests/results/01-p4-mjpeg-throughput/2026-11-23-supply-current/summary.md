# Supply current of the P4 rig (2026-11-23)

Measured with an FNB-C2 USB-C meter between a laptop's USB-C port
and the Unit PoE-P4's download/power USB-C, while the player looped the
1080p q90 clip.

Rig: Unit PoE-P4 + Display Out (LT8912B, HDMI connected) + a USB-C SD card
reader on the Unit's Host port (powered from the Unit). No Ethernet.

| | value |
|---|---|
| VBUS | 5.233 V |
| current (one reading) | 0.227 A |
| power (one reading) | 1.19 W |
| typical (read off the meter) | ~0.2 A |
| peak (read off the meter) | ~0.3 A |

Conclusion: the whole rig stays under the 500 mA a USB 2.0 port must supply,
so a monitor's USB port should be able to power it. Not yet measured: the
boot inrush with a sampling meter, and a real monitor USB port (some cut
power in standby, which is a power cut for the player).
