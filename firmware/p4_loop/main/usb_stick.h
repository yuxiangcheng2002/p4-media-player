#pragma once
// USB stick on the ESP32-P4 USB 2.0 High-Speed OTG controller, mounted at /usb.
//
// Port note: in ESP-IDF v5.4.x the USB Host Library on the P4 drives only the
// HS controller + UTMI PHY (usb_host.c: "We support HS+UTMI only ATM"). On the
// Unit PoE-P4 that is the USB-C port labelled Host (USB2_OTG_DP/DN). The
// Display Out add-on's USB-A port is wired to USB1_DP/DN (a full-speed PHY),
// which this host stack does not drive, and full speed (12 Mbit/s) could not
// feed 1080p MJPEG anyway. So the stick goes into the Unit's USB-C Host port.
#include <string>
#include <vector>

#define USB_MOUNT_POINT "/usb"

// Install the USB host + MSC driver. Non-blocking.
void usb_stick_start();
// Wait until a stick is mounted (or timeout_ms elapses). Returns true when mounted.
bool usb_stick_wait_mounted(int timeout_ms);
bool usb_stick_mounted();
// FatFs drive number of the mounted drive, or -1.
int usb_stick_drive();
// Anything reading the drive holds it; on removal the unmount waits (up to
// 3 s) for every hold to be released, so no reader touches a freed device.
void usb_stick_hold();
void usb_stick_release();
// *.avi files, sorted by name, as paths relative to the drive root: those in
// /media if that folder has any, otherwise those in the root (older drives).
std::vector<std::string> usb_stick_list_avi();
