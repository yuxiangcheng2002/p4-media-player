#pragma once
// microSD slot on the Display Out add-on, mounted read-only-in-practice at /sd.
//
// Wiring (Display Out schematic V0.2, TF card block, card connector J2):
//   GPIO8 = DAT2, GPIO9 = DAT3, GPIO10 = CMD, GPIO11 = CLK, GPIO12 = DAT0,
//   GPIO13 = DAT1, 10k pull-ups to 3V3 on the add-on, card detect = TF_DET
//   (its P4 GPIO is not resolved yet).
// The same schematic labels the connector-side nets differently
// (SDIO_G8_D0 ... SDIO_G12_CK, SDIO_G13_CMD), and M5's Arduino variant names
// GPIO8-13 for ESP-Hosted with yet another order. So sd_card_mount() tries the
// card-connector mapping first, then the two others, and logs which one worked.
// GPIO8-13 are not the P4's SDMMC slot-0 IOMUX pins, so this uses slot 1
// through the GPIO matrix (fine for SD high speed, 40 MHz, 4-bit).
#include <string>
#include <vector>

#define SD_MOUNT_POINT "/sd"

// Returns true when a card is mounted at /sd.
bool sd_card_mount();
// FatFs drive number of the mounted card, or -1.
int sd_card_drive();
// *.avi files in the card's root, sorted by name.
std::vector<std::string> sd_card_list_avi();
