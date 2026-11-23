#pragma once
// Firmware update from the drive (/update/*.bin), with rollback.
//
// Flow at each mount: read the image's app descriptor (256 bytes); if its ELF
// SHA-256 equals the running firmware's, nothing happens. Otherwise the image
// is streamed into the other OTA slot, its appended SHA-256 is checked, and
// it becomes the boot partition; the caller restarts. The new firmware boots
// "pending verify" and confirms itself (see update_start_confirm_watch); a
// reset before that rolls the bootloader back to the previous slot. There is
// no signature check: whoever can write the drive can install firmware
// (set update = off in /config.txt at an unattended show). The drive is only read: the file
// stays in /update, and an image that was installed twice without being
// confirmed is skipped from then on (attempt count in NVS).
enum class UpdateResult { None, Same, Skipped, Applied, Failed };

// mount: "/usb". Blocks while flashing (~1-2 s for a 0.6 MB image).
UpdateResult update_from_drive(const char *mount);

// Call once the display is up: starts the self-confirm watch if this boot
// runs a freshly installed image (no-op otherwise). Healthy = a video frame
// reached the screen (then 3 s more), or 20 s with no video to play. If a
// source with videos was found but no frame ever showed, the image is marked
// invalid and the bootloader returns to the previous firmware.
void update_start_confirm_watch();
// Tell the confirm watch whether the mounted source has videos to play.
void update_note_media(bool present);
