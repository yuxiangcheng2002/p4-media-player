#!/usr/bin/env python3
"""Lay out a USB drive for the P4 player (macOS).

Drive layout (read by firmware/p4_loop; the firmware never writes the drive):

  /media/*.avi       videos, played in name order (tools/p4_transcode.py output)
  /config.txt        optional settings (template written if absent)
  /update/*.bin      a new firmware image; installed at the next mount, with
                     rollback if it does not come up (firmware/p4_loop/main/update.h)
  /kit/              the project itself: source, tools, docs, and the firmware
                     binary as /kit/firmware-bin/p4_loop.bin,
                     so whoever holds the drive can see how it works and rebuild

Why videos are copied first: the player reads each video as raw sectors of its
cluster runs. On a freshly formatted drive, large files copied before anything
else are contiguous; replacing videos on a used drive fragments them, which
still plays but reads slower. To change videos: delete everything in /media,
then copy the new set (or re-run this tool with --erase).

Usage:
  tools/make_drive.py /Volumes/P4VIDEO --media clip1.avi clip2.avi \\
      [--firmware firmware/p4_loop/build/p4_loop.bin [--update]] \\
      [--kit DIR | --kit-git URL] [--erase diskN --name P4VIDEO --yes]

--erase reformats the whole disk as MBR + FAT32 (destroys its contents). It
needs --yes, checks every argument before erasing, and refuses anything that
diskutil does not report as an external, removable or ejectable whole disk.
macOS only (diskutil, ditto, dot_clean); elsewhere create the folders by hand
on an MBR + FAT32 drive.
"""
import argparse
import os
import plistlib
import shutil
import subprocess
import sys

CONFIG_TEMPLATE = """\
# P4 player settings. "key = value"; '#' starts a comment. Delete a line for the default.
#
# file = clip.avi     loop only this file from /media (default: every .avi, in name order)
# update = off        ignore /update/*.bin (default: on; a new image there is installed whenever
#                     the drive is mounted, at power-up or when plugged in)
# mode = 720          HDMI output: 1080p30 (default, 30 Hz), 1080 (35 Hz) or 720 (60 Hz).
#                     Use 720 for monitors that show "Out of range" at 1080. Videos must be
#                     encoded at the same resolution (tools/p4_transcode.py --height 720).
#                     A change restarts the player once.
"""

FAT32_MAX = 4 * 1024**3 - 1


def run(cmd, check=True, **kw):
    print("+", " ".join(cmd))
    return subprocess.run(cmd, check=check, **kw)


def copy_file(src, dst):
    # ditto without resource forks / xattrs: otherwise macOS writes "._name"
    # AppleDouble files next to every file on FAT.
    run(["ditto", "--norsrc", "--noextattr", "--noacl", src, dst])


def disk_info(target):
    out = subprocess.run(["diskutil", "info", "-plist", target], capture_output=True, check=True).stdout
    return plistlib.loads(out)


MAX_PLAIN_ERASE = 512 * 1000**3


def check_erasable(disk, force=False):
    """The whole disk must be reported as not internal and as removable,
    ejectable or external; missing or conflicting fields refuse."""
    info = disk_info(disk)
    problems = []
    if not info.get("WholeDisk"):
        problems.append("not a whole disk (give e.g. disk4, not disk4s1)")
    if info.get("Internal", True):
        problems.append("reported as internal (or not reported)")
    if not (info.get("RemovableMedia") or info.get("Ejectable") or info.get("DeviceLocation") == "External"):
        problems.append("not reported as removable, ejectable or external")
    if info.get("VirtualOrPhysical") != "Physical":
        problems.append("not reported as a physical disk")
    if not info.get("DeviceIdentifier"):
        problems.append("no device identifier")
    if not (info.get("TotalSize") or info.get("Size")):
        problems.append("no size reported")
    boot = disk_info("/")
    stores = {boot.get("ParentWholeDisk")} | {s.get("DeviceIdentifier") for s in boot.get("APFSPhysicalStores", [])}
    if info.get("DeviceIdentifier") in stores or any(x and x.startswith(info.get("DeviceIdentifier", "?") + "s")
                                                         for x in stores):
        problems.append("holds the startup volume")
    if problems:
        sys.exit(f"refusing to erase {disk}: " + "; ".join(problems))
    # A typo in the disk number should not wipe a big Mac disk: large disks and
    # disks holding APFS/HFS data need --force on top of --yes.
    listing = subprocess.run(["diskutil", "list", disk], capture_output=True, text=True, check=True).stdout
    mac_fs = any(t in listing for t in ("Apple_APFS", "Apple_HFS", "APFS Volume"))
    size = info.get("TotalSize") or info.get("Size") or 0
    if (size > MAX_PLAIN_ERASE or mac_fs) and not force:
        sys.exit(f"{disk} is {size / 1e9:.0f} GB" + (" with APFS/HFS data" if mac_fs else "") +
                 "; this does not look like a video drive. Add --force if you really mean it.\n" + listing)
    print(listing)
    return info


def erase(disk, name, force=False):
    info = check_erasable(disk, force)
    dev = info["DeviceIdentifier"]  # canonical "diskN", whatever form was given
    run(["diskutil", "eraseDisk", "FAT32", name, "MBRFormat", dev])
    # Find the new volume by device, not by name (another volume may share it).
    part = disk_info(f"{dev}s1")
    mp = part.get("MountPoint")
    if not mp:
        sys.exit(f"{disk}s1 did not mount after erasing")
    return mp


def check_volume(vol):
    """An existing volume must be a mounted FAT volume on a non-internal disk."""
    if not os.path.ismount(vol) or os.path.realpath(vol) == "/":
        sys.exit(f"{vol} is not a mounted volume")
    info = disk_info(vol)
    if info.get("Internal", True):
        sys.exit(f"{vol} is on an internal disk; refusing")
    if info.get("FilesystemType") != "msdos":
        sys.exit(f"{vol} is {info.get('FilesystemType')}, not FAT (format MBR + FAT32 first, or use --erase)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("volume", nargs="?", help="mounted drive, e.g. /Volumes/P4VIDEO (omit with --erase)")
    ap.add_argument("--media", nargs="*", default=[], help="videos to copy into /media, in this order")
    ap.add_argument("--firmware", help="p4_loop.bin to put in /kit/firmware-bin/")
    ap.add_argument("--update", action="store_true", help="also put --firmware into /update/ (installs at next boot)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--kit", help="directory to copy into /kit (e.g. a checkout of the public repo)")
    g.add_argument("--kit-git", help="git URL to clone into /kit (shallow)")
    ap.add_argument("--erase", metavar="DISK", help="reformat this whole disk (e.g. disk4) as MBR + FAT32 first")
    ap.add_argument("--name", default="P4VIDEO", help="volume name for --erase (FAT: max 11 chars, upper case)")
    ap.add_argument("--yes", action="store_true", help="confirm --erase")
    ap.add_argument("--force", action="store_true", help="allow --erase of a disk over 512 GB or holding APFS/HFS data")
    a = ap.parse_args()

    # Check everything before touching any disk.
    if a.update and not a.firmware:
        sys.exit("--update needs --firmware")
    for m in a.media:
        if not os.path.isfile(m):
            sys.exit(f"{m}: no such file")
        if os.path.getsize(m) > FAT32_MAX:
            sys.exit(f"{m} is over 4 GB, the FAT32 file limit: split it or lower --quality/--max-kb")
    if a.firmware and not os.path.isfile(a.firmware):
        sys.exit(f"{a.firmware}: no such file")
    if a.kit and not os.path.isdir(a.kit):
        sys.exit(f"{a.kit}: no such directory")
    if not (1 <= len(a.name) <= 11) or not all(c.isascii() and (c.isupper() or c.isdigit() or c in "_-") for c in a.name):
        sys.exit("--name: FAT volume names are 1-11 characters, A-Z 0-9 _ -")

    if a.erase:
        if not a.yes:
            sys.exit("--erase destroys everything on the disk; add --yes to confirm")
        if a.volume:
            sys.exit("give either a volume or --erase DISK, not both")
        vol = erase(a.erase, a.name, a.force)
    elif a.volume:
        vol = a.volume
        check_volume(vol)
    else:
        sys.exit("give the mounted volume, or --erase DISK")

    # Keep Spotlight from writing its index onto the drive.
    open(os.path.join(vol, ".metadata_never_index"), "w").close()

    media = os.path.join(vol, "media")
    os.makedirs(media, exist_ok=True)
    for m in a.media:  # first, so they are contiguous on a fresh volume
        copy_file(m, os.path.join(media, os.path.basename(m)))

    os.makedirs(os.path.join(vol, "update"), exist_ok=True)
    cfg = os.path.join(vol, "config.txt")
    if not os.path.exists(cfg):
        with open(cfg, "w") as f:
            f.write(CONFIG_TEMPLATE)

    kit = os.path.join(vol, "kit")
    if a.kit_git:
        if os.path.isdir(os.path.join(kit, ".git")):
            run(["git", "-C", kit, "pull", "--ff-only"])
        else:
            run(["git", "clone", "--depth", "1", a.kit_git, kit])
    elif a.kit:
        if os.path.exists(kit):
            shutil.rmtree(kit)
        run(["ditto", "--norsrc", "--noextattr", "--noacl", a.kit, kit])
    if a.firmware:
        os.makedirs(os.path.join(kit, "firmware-bin"), exist_ok=True)
        copy_file(a.firmware, os.path.join(kit, "firmware-bin", "p4_loop.bin"))
        if a.update:
            copy_file(a.firmware, os.path.join(vol, "update", "p4_loop.bin"))

    run(["dot_clean", "-m", vol], check=False)  # drop AppleDouble files; Spotlight dirs may refuse
    print(f"\n{vol}:")
    for root, dirs, files in os.walk(vol):
        dirs[:] = [d for d in dirs if not d.startswith(".") and not (root == vol and d == "kit")]
        for fn in sorted(files):
            if fn.startswith("."):
                continue
            p = os.path.join(root, fn)
            print(f"  {os.path.relpath(p, vol):50s} {os.path.getsize(p) / 1e6:9.1f} MB")
    if os.path.isdir(kit):
        print(f"  kit/ ({sum(len(f) for _, _, f in os.walk(kit))} files)")


if __name__ == "__main__":
    main()
