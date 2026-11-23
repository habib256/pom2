// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include <libretro.h>

#include "ByteIO.h"
#include "CpuClock.h"
#include "MediaAutosave.h"
#include "disk_playlist.hpp"
#include "pad_keyboard.hpp"
#include "disk_storage.hpp"
#include "pom2/core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

VideoTiming machineTiming = POM2_TIMING_NTSC;
constexpr unsigned kWidth = 280;
constexpr unsigned kMaxWidth = 560;
constexpr unsigned kHeight = POM2_TIMING_NTSC.visibleScanlines;
constexpr unsigned kSampleRate = 44100;
constexpr unsigned kMaxAudioFrames =
    (kSampleRate * POM2_TIMING_PAL.cyclesPerFrame + POM2_TIMING_PAL.cpuClockHz - 1) / POM2_TIMING_PAL.cpuClockHz;
constexpr unsigned kMaxDiskLoadFrames = 12;
constexpr char kFastDiskLoading[] = "pom2_fast_disk_loading";

retro_environment_t environment = nullptr;
retro_video_refresh_t video = nullptr;
retro_audio_sample_t audioSample = nullptr;
retro_audio_sample_batch_t audioBatch = nullptr;
retro_input_poll_t pollInput = nullptr;
retro_input_state_t inputState = nullptr;
retro_log_printf_t logMessage = nullptr;
retro_led_interface driveLed{};
bool driveLedOn = false;

void showDiskRead(bool active)
{
    if (driveLed.set_led_state && active != driveLedOn) {
        driveLed.set_led_state(0, active ? 1 : 0);
        driveLedOn = active;
    }
}

std::unique_ptr<pom2::Core> core;

struct MountedDisk {
    std::filesystem::path path;
    bool smartPort;
    bool writable;
    std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
};

struct DiskState {
    std::vector<pom2::libretro::DiskEntry> images;
    std::optional<unsigned> index;
    bool ejected = false;
    bool controllerAttached = false;
    bool smartPortAttached = false;
    std::filesystem::path firmware;
    std::filesystem::path saves;
    std::filesystem::path content;
    std::optional<MountedDisk> mounted;
    unsigned framesSinceFlush = 0;
    bool flushWarning = false;
};

struct InitialDisk {
    unsigned index;
    std::filesystem::path path;
};

DiskState disks;
std::optional<InitialDisk> initialDisk;

std::array<std::uint32_t, kMaxWidth * kHeight> videoPixels;
std::array<float, 2 * kMaxAudioFrames> audioFloat;
std::array<std::int16_t, 2 * kMaxAudioFrames> audioPcm;
int cycleOvershoot = 0;
unsigned machineAudioRemainder = 0;
unsigned frontendAudioRemainder = 0;
bool fastDiskLoading = true;
std::size_t stateCapacity = 0;
std::vector<std::uint8_t> stateScratch;

bool joypadEnabled = true;
bool openApple = false;
bool solidApple = false;
bool startHeld = false;
bool selectHeld = false;
pom2::libretro::PadKeyboard padKeyboard;
unsigned padButtons = 0;
bool supportsPadMask = false;
int paddleDeadZone = 4096;
float paddleSensitivity = 1.0f;

retro_input_descriptor inputDescriptors[] = {
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
        RETRO_DEVICE_ID_ANALOG_X, "Joystick X"},
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
        RETRO_DEVICE_ID_ANALOG_Y, "Joystick Y"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick Left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick Up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick Down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 0 / Open Apple"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 1 / Solid Apple"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Return"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Escape"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Show / hide Apple keyboard"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Next disk"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Keyboard shift"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Keyboard delete"},
    {}
};

retro_variable options[] = {
    {"pom2_machine_profile", "Machine profile (restart); auto|ntsc|dix-pal"},
    {"pom2_mouse", "AppleMouse in slot 4 (restart); disabled|enabled"},
    {"pom2_paddle_deadzone", "Analog paddle dead zone; 12.5%|0%|25%"},
    {"pom2_paddle_sensitivity", "Analog paddle sensitivity; 100%|75%|125%|150%"},
    {kFastDiskLoading, "Fast disk loading; enabled|disabled"},
    {}
};

std::string_view optionValue(const char* key)
{
    retro_variable option{key, nullptr};
    if (environment && environment(RETRO_ENVIRONMENT_GET_VARIABLE, &option) && option.value)
        return option.value;
    return {};
}

bool useDixProfile(const std::vector<pom2::libretro::DiskEntry>& images)
{
    const auto profile = optionValue("pom2_machine_profile");
    if (profile == "dix-pal") return true;
    if (profile == "ntsc" || images.empty() || !images.front().smartPort()) return false;
    // The bundled DIX and the reference corpus image have distinct file hashes.
    const auto hash = pom2::Core::sha256File(images.front().path.string());
    return hash == "e28da6f871e9564fca0665f69d4806b258d0bbaef73ba1f9679a53c56eac9dc4"
        || hash == "e375db9d722454ec1c75dff32386c5d92f15847f8390552acd081a69991e18df";
}

void readOptions()
{
    const auto zone = optionValue("pom2_paddle_deadzone");
    paddleDeadZone = zone == "0%" ? 0 : zone == "25%" ? 8192 : 4096;
    const auto gain = optionValue("pom2_paddle_sensitivity");
    paddleSensitivity = gain == "75%" ? .75f : gain == "125%" ? 1.25f : gain == "150%" ? 1.5f : 1.0f;
    retro_variable variable{kFastDiskLoading, nullptr};
    fastDiskLoading = !environment || !environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable)
        || !variable.value || std::string_view(variable.value) != "disabled";
}

void reportError(const std::string& detail)
{
    if (logMessage)
        logMessage(RETRO_LOG_ERROR, "POM2: %s\n", detail.c_str());
    else
        std::fprintf(stderr, "POM2: %s\n", detail.c_str());

    if (environment) {
        retro_message message{detail.c_str(), 300};
        environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
    }
}

unsigned appleKey(unsigned keycode, std::uint32_t character, std::uint16_t modifiers)
{
    switch (keycode) {
    case RETROK_RETURN:
    case RETROK_KP_ENTER:  return 0x0d;
    case RETROK_BACKSPACE:
    case RETROK_LEFT:      return 0x08;
    case RETROK_RIGHT:     return 0x15;
    case RETROK_UP:        return 0x0b;
    case RETROK_DOWN:      return 0x0a;
    case RETROK_ESCAPE:    return 0x1b;
    case RETROK_TAB:       return 0x09;
    case RETROK_DELETE:    return 0x7f;
    default:              break;
    }
    if ((modifiers & RETROKMOD_CTRL) && keycode >= RETROK_a && keycode <= RETROK_z)
        return keycode - RETROK_a + 1;
    if (character >= 0x20 && character <= 0x7e)
        return character;
    if (character == 0 && keycode >= RETROK_SPACE && keycode <= RETROK_z) {
        const bool uppercase = bool(modifiers & RETROKMOD_SHIFT)
                            != bool(modifiers & RETROKMOD_CAPSLOCK);
        return uppercase && keycode >= RETROK_a ? keycode - 'a' + 'A' : keycode;
    }
    return 0;
}

void RETRO_CALLCONV keyboardEvent(bool down, unsigned keycode,
                                 std::uint32_t character, std::uint16_t modifiers)
{
    if (!core) return;
    if (keycode == RETROK_LALT) openApple = down;
    if (keycode == RETROK_RALT) solidApple = down;

    const unsigned ascii = appleKey(keycode, character, modifiers);
    // The libretro keyboard keycodes fit in the SDK's 512 physical key tokens.
    if (keycode != RETROK_UNKNOWN && keycode < 512 && (ascii != 0 || !down))
        core->setKeyHeld(static_cast<std::uint16_t>(keycode), down);
    if (down && ascii != 0)
        core->queueKey(static_cast<std::uint8_t>(ascii));
}

bool padPressed(unsigned id)
{
    return (padButtons & (1u << id)) != 0;
}

float padAxis(unsigned id, unsigned negative, unsigned positive)
{
    const int dpad = int(padPressed(positive)) - int(padPressed(negative));
    if (dpad != 0) return static_cast<float>(dpad);
    if (!joypadEnabled || !inputState) return 0.0f;
    const auto value = inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, id);
    if (std::abs(int(value)) < paddleDeadZone) return 0.0f;
    return std::clamp(paddleSensitivity * value / (value < 0 ? 32768.0f : 32767.0f), -1.0f, 1.0f);
}

void nextDisk();

void applyInput()
{
    padButtons = 0;
    if (joypadEnabled && inputState) {
        if (supportsPadMask) {
            padButtons = static_cast<std::uint16_t>(inputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
        } else {
            for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
                if (inputState(0, RETRO_DEVICE_JOYPAD, 0, id)) padButtons |= 1u << id;
        }
    }
    if (core->mouseAttached() && inputState) {
        const auto mouse = [](unsigned id) { return inputState(0, RETRO_DEVICE_MOUSE, 0, id); };
        core->moveMouse(mouse(RETRO_DEVICE_ID_MOUSE_X), mouse(RETRO_DEVICE_ID_MOUSE_Y), mouse(RETRO_DEVICE_ID_MOUSE_LEFT) != 0);
    }
    const bool wasOpen = padKeyboard.open;
    const bool swap = padPressed(RETRO_DEVICE_ID_JOYPAD_R3) && !(padKeyboard.held & (1u << RETRO_DEVICE_ID_JOYPAD_R3));
    const auto key = padKeyboard.update(static_cast<std::uint16_t>(padButtons));
    if (key) core->queueKey(key);
    if (wasOpen || padKeyboard.open) {
        core->setJoystickAxes(0, 0);
        core->setPaddleButton(0, openApple);
        core->setPaddleButton(1, solidApple);
        startHeld = padPressed(RETRO_DEVICE_ID_JOYPAD_START);
        selectHeld = padPressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
        return;
    }
    if (swap) nextDisk();
    core->setJoystickAxes(
        padAxis(RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT),
        padAxis(RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN));
    core->setPaddleButton(0, openApple || padPressed(RETRO_DEVICE_ID_JOYPAD_B));
    core->setPaddleButton(1, solidApple || padPressed(RETRO_DEVICE_ID_JOYPAD_A));

    const bool start = padPressed(RETRO_DEVICE_ID_JOYPAD_START);
    const bool select = padPressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (start && !startHeld) core->queueKey(0x0d);
    if (select && !selectHeld) core->queueKey(0x1b);
    startHeld = start;
    selectHeld = select;
}

void clearInput()
{
    padKeyboard = {};
    padButtons = 0;
    openApple = false;
    solidApple = false;
    startHeld = false;
    selectHeld = false;
    if (core) core->releaseHeldKeys();
}

void renderVideo()
{
    if (!video) return;
    const auto frame = core->renderFrame();
    const auto width = static_cast<unsigned>(frame.width);
    const auto height = static_cast<unsigned>(frame.height);
    const auto count = width * height;
    for (unsigned i = 0; i < count; ++i) {
        const auto rgba = frame.pixels[i];
        // The SDK packs bytes as 0xAABBGGRR and libretro expects 0x00RRGGBB.
        videoPixels[i] = ((rgba & 0xff) << 16) | (rgba & 0xff00) | ((rgba >> 16) & 0xff);
    }
    padKeyboard.draw(videoPixels.data(), width, height);
    video(videoPixels.data(), width, height, width * sizeof(videoPixels[0]));
}

unsigned audioFramesForTick(unsigned& remainder)
{
    // Distribute fractional samples over consecutive video frames.
    remainder += kSampleRate * machineTiming.cyclesPerFrame;
    const auto frames = remainder / machineTiming.cpuClockHz;
    remainder %= machineTiming.cpuClockHz;
    return frames;
}

void outputAudio(unsigned machineFrames)
{
    const auto frames = audioFramesForTick(frontendAudioRemainder);
    // The machine and frontend have different fractional phases after a burst.
    // The final emulated frame is at most one sample shorter or longer.
    if (frames > machineFrames) {
        audioFloat[2 * machineFrames] = audioFloat[2 * (machineFrames - 1)];
        audioFloat[2 * machineFrames + 1] = audioFloat[2 * (machineFrames - 1) + 1];
    }
    for (unsigned i = 0; i < 2 * frames; ++i)
        audioPcm[i] = static_cast<std::int16_t>(
            std::lround(std::clamp(audioFloat[i], -1.0f, 1.0f) * 32767.0f));

    if (audioBatch) {
        audioBatch(audioPcm.data(), frames);
    } else if (audioSample) {
        for (unsigned i = 0; i < 2 * frames; i += 2)
            audioSample(audioPcm[i], audioPcm[i + 1]);
    }
}

unsigned runFrame()
{
    // Charge any extra cycles at the end of a 6502 instruction to the next frame.
    const int budget = machineTiming.cyclesPerFrame - cycleOvershoot;
    cycleOvershoot = core->run(budget) - budget;
    const auto frames = audioFramesForTick(machineAudioRemainder);
    core->pullAudio(audioFloat.data(), frames, kSampleRate);
    return frames;
}

std::string optionalRom(const std::filesystem::path& path)
{
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

bool mountDisk(pom2::Core& machine, DiskState& state, const pom2::libretro::DiskEntry& image)
{
    const bool smartPort = image.smartPort();
    pom2::libretro::DiskStorage storage{image.path};
    std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
    try {
        storage = pom2::libretro::openDiskStorage(state.saves, state.content, image.path);
        if (storage.privateCopy) lease = pom2::libretro::lockDiskStorage(storage.path);
    } catch (const std::exception& error) {
        storage = {image.path};
        if (logMessage) logMessage(RETRO_LOG_WARN, "POM2: Disk mounted read-only: %s\n", error.what());
    }
    if (storage.privateCopy && !lease && logMessage)
        logMessage(RETRO_LOG_WARN, "POM2: Disk copy is locked or cannot be locked, mounting read-only: %s\n",
                   storage.path.string().c_str());
    if (smartPort && !state.smartPortAttached) {
        if (!machine.attachSmartPort(5)) {
            reportError(machine.lastError());
            return false;
        }
        state.smartPortAttached = true;
    }
    if (!smartPort && !state.controllerAttached) {
        if (!machine.attachDiskII(optionalRom(state.firmware / "disk2.rom"),
                                  optionalRom(state.firmware / "diskii_p6.rom"))) {
            reportError(machine.lastError());
            return false;
        }
        state.controllerAttached = true;
    }

    const auto path = storage.path.string();
    if (!(smartPort ? machine.insertSmartPortDisk(0, path) : machine.insertDisk(0, path))) {
        reportError("Cannot insert disk " + image.path.string() + ": " + machine.lastError());
        return false;
    }
    // Only a private copy with a write lease may accept guest writes.
    if (!lease && !(smartPort ? machine.setSmartPortDiskWriteProtected(0, true)
                             : machine.setDiskWriteProtected(0, true))) {
        const auto reason = machine.lastError();
        if (smartPort) machine.ejectSmartPortDisk(0);
        else machine.ejectDisk(0);
        reportError("Cannot write-protect disk " + path + ": " + reason);
        return false;
    }
    const bool protectedDisk = smartPort ? machine.smartPortDiskWriteProtected(0)
                                         : machine.diskWriteProtected(0);
    if (protectedDisk) lease.reset();
    state.mounted = MountedDisk{storage.path, smartPort, !protectedDisk, std::move(lease)};
    state.framesSinceFlush = 0;
    state.flushWarning = false;
    return true;
}

bool flushMountedDisk(pom2::Core& machine, const DiskState& state)
{
    if (!state.mounted || !state.mounted->writable) return true;
    return state.mounted->smartPort ? machine.flushSmartPortDisk(0) : machine.flushDisk(0);
}

bool ejectMountedDisk(pom2::Core& machine, DiskState& state)
{
    if (!state.mounted) return true;
    if (!flushMountedDisk(machine, state)) return false;
    const bool ejected = state.mounted->smartPort ? machine.ejectSmartPortDisk(0)
                                                  : machine.ejectDisk(0);
    if (ejected) state.mounted.reset();
    return ejected;
}

bool keepDiskEffects()
{
    auto phase = RETRO_EXECUTION_PHASE_RETAINED;
    return !environment(RETRO_ENVIRONMENT_GET_EXECUTION_PHASE, &phase)
        || phase == RETRO_EXECUTION_PHASE_RETAINED;
}

void autosaveMountedDisk()
{
    constexpr unsigned interval = 300;
    if (!disks.mounted || !disks.mounted->writable || !keepDiskEffects()) return;
    if (++disks.framesSinceFlush < interval) return;
    disks.framesSinceFlush = 0;
    try {
        if (flushMountedDisk(*core, disks)) {
            disks.flushWarning = false;
            return;
        }
        if (!disks.flushWarning) reportError(core->lastError());
    } catch (const std::exception& error) {
        if (!disks.flushWarning) reportError(error.what());
    }
    disks.flushWarning = true;
}

bool RETRO_CALLCONV setDiskEjected(bool ejected)
{
    if (disks.ejected == ejected) return true;
    if (!core) return false;
    try {
        if (ejected && !ejectMountedDisk(*core, disks)) {
            reportError(core->lastError());
            return false;
        }
        if (!ejected && disks.index && !mountDisk(*core, disks, disks.images[*disks.index]))
            return false;
        disks.ejected = ejected;
        if (ejected) showDiskRead(false);
        return true;
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

bool RETRO_CALLCONV getDiskEjected() { return disks.ejected; }
unsigned RETRO_CALLCONV getDiskCount() { return static_cast<unsigned>(disks.images.size()); }
unsigned RETRO_CALLCONV getDiskIndex() { return disks.index.value_or(getDiskCount()); }

bool RETRO_CALLCONV setDiskIndex(unsigned index)
{
    if (!disks.ejected) return false;
    if (index >= getDiskCount()) {
        disks.index.reset();
    } else {
        if (disks.images[index].path.empty()) return false;
        disks.index = index;
    }
    return true;
}

void nextDisk()
{
    const unsigned count = getDiskCount();
    if (count < 2 || disks.ejected || !keepDiskEffects()) return;
    const auto previous = getDiskIndex();
    unsigned next = previous < count ? (previous + 1) % count : 0;
    while (next != previous && disks.images[next].path.empty()) {
        next = (next + 1) % count;
        if (next == 0 && previous >= count) return;
    }
    if (next == previous || !setDiskEjected(true)) return;
    if (!setDiskIndex(next) || !setDiskEjected(false)) {
        if (setDiskIndex(previous)) setDiskEjected(false);
        return;
    }
    retro_message message{disks.images[next].label.c_str(), 120};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

bool RETRO_CALLCONV replaceDisk(unsigned index, const retro_game_info* game)
{
    if (!disks.ejected || index >= getDiskCount()) return false;
    if (!game) {
        disks.images.erase(disks.images.begin() + index);
        if (disks.index == index) disks.index.reset();
        else if (disks.index && *disks.index > index) --*disks.index;
        return true;
    }
    if (!game->path || !*game->path) return false;
    try {
        disks.images[index] = pom2::libretro::loadDiskImage(std::filesystem::u8path(game->path));
        return true;
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

bool RETRO_CALLCONV addDisk()
{
    if (!core) return false;
    try {
        disks.images.emplace_back();
        return true;
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

bool RETRO_CALLCONV setInitialDisk(unsigned index, const char* path)
{
    initialDisk.reset();
    if (!path || !*path) return false;
    try {
        initialDisk = InitialDisk{index, std::filesystem::absolute(std::filesystem::u8path(path))};
        return true;
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

bool copyDiskField(const std::string& value, char* buffer, std::size_t length)
{
    if (!buffer || length <= value.size()) return false;
    std::memcpy(buffer, value.c_str(), value.size() + 1);
    return true;
}

bool RETRO_CALLCONV getDiskPath(unsigned index, char* buffer, std::size_t length)
{
    if (index >= getDiskCount() || disks.images[index].path.empty()) return false;
    try {
        return copyDiskField(disks.images[index].path.u8string(), buffer, length);
    } catch (const std::exception&) {
        return false;
    }
}

bool RETRO_CALLCONV getDiskLabel(unsigned index, char* buffer, std::size_t length)
{
    return index < getDiskCount() && !disks.images[index].path.empty()
        && copyDiskField(disks.images[index].label, buffer, length);
}

void registerDiskControl()
{
    unsigned version = 0;
    if (environment(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &version) && version >= 1) {
        retro_disk_control_ext_callback callbacks{
            setDiskEjected, getDiskEjected, getDiskIndex, setDiskIndex,
            getDiskCount, replaceDisk, addDisk, setInitialDisk, getDiskPath, getDiskLabel
        };
        if (environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &callbacks)) return;
    }
    retro_disk_control_callback callbacks{
        setDiskEjected, getDiskEjected, getDiskIndex, setDiskIndex, getDiskCount, replaceDisk, addDisk
    };
    environment(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &callbacks);
}

bool loadGame(const retro_game_info* game, const std::optional<InitialDisk>& requestedDisk)
{
    const char* systemDirectory = nullptr;
    if (!environment || !environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &systemDirectory)
            || !systemDirectory || !*systemDirectory) {
        reportError("Set the frontend system directory and place apple2e.rom and "
                    "apple2e_char.rom in its pom2 subfolder.");
        return false;
    }
    auto pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixelFormat)) {
        reportError("The frontend must support XRGB8888 video.");
        return false;
    }

    const auto firmware = std::filesystem::path(systemDirectory) / "pom2";
    const auto rom = (firmware / "apple2e.rom").string();
    const auto charRom = (firmware / "apple2e_char.rom").string();
    DiskState state;
    state.firmware = firmware;
    const char* saveDirectory = nullptr;
    if (environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory) && saveDirectory && *saveDirectory)
        state.saves = std::filesystem::u8path(saveDirectory);
    if (game) {
        if (!game->path || !*game->path) {
            reportError("A filesystem path is required for a disk image or M3U playlist.");
            return false;
        }
        state.content = std::filesystem::absolute(std::filesystem::u8path(game->path));
        state.images = pom2::libretro::loadDiskList(state.content);
    }
    const bool pal = useDixProfile(state.images);
    pom2::CoreConfig config;
    config.machine = pom2::Machine::AppleIIeEnhanced;
    config.timing = pal ? pom2::VideoTiming::PAL : pom2::VideoTiming::NTSC;
    auto machine = std::make_unique<pom2::Core>(config);
    if (!machine->loadRom(rom)) {
        reportError("Cannot load Apple //e firmware at " + rom + ": " + machine->lastError());
        return false;
    }
    if (!machine->loadCharacterRom(charRom)) {
        reportError("Cannot load Apple //e character ROM at " + charRom + ": " + machine->lastError());
        return false;
    }
    if (pal && (!machine->attachMockingboard(2) || !machine->attachRgbCard(7)
                || !machine->setRgbMonitor(true))) {
        reportError("Cannot configure the DIX PAL expansion cards: " + machine->lastError());
        return false;
    }
    if (optionValue("pom2_mouse") == "enabled") {
        const auto model = std::filesystem::exists(firmware / "mouse_341-0269.bin")
            ? pom2::MouseModel::AppleMouse : pom2::MouseModel::AppleMouseAppleWin;
        if (!machine->attachMouse(4, model, firmware.string())) {
            reportError("AppleMouse needs mouse_341-0270-c.bin in the pom2 firmware folder: " + machine->lastError());
            return false;
        }
    }
    machine->setAudioSampleRate(kSampleRate);
    if (game) {
        state.index = 0;
        std::error_code pathError;
        if (requestedDisk && requestedDisk->index < state.images.size()
                && std::filesystem::equivalent(state.images[requestedDisk->index].path,
                                                requestedDisk->path, pathError))
            state.index = requestedDisk->index;
        if (!mountDisk(*machine, state, state.images[*state.index])) return false;
        if (state.mounted->smartPort) {
            // Let the //e ROM initialize the machine and enter the SmartPort
            // boot ROM through its slot scan, as the DIX master expects.
            machine->coldBoot();
        } else if (!machine->bootDisk()) {
            reportError("Cannot boot disk " + std::string(game->path) + ": " + machine->lastError());
            return false;
        }
    } else {
        machine->coldBoot();
    }
    machineTiming = pal ? POM2_TIMING_PAL : POM2_TIMING_NTSC;
    stateCapacity = 1024u * 1024u;
    for (const auto& image : state.images) {
        std::error_code error;
        const auto size = std::filesystem::file_size(image.path, error);
        if (!error && size <= (16u << 20))
            stateCapacity = std::max(stateCapacity, static_cast<std::size_t>(size) + (1u << 20));
    }
    disks = std::move(state);
    core = std::move(machine);
    readOptions();
    retro_keyboard_callback keyboard{keyboardEvent};
    environment(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);
    environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, inputDescriptors);
    return true;
}

constexpr std::uint8_t kRetroStateMagic[] = {'P', '2', 'L', 'R', 1};
constexpr std::size_t kRetroStateHeader = sizeof(kRetroStateMagic) + 4 * 6 + 8 * 3 + 3 + 2;
constexpr unsigned kNoDisk = UINT32_MAX;

std::uint64_t pathIdentity(const std::filesystem::path& path, std::uint64_t hash = 0xcbf29ce484222325ull)
{
    const auto utf8 = path.u8string();
    return pom2::hashMediaBytes(reinterpret_cast<const std::uint8_t*>(utf8.c_str()),
                                utf8.size() + 1, hash);
}

std::uint64_t playlistIdentity()
{
    auto hash = pathIdentity(disks.content);
    for (const auto& image : disks.images) hash = pathIdentity(image.path, hash);
    return hash;
}

bool saveRetroState(void* data, std::size_t size)
{
    if (!core || !data || size < stateCapacity || !core->saveState(stateScratch) ||
        stateScratch.size() > stateCapacity - kRetroStateHeader) return false;
    using namespace pom2::byteio;
    std::vector<std::uint8_t> header(std::begin(kRetroStateMagic), std::end(kRetroStateMagic));
    putU32(header, static_cast<std::uint32_t>(stateScratch.size()));
    putU64(header, playlistIdentity());
    putU64(header, disks.mounted && disks.mounted->writable ? pathIdentity(disks.mounted->path) : 0);
    putU32(header, disks.index.value_or(kNoDisk));
    putU32(header, static_cast<std::uint32_t>(cycleOvershoot));
    putU32(header, machineAudioRemainder);
    putU32(header, frontendAudioRemainder);
    putU32(header, disks.framesSinceFlush);
    putU8(header, (disks.ejected ? 1 : 0) | (disks.mounted ? 2 : 0) |
                  (disks.mounted && disks.mounted->writable ? 4 : 0) |
                  (openApple ? 8 : 0) | (solidApple ? 16 : 0) |
                  (startHeld ? 32 : 0) | (selectHeld ? 64 : 0) | (driveLedOn ? 128 : 0));
    putU8(header, padKeyboard.column);
    putU8(header, padKeyboard.row | (padKeyboard.open ? 16 : 0) | (padKeyboard.shifted ? 32 : 0));
    putU16(header, padKeyboard.held);
    const auto headerHash = pom2::hashMediaBytes(header.data(), header.size());
    putU64(header, pom2::hashMediaBytes(stateScratch.data(), stateScratch.size(), headerHash));
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::memcpy(bytes, header.data(), header.size());
    std::memcpy(bytes + header.size(), stateScratch.data(), stateScratch.size());
    const auto used = header.size() + stateScratch.size();
    std::memset(bytes + used, 0, stateCapacity - used);
    return true;
}

bool restoreRetroState(const void* data, std::size_t size)
{
    if (!core || !data || size < kRetroStateHeader ||
        std::memcmp(data, kRetroStateMagic, sizeof(kRetroStateMagic))) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    pom2::byteio::Reader reader(bytes, size);
    reader.pos = sizeof(kRetroStateMagic);
    const auto length = reader.u32();
    if (reader.u64() != playlistIdentity()) return false;
    const auto backingIdentity = reader.u64();
    const auto index = reader.u32(), overshoot = reader.u32();
    const auto machineRemainder = reader.u32(), frontendRemainder = reader.u32();
    const auto framesSinceFlush = reader.u32();
    const auto flags = reader.u8(), column = reader.u8(), rowFlags = reader.u8();
    const auto held = reader.u16();
    const auto headerHash = pom2::hashMediaBytes(bytes, reader.pos);
    const auto checksum = reader.u64();
    const bool ejected = (flags & 1) != 0;
    const bool mounted = (flags & 2) != 0;
    const bool writable = (flags & 4) != 0;
    if (stateCapacity < reader.pos || length > stateCapacity - reader.pos || !reader.has(length) ||
        checksum != pom2::hashMediaBytes(bytes + reader.pos, length, headerHash) ||
        (index != kNoDisk && index >= disks.images.size()) || (mounted && index == kNoDisk) ||
        (mounted && ejected) || (writable && !mounted) || bool(backingIdentity) != writable ||
        overshoot > 16 || machineRemainder >= static_cast<unsigned>(machineTiming.cpuClockHz) ||
        frontendRemainder >= static_cast<unsigned>(machineTiming.cpuClockHz) ||
        column >= 12 || (rowFlags & 15) >= 4 || (rowFlags & 192)) return false;

    std::optional<MountedDisk> restoredDisk;
    std::string backing;
    if (mounted) {
        const auto& image = disks.images[index];
        std::filesystem::path path = image.path;
        std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
        if (writable) {
            const auto storage = pom2::libretro::openDiskStorage(disks.saves, disks.content, image.path,
                pom2::libretro::DiskStorageAccess::ExistingOnly);
            if (!storage.privateCopy || pathIdentity(storage.path) != backingIdentity) return false;
            path = storage.path;
            lease = disks.mounted && disks.mounted->path == path && disks.mounted->lease
                ? disks.mounted->lease : pom2::libretro::lockDiskStorage(path);
            if (!lease) return false;
            backing = path.string();
        }
        restoredDisk = MountedDisk{std::move(path), image.smartPort(), writable, std::move(lease)};
    }
    const bool smartPort = restoredDisk && restoredDisk->smartPort;
    // A state rolls back its own face. Writes to an outgoing face are
    // independent and would otherwise vanish when the controller is restored.
    const auto preserveOutgoingFace = [&] {
        if (!disks.mounted || !disks.mounted->writable ||
            (restoredDisk && restoredDisk->path == disks.mounted->path && restoredDisk->writable))
            return true;
        return keepDiskEffects() && flushMountedDisk(*core, disks);
    };
    if (!core->loadState(bytes + reader.pos, length, smartPort ? std::string_view{} : backing,
                         smartPort ? backing : std::string_view{}, preserveOutgoingFace)) return false;

    disks.index = index == kNoDisk ? std::nullopt : std::optional<unsigned>(index);
    disks.ejected = ejected;
    disks.mounted = std::move(restoredDisk);
    disks.framesSinceFlush = framesSinceFlush;
    disks.flushWarning = false;
    cycleOvershoot = static_cast<int>(overshoot);
    machineAudioRemainder = machineRemainder;
    frontendAudioRemainder = frontendRemainder;
    openApple = (flags & 8) != 0;
    solidApple = (flags & 16) != 0;
    startHeld = (flags & 32) != 0;
    selectHeld = (flags & 64) != 0;
    padKeyboard = {held, column, static_cast<std::uint8_t>(rowFlags & 15),
                   (rowFlags & 16) != 0, (rowFlags & 32) != 0};
    showDiskRead((flags & 128) != 0);
    return true;
}

} // namespace

void retro_set_environment(retro_environment_t callback)
{
    environment = callback;
    if (environment) {
        bool noContent = true;
        environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noContent);
        environment(RETRO_ENVIRONMENT_SET_VARIABLES, options);
    }
}

void retro_set_video_refresh(retro_video_refresh_t callback) { video = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audioSample = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audioBatch = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { pollInput = callback; }
void retro_set_input_state(retro_input_state_t callback) { inputState = callback; }

void retro_init()
{
    supportsPadMask = environment && environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
    if (environment)
        environment(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &driveLed);
    retro_log_callback callback{};
    if (environment && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &callback))
        logMessage = callback.log;
    if (environment) registerDiskControl();
    joypadEnabled = true;
}

void retro_deinit()
{
    retro_unload_game();
    driveLed = {};
    logMessage = nullptr;
}

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_get_system_info(retro_system_info* info)
{
    static const std::string version = pom2::Core::sdkInfo().version;
    *info = {"POM2", version.c_str(), "dsk|do|po|nib|woz|2mg|2img|m3u", true, true};
}

void retro_get_system_av_info(retro_system_av_info* info)
{
    *info = {{kWidth, kHeight, kMaxWidth, kHeight, 4.0f / 3.0f},
             {double(machineTiming.cpuClockHz) / machineTiming.cyclesPerFrame, kSampleRate}};
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port != 0) return;
    const auto type = device & RETRO_DEVICE_MASK;
    joypadEnabled = type == RETRO_DEVICE_JOYPAD || type == RETRO_DEVICE_ANALOG;
}

void retro_reset()
{
    if (core) {
        clearInput();
        core->softReset();
    }
}

void retro_run()
{
    if (pollInput) pollInput();
    if (!core) return;
    bool optionsUpdated = false;
    if (environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &optionsUpdated) && optionsUpdated)
        readOptions();
    applyInput();
    auto frames = runFrame();
    const bool readDuringFrame = core->diskReadRecently(machineTiming.cyclesPerFrame);
    showDiskRead(readDuringFrame);
    // Keep advancing the audio mixer for the frames skipped by the frontend.
    // Present the final frame's sound and picture at the normal output rate.
    bool keepLoading = fastDiskLoading && readDuringFrame;
    for (unsigned i = 1; keepLoading && i < kMaxDiskLoadFrames; ++i) {
        frames = runFrame();
        keepLoading = core->diskReadRecently(machineTiming.cyclesPerFrame);
    }
    renderVideo();
    outputAudio(frames);
    autosaveMountedDisk();
}

bool retro_load_game(const retro_game_info* game)
{
    auto requestedDisk = std::exchange(initialDisk, std::nullopt);
    retro_unload_game();
    try {
        return loadGame(game, requestedDisk);
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

void retro_unload_game()
{
    showDiskRead(false);
    try {
        if (core && !flushMountedDisk(*core, disks)) reportError(core->lastError());
    } catch (const std::exception& error) {
        reportError(error.what());
    }
    core.reset();
    machineTiming = POM2_TIMING_NTSC;
    disks = {};
    initialDisk.reset();
    clearInput();
    cycleOvershoot = 0;
    machineAudioRemainder = 0;
    frontendAudioRemainder = 0;
    stateCapacity = 0;
}

unsigned retro_get_region() { return machineTiming.standard == VideoStandard::PAL ? RETRO_REGION_PAL : RETRO_REGION_NTSC; }
bool retro_load_game_special(unsigned, const retro_game_info*, std::size_t) { return false; }
std::size_t retro_serialize_size() { return core && core->supportsState() ? stateCapacity : 0; }
bool retro_serialize(void* data, std::size_t size)
{
    try { return saveRetroState(data, size); }
    catch (const std::exception&) { return false; }
}
bool retro_unserialize(const void* data, std::size_t size)
{
    try { return restoreRetroState(data, size); }
    catch (const std::exception&) { return false; }
}
void retro_cheat_reset() {}
void retro_cheat_set(unsigned, bool, const char*) {}
void* retro_get_memory_data(unsigned) { return nullptr; }
std::size_t retro_get_memory_size(unsigned) { return 0; }
