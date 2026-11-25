#pragma once
// /config.txt on the drive: "key = value" lines (keys case-insensitive), '#'
// at the line start or after a space starts a comment. Read once per mount;
// the firmware never writes the drive.
//
//   file   = clip.avi   loop only this file (name in /media, case-insensitive);
//                       default: every .avi, in name order
//   update = on | off   apply /update/*.bin at mount (default on; only
//                       on/yes/true/1 turn it on)
//   mode   = 1080 | 1080p30 | 720
//                       HDMI output (default 1080p30 = 30 Hz). 1080 = 35 Hz,
//                       M5's timing; 720 = 60 Hz, for
//                       monitors that refuse 1080 ("Out of range"). Videos
//                       must be encoded at the same resolution. A change
//                       restarts the player once.
#include <string>

struct PlayerConfig {
    std::string file;
    bool update = true;
    std::string mode;  // empty = not given (default 1080p30)
};

// Missing file = defaults (returns false); unknown keys are logged and ignored.
bool config_load(const char *path, PlayerConfig &out);
