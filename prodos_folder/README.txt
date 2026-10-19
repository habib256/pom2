prodos_folder/ — shared ProDOS host folder
========================================

POM2 builds a guest-writable ProDOS volume from this directory and mounts
it through the HDV controller. The usual volume name is /HOST/. This volume
contains no boot code: boot ProDOS from another disk first.

How to use
----------
1. Copy ordinary files or subdirectories into prodos_folder/.
2. Open the HDV panel and mount its [host folder] entry.
3. Boot ProDOS and access /HOST/ using the selected controller's slot/drive.
   The controller is not restricted to slot 5.
4. To write guest changes back to the directory, enable "Sync to host folder
   (on eject)". Without that opt-in, guest changes stay in memory. Remount
   the folder to import host edits.

Current limits and behavior
---------------------------
* Recursive directories are supported, with depth and symlink-cycle bounds.
* The root holds 51 entries, including directories: this is a ProDOS limit,
  not a 51-file limit for the whole tree.
* Seedling, sapling and tree files are supported up to 16 MiB minus one byte
  each, subject to volume capacity. Skipped imports are reported in the log.
* Names follow ProDOS's 15-character rules; collisions receive suffixes.
* Host read-only flags, modification times, file type and auxiliary type are
  reflected in the generated volume.
* Concurrent host edits are preserved; conflicting guest copies are saved
  in .pom2-conflicts/ when syncing. Failed exports remain visible as errors.

Metadata and text
-----------------
Extensions infer file type (.bas = BAS, .bin = BIN, .sys = SYS, .txt = TXT,
.int = INT). Portable NAME#TTAAAA suffixes preserve explicit hexadecimal
file type and auxiliary type, for example PIC#062000 for BIN at $2000.
TXT line-ending conversion is optional and off by default: when enabled on
the next mount, ProDOS sees CR and the host sees LF. Binary files are unchanged.

Sessions
--------
A v7 .pom2session captures the volume's current blocks as a detached disk.
Restoring it does not reopen or synchronize this directory. Save another
session to retain subsequent changes to that detached disk.

Reference: docs/USER-GUIDE.md, "Share a ProDOS folder", and
docs/session-format.md in the source repository.
