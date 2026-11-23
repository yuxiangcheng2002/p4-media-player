#pragma once
// /config.txt on the drive: "key = value" lines (keys case-insensitive), '#'
// at the line start or after a space starts a comment. Read once per mount;
// the firmware never writes the drive.
//
//   file   = clip.avi   loop only this file (name in /media, case-insensitive);
//                       default: every .avi, in name order
//   update = on | off   apply /update/*.bin at mount (default on; only
//                       on/yes/true/1 turn it on)
#include <string>

struct PlayerConfig {
    std::string file;
    bool update = true;
};

// Missing file = defaults (returns false); unknown keys are logged and ignored.
bool config_load(const char *path, PlayerConfig &out);
