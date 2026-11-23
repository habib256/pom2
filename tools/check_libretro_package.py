#!/usr/bin/env python3
"""Install a release ZIP in a fresh profile, boot Dirtbike, quit and resume in RetroArch."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from PIL import Image


def run_game(executable, config, module, disk, directory, evidence, frames, label):
    screenshot = directory / (label + ".png")
    log = directory / (label + ".log")
    cmd = [str(executable), "--verbose", "--config", str(config), "--log-file", str(log),
           "-L", str(module), "--max-frames", str(frames), "--max-frames-ss",
           "--max-frames-ss-path", str(screenshot), str(disk)]
    try:
        subprocess.run(cmd, cwd=directory, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
                       check=True, timeout=90)
        if not screenshot.exists():
            raise RuntimeError(f"RetroArch did not capture frame {frames}")
    except (subprocess.SubprocessError, RuntimeError):
        if log.exists():
            print(log.read_text(errors="replace")[-6000:], flush=True)
        raise
    finally:
        if evidence:
            for path in (log, screenshot):
                if path.is_file():
                    shutil.copyfile(path, evidence / path.name)
    return screenshot


def logo(path):
    with Image.open(path) as screenshot:
        # With gpu_screenshot disabled RetroArch captures the core framebuffer.
        im = screenshot.convert("RGB")
        if im.size not in ((280, 192), (560, 192)):
            raise RuntimeError(f"unexpected framebuffer size: {im.size}")
        value = 2166136261
        for y in range(12, 45):
            for x in range(10, 270):
                lit = any(im.getpixel((x * (im.width // 280), y)))
                value = ((value ^ lit) * 16777619) & 0xffffffff
        return value == 0x90f673bd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", type=Path)
    parser.add_argument("retroarch", type=Path)
    parser.add_argument("roms", type=Path)
    parser.add_argument("disk", type=Path)
    parser.add_argument("--output", type=Path, help="directory for evidence")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as name:
        profile = Path(name)
        with zipfile.ZipFile(args.zip) as archive:
            archive.extractall(profile)
        for line in (profile / "doc/pom2/SHA256SUMS").read_text().splitlines():
            digest, path = line.split("  ", 1)
            if hashlib.sha256((profile / path).read_bytes()).hexdigest() != digest:
                raise RuntimeError(f"package checksum mismatch: {path}")
        module, = (profile / "cores").glob("pom2_libretro.*")
        if not (profile / "info/pom2_libretro.info").is_file():
            raise RuntimeError("missing core info")
        # The package ships system/pom2/floppy_samples: the folder may exist.
        if not (profile / "system/pom2/floppy_samples/525_step_1_1.wav").is_file():
            raise RuntimeError("missing the floppy drive samples")
        for folder in ("system/pom2", "saves", "states", "config", "playlists", "logs", "screenshots"):
            (profile / folder).mkdir(parents=True, exist_ok=True)
        for rom in ("apple2e.rom", "apple2e_char.rom"):
            shutil.copyfile(args.roms / rom, profile / "system/pom2" / rom)
        options = profile / "core-options.cfg"
        options.write_text('pom2_fast_disk_loading = "enabled"\n')
        settings = {"system_directory": profile / "system", "savefile_directory": profile / "saves",
                    "savestate_directory": profile / "states", "libretro_directory": profile / "cores",
                    "libretro_info_path": profile / "info", "rgui_config_directory": profile / "config",
                    "playlist_directory": profile / "playlists", "core_options_path": options,
                    "screenshot_directory": profile / "screenshots", "config_save_on_exit": "false",
                    "game_specific_options": "false", "savestate_auto_save": "true",
                    "savestate_auto_load": "true", "savestate_file_compression": "false",
                    "menu_show_start_screen": "false", "pause_nonactive": "false",
                    "video_fullscreen": "false", "video_vsync": "false", "video_gpu_screenshot": "false",
                    "audio_enable": "false", "rewind_enable": "false", "bundle_assets_extract_enable": "false"}
        config = profile / "retroarch.cfg"
        config.write_text("".join(f'{key} = "{str(value).replace(chr(92), "/")}"\n' for key, value in settings.items()))
        disk = args.disk.resolve()
        exe = args.retroarch.resolve()
        evidence = args.output.resolve() if args.output else None
        if evidence:
            evidence.mkdir(parents=True, exist_ok=True)
        first = run_game(exe, config, module, disk, profile, evidence, 300, "loading")
        if logo(first) or not any(p.stat().st_size > 0 for p in (profile / "states").rglob("*.auto")):
            raise RuntimeError("first session did not stop and save during disk loading")
        resumed = run_game(exe, config, module, disk, profile, evidence, 155, "resumed")
        if not logo(resumed):
            raise RuntimeError("second session did not resume loading and reach the Dirtbike title")
        print("PASS: fresh install, cold boot, automatic save, process restart and resumed Dirtbike title")


if __name__ == "__main__":
    main()
