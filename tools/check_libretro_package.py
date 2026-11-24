#!/usr/bin/env python3
"""Install a release ZIP in a fresh profile, boot Dirtbike, quit and resume in RetroArch,
then render the shipped shader presets."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

from PIL import Image, ImageChops, ImageStat

PRESETS = ("pom2-composite", "pom2-monochrome", "pom2-monochrome-white", "pom2-rgb-peritel")
GLASS_PASSES = ("pom2-cable", "pom2-bloom-h", "pom2-bloom-v", "pom2-glass", "pom2-present")


def run_game(executable, config, module, disk, directory, evidence, frames, label, extra=()):
    screenshot = directory / (label + ".png")
    log = directory / (label + ".log")
    cmd = [str(executable), "--verbose", "--config", str(config), "--log-file", str(log),
           "-L", str(module), *extra, "--max-frames", str(frames), "--max-frames-ss",
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


def write_config(path, settings):
    path.write_text("".join(f'{key} = "{str(value).replace(chr(92), "/")}"\n'
                            for key, value in settings.items()))


def shader_drivers():
    # Slang presets need a driver with a slang backend. The runners have no
    # GPU: Mesa's llvmpipe gives Linux x64 a GL 4.5 core context, while the
    # arm64 runner's RetroArch could not start glcore (2026-11-24), so Vulkan
    # (lavapipe) is tried next. Windows has Direct3D 11 through its basic
    # render driver; macOS keeps RetroArch's own default.
    return {"linux": ["glcore", "vulkan"], "win32": ["d3d11"]}.get(sys.platform, [None])


def check_shaders(exe, profile, settings, module, disk, evidence):
    """Every shipped preset compiles and changes the picture; the core preset loads by itself."""
    base = dict(settings, savestate_auto_save="false", savestate_auto_load="false",
                video_shader_dir=profile / "shaders", video_gpu_screenshot="true",
                auto_shaders_enable="true")
    plain_config, shaded_config = profile / "plain.cfg", profile / "shaded.cfg"
    frames = 300
    plain = None
    for driver in shader_drivers():
        shaded = dict(base, **({"video_driver": driver} if driver else {}))
        write_config(plain_config, dict(shaded, video_shader_enable="false"))
        write_config(shaded_config, dict(shaded, video_shader_enable="true"))
        try:
            with Image.open(run_game(exe, plain_config, module, disk, profile, evidence, frames,
                                     "shader-none")) as image:
                plain = image.convert("RGB")
            print(f"shader checks on the {driver or 'default'} video driver", flush=True)
            break
        except (subprocess.SubprocessError, RuntimeError):
            print(f"the {driver} video driver did not start here", flush=True)
    if plain is None:
        print("SKIP: no video driver with a slang backend starts on this runner; "
              "the presets are rendered on the other platforms", flush=True)
        return False
    runs = [("shader-auto", ())] + [
        (f"shader-{name}", ("--set-shader", str(profile / f"shaders/shaders_slang/pom2/{name}.slangp")))
        for name in PRESETS]
    for label, extra in runs:
        with Image.open(run_game(exe, shaded_config, module, disk, profile, evidence, frames,
                                 label, extra)) as image:
            picture = image.convert("RGB")
        log = (profile / (label + ".log")).read_text(errors="replace")
        if label == "shader-auto" and "presets/POM2/POM2.slangp" not in log.replace("\\", "/"):
            raise RuntimeError("RetroArch did not load the POM2 core preset by itself")
        missing = [p for p in GLASS_PASSES if f"{p}.slang" not in log]
        failed = [line for line in log.splitlines()
                  if "[ERROR]" in line and any(k in line for k in ("Slang", "Shader", "shader", "filter chain"))]
        if missing or failed:
            print(log[-6000:], flush=True)
            raise RuntimeError(f"{label}: passes not compiled {missing}, errors {failed[:3]}")
        if picture.size != plain.size:
            raise RuntimeError(f"{label}: {picture.size} against {plain.size} without shaders")
        # A preset that failed to build leaves RetroArch's stock scaling: the
        # same picture as with shaders off. The glass changes it everywhere
        # the Apple draws (scanlines, the curved edge, the cable, the glow).
        difference = ImageStat.Stat(ImageChops.difference(picture, plain).convert("L")).mean[0]
        brightness = ImageStat.Stat(picture.convert("L")).mean[0]
        print(f"{label}: mean difference {difference:.2f}, mean brightness {brightness:.2f}", flush=True)
        if difference < 0.5 or brightness < 1.0:
            raise RuntimeError(f"{label}: the shader did not change the picture")
    return True


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
        for preset in ["presets/POM2/POM2.slangp"] + [f"shaders_slang/pom2/{p}.slangp" for p in PRESETS]:
            if not (profile / "shaders" / preset).is_file():
                raise RuntimeError(f"missing the shader preset {preset}")
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
        write_config(config, settings)
        disk = args.disk.resolve()
        exe = args.retroarch.resolve()
        evidence = args.output.resolve() if args.output else None
        if evidence:
            evidence.mkdir(parents=True, exist_ok=True)
        first = run_game(exe, config, module, disk, profile, evidence, 150, "loading")
        if logo(first) or not any(p.stat().st_size > 0 for p in (profile / "states").rglob("*.auto")):
            raise RuntimeError("first session did not stop and save during disk loading")
        resumed = run_game(exe, config, module, disk, profile, evidence, 155, "resumed")
        if not logo(resumed):
            raise RuntimeError("second session did not resume loading and reach the Dirtbike title")
        rendered = check_shaders(exe, profile, settings, module, disk, evidence)
        print("PASS: fresh install, cold boot, automatic save, process restart and resumed Dirtbike title"
              + (", core shader preset loaded by itself and every shipped preset rendered" if rendered
                 else " (shader presets not rendered here: no slang-capable video driver)"))


if __name__ == "__main__":
    main()
