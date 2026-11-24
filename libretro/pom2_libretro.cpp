// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
// SPDX-License-Identifier: GPL-3.0-or-later

#include <libretro.h>

#include "ByteIO.h"
#include "CpuClock.h"
#include "Logger.h"
#include "MediaAutosave.h"
#include "archive_content.hpp"
#include "cheats.hpp"
#include "core_options.hpp"
#include "disk_playlist.hpp"
#include "disk_storage.hpp"
#include "machine_profile.hpp"
#include "pad_keyboard.hpp"
#include "program_file.hpp"
#include "tape_content.hpp"
#include "pom2/core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using pom2::libretro::MediaKind;
using pom2::libretro::coreOption;

VideoTiming machineTiming = POM2_TIMING_NTSC;
constexpr unsigned kWidth = 280;
constexpr unsigned kMaxWidth = 560;
constexpr unsigned kHeight = POM2_TIMING_NTSC.visibleScanlines;
constexpr unsigned kSampleRate = 44100;
constexpr unsigned kMaxAudioFrames =
    (kSampleRate * POM2_TIMING_PAL.cyclesPerFrame + POM2_TIMING_PAL.cpuClockHz - 1) / POM2_TIMING_PAL.cpuClockHz;
constexpr unsigned kMaxDiskLoadFrames = 12;

// Controller types. Port 1 is the Apple II joystick, or the RetroPad typing
// the keys most keyboard games use; port 2 is a second joystick on paddles 2-3.
constexpr unsigned kPadArrows = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
constexpr unsigned kPadIjkm = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1);
constexpr unsigned kPadWasd = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2);

retro_environment_t environment = nullptr;
retro_video_refresh_t video = nullptr;
retro_audio_sample_t audioSample = nullptr;
retro_audio_sample_batch_t audioBatch = nullptr;
retro_input_poll_t pollInput = nullptr;
retro_input_state_t inputState = nullptr;
retro_log_printf_t logMessage = nullptr;
retro_led_interface driveLed{};
bool driveLedOn = false;
bool supportsExecutionPhase = false;

void showDiskRead(bool active)
{
    if (driveLed.set_led_state && active != driveLedOn) {
        driveLed.set_led_state(0, active ? 1 : 0);
        driveLedOn = active;
    }
}

std::unique_ptr<pom2::Core> core;
pom2::libretro::MachineChoice machine;

struct MountedDisk {
    std::filesystem::path path;
    MediaKind kind;
    bool writable;
    std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
};

// One drive as the frontend's Disc Control sees it: which playlist entry it
// holds, whether its door is open, and the medium mounted behind it. Drive 0
// takes any image; drive 1 is the Disk II's second 5.25-inch drive.
struct DriveState {
    std::optional<unsigned> index;
    bool ejected = false;
    std::optional<MountedDisk> mounted;
    unsigned framesSinceFlush = 0;
    bool flushWarning = false;
};

struct DiskState {
    std::vector<pom2::libretro::DiskEntry> images;
    std::array<DriveState, 2> drives;
    bool controllerAttached = false;
    bool smartPortAttached = false;
    bool hardDiskAttached = false;
    int smartPortSlot = 0;
    int hardDiskSlot = 0;
    std::array<bool, 8> slotUsed{};
    bool appleDiskFirmware = true;
    std::filesystem::path firmware;
    std::filesystem::path saves;
    std::filesystem::path content;
};

struct InitialDisk {
    unsigned index;
    std::filesystem::path path;
};

DiskState disks;
std::optional<InitialDisk> initialDisk;
unsigned controlDrive = 0;   // the drive Disc Control acts on (pom2_disk_control_drive)

DriveState& controlled() { return disks.drives[controlDrive]; }

std::array<std::uint32_t, kMaxWidth * kHeight> videoPixels;
std::array<float, 2 * kMaxAudioFrames> audioFloat;
std::array<std::int16_t, 2 * kMaxAudioFrames> audioPcm;
int cycleOvershoot = 0;
unsigned machineAudioRemainder = 0;
unsigned frontendAudioRemainder = 0;
std::size_t stateCapacity = 0;
bool stateCapacityWarning = false;
std::vector<std::uint8_t> stateScratch;

// Runtime options (core_options.cpp lists them).
struct RuntimeOptions {
    bool fastDiskLoading = true;
    int paddleDeadZone = 4096;
    float paddleSensitivity = 1.0f;
    bool invertX = false;
    bool invertY = false;
    bool swapAxes = false;
    bool swapButtons = false;
    bool altAppleKeys = true;
    bool uppercase = false;
    float mouseSpeed = 1.0f;
    bool writeProtectAll = false;
    unsigned autosaveFrames = 300;
    std::string_view display = "auto";
};
RuntimeOptions settings;
bool rgbWarning = false;

std::array<unsigned, 2> portDevice{RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
bool openApple = false;
bool solidApple = false;
bool startHeld = false;
bool selectHeld = false;
pom2::libretro::PadKeyboard padKeyboard;
unsigned padButtons = 0;
bool supportsPadMask = false;
// A RetroPad typing keys repeats a held direction the way the //e keyboard
// does: after about half a second, then about fifteen times a second.
std::uint8_t repeatKey = 0;
std::uint8_t repeatFrames = 0;
std::uint16_t keyPadHeld = 0;
float mouseRemainderX = 0, mouseRemainderY = 0;

std::vector<std::optional<std::vector<pom2::libretro::CheatWrite>>> cheats;

retro_input_descriptor inputDescriptors[] = {
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Joystick X"},
    {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Joystick Y"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Joystick Left"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Joystick Up"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Joystick Down"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 0 / Open Apple / Space"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 1 / Solid Apple / Return"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Return"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Escape"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Show / hide Apple keyboard"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Next disk"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Keyboard shift"},
    {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Keyboard delete"},
    {1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Paddle 2"},
    {1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Paddle 3"},
    {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 2"},
    {}
};

const retro_controller_description port1Types[] = {
    {"Apple II joystick", RETRO_DEVICE_JOYPAD},
    {"Keyboard: arrows, Space, Return", kPadArrows},
    {"Keyboard: I J K M, Space, Return", kPadIjkm},
    {"Keyboard: W A S D, Space, Return", kPadWasd},
    {"None", RETRO_DEVICE_NONE},
};
const retro_controller_description port2Types[] = {
    {"Second joystick (paddles 2 and 3)", RETRO_DEVICE_JOYPAD},
    {"None", RETRO_DEVICE_NONE},
};
const retro_controller_info controllerInfo[] = {
    {port1Types, 5},
    {port2Types, 2},
    {nullptr, 0},
};

std::string_view option(const char* key)
{
    return coreOption(environment, key);
}

float percentOption(const char* key)
{
    const auto text = option(key);
    try {
        return std::stof(std::string(text)) / 100.0f;
    } catch (const std::exception&) {
        return 1.0f;
    }
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

void notify(const std::string& text)
{
    if (logMessage) logMessage(RETRO_LOG_INFO, "POM2: %s\n", text.c_str());
    if (!environment) return;
    retro_message_ext message{text.c_str(), 3000, 1, RETRO_LOG_INFO, RETRO_MESSAGE_TARGET_OSD,
                              RETRO_MESSAGE_TYPE_NOTIFICATION_ALT, -1};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &message);
}

pom2::DisplayMode displayModeFor(std::string_view value)
{
    using pom2::DisplayMode;
    if (value == "composite-medium") return DisplayMode::CompositeMedium;
    if (value == "composite-sharp") return DisplayMode::CompositeSharp;
    if (value == "composite-oe") return DisplayMode::CompositeOpenEmulator;
    if (value == "applewin-monitor") return DisplayMode::AppleWinMonitor;
    if (value == "applewin-tv") return DisplayMode::AppleWinTv;
    if (value == "applewin-idealized") return DisplayMode::AppleWinIdealized;
    if (value == "mono-white") return DisplayMode::MonoWhite;
    if (value == "mono-green") return DisplayMode::MonoGreen;
    if (value == "mono-amber") return DisplayMode::MonoAmber;
    if (value == "rgb") return DisplayMode::Rgb;
    if (value == "auto" && core && core->rgbCardAttached()) return DisplayMode::Rgb;
    return DisplayMode::Composite;
}

void applyRuntimeOptions()
{
    settings.paddleDeadZone = static_cast<int>(32768.0f * percentOption("pom2_paddle_deadzone"));
    settings.paddleSensitivity = percentOption("pom2_paddle_sensitivity");
    const auto axes = option("pom2_joystick_axes");
    settings.invertX = axes == "invert-x" || axes == "invert-both";
    settings.invertY = axes == "invert-y" || axes == "invert-both";
    settings.swapAxes = axes == "swap";
    settings.swapButtons = option("pom2_swap_buttons") == "enabled";
    settings.altAppleKeys = option("pom2_alt_apple_keys") != "disabled";
    const auto upper = option("pom2_uppercase");
    settings.uppercase = upper == "enabled"
        || (upper == "auto" && (machine.kind == pom2::libretro::MachineKind::IIPlus
                                || machine.kind == pom2::libretro::MachineKind::II));
    settings.mouseSpeed = percentOption("pom2_mouse_speed");
    settings.fastDiskLoading = option("pom2_fast_disk_loading") != "disabled";
    settings.writeProtectAll = option("pom2_write_protect") == "enabled";
    controlDrive = option("pom2_disk_control_drive") == "2" ? 1 : 0;
    const double fps = double(machineTiming.cpuClockHz) / machineTiming.cyclesPerFrame;
    unsigned seconds = 5;
    try { seconds = static_cast<unsigned>(std::stoul(std::string(option("pom2_autosave_interval")))); }
    catch (const std::exception&) {}
    settings.autosaveFrames = std::max(1u, static_cast<unsigned>(std::lround(seconds * fps)));
    if (!core) return;
    core->setSpeakerVolume(percentOption("pom2_speaker_volume"));
    core->setMockingboardVolume(percentOption("pom2_mockingboard_volume"));
    core->setDriveSoundVolume(percentOption("pom2_drive_sound_volume"));
    settings.display = option("pom2_display");
    if (!core->setDisplayMode(displayModeFor(settings.display))) {
        core->setDisplayMode(pom2::DisplayMode::Composite);
        if (!rgbWarning) reportError("The RGB display needs the Le Chat Mauve RGB card (Core Options, restart).");
        rgbWarning = true;
    }
    pom2::libretro::updateOptionVisibility(environment);
}

std::uint8_t typed(std::uint8_t ascii)
{
    if (settings.uppercase && ascii >= 'a' && ascii <= 'z') return static_cast<std::uint8_t>(ascii - 32);
    return ascii;
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
    if (settings.altAppleKeys) {
        if (keycode == RETROK_LALT) openApple = down;
        if (keycode == RETROK_RALT) solidApple = down;
    }

    const unsigned ascii = appleKey(keycode, character, modifiers);
    // The libretro keyboard keycodes fit in the SDK's 512 physical key tokens.
    if (keycode != RETROK_UNKNOWN && keycode < 512 && (ascii != 0 || !down))
        core->setKeyHeld(static_cast<std::uint16_t>(keycode), down);
    if (down && ascii != 0)
        core->queueKey(typed(static_cast<std::uint8_t>(ascii)));
}

bool padPressed(unsigned id)
{
    return (padButtons & (1u << id)) != 0;
}

float stickAxis(unsigned port, unsigned id)
{
    if (!inputState) return 0.0f;
    const auto value = inputState(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, id);
    if (std::abs(int(value)) < settings.paddleDeadZone) return 0.0f;
    return std::clamp(settings.paddleSensitivity * value / (value < 0 ? 32768.0f : 32767.0f), -1.0f, 1.0f);
}

float padAxis(unsigned id, unsigned negative, unsigned positive)
{
    const int dpad = int(padPressed(positive)) - int(padPressed(negative));
    if (dpad != 0) return static_cast<float>(dpad);
    return stickAxis(0, id);
}

void nextDisk();

unsigned readPad(unsigned port)
{
    unsigned buttons = 0;
    if (!inputState) return 0;
    if (supportsPadMask)
        return static_cast<std::uint16_t>(inputState(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
    for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; ++id)
        if (inputState(port, RETRO_DEVICE_JOYPAD, 0, id)) buttons |= 1u << id;
    return buttons;
}

// The RetroPad as a keyboard: the directions and two buttons type keys, and
// hold them for the //e's any-key-down flag.
void applyKeyPad(unsigned device)
{
    struct Binding { unsigned button; std::uint8_t ascii; std::uint16_t token; };
    const Binding arrows[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, 0x0b, RETROK_UP}, {RETRO_DEVICE_ID_JOYPAD_DOWN, 0x0a, RETROK_DOWN},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, 0x08, RETROK_LEFT}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, 0x15, RETROK_RIGHT}};
    const Binding ijkm[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, 'I', RETROK_i}, {RETRO_DEVICE_ID_JOYPAD_DOWN, 'M', RETROK_m},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, 'J', RETROK_j}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, 'K', RETROK_k}};
    const Binding wasd[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, 'W', RETROK_w}, {RETRO_DEVICE_ID_JOYPAD_DOWN, 'S', RETROK_s},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, 'A', RETROK_a}, {RETRO_DEVICE_ID_JOYPAD_RIGHT, 'D', RETROK_d}};
    const Binding* directions = device == kPadIjkm ? ijkm : device == kPadWasd ? wasd : arrows;
    std::array<Binding, 6> bindings{directions[0], directions[1], directions[2], directions[3],
        Binding{RETRO_DEVICE_ID_JOYPAD_B, ' ', RETROK_SPACE},
        Binding{RETRO_DEVICE_ID_JOYPAD_A, 0x0d, RETROK_RETURN}};
    std::uint8_t pressedKey = 0;
    for (unsigned i = 0; i < bindings.size(); ++i) {
        const bool down = padPressed(bindings[i].button);
        const bool was = (keyPadHeld & (1u << i)) != 0;
        if (down != was) core->setKeyHeld(bindings[i].token, down);
        if (down && !was) {
            core->queueKey(typed(bindings[i].ascii));
            pressedKey = bindings[i].ascii;
            repeatFrames = 0;
        }
        keyPadHeld = static_cast<std::uint16_t>(down ? keyPadHeld | (1u << i) : keyPadHeld & ~(1u << i));
    }
    if (pressedKey) {
        repeatKey = pressedKey;
        return;
    }
    // Repeat only the most recently pressed direction, while it is held.
    bool held = false;
    for (unsigned i = 0; i < 4; ++i)
        held |= (keyPadHeld & (1u << i)) && bindings[i].ascii == repeatKey;
    if (!held) {
        repeatKey = 0;
        repeatFrames = 0;
        return;
    }
    if (++repeatFrames >= 30 && (repeatFrames - 30) % 4 == 0) core->queueKey(typed(repeatKey));
    if (repeatFrames > 60) repeatFrames = 34;
}

void releaseKeyPad()
{
    const std::uint16_t tokens[] = {RETROK_UP, RETROK_DOWN, RETROK_LEFT, RETROK_RIGHT,
                                    RETROK_i, RETROK_m, RETROK_j, RETROK_k,
                                    RETROK_w, RETROK_s, RETROK_a, RETROK_d, RETROK_SPACE, RETROK_RETURN};
    if (core && keyPadHeld)
        for (auto token : tokens) core->setKeyHeld(token, false);
    keyPadHeld = 0;
    repeatKey = 0;
    repeatFrames = 0;
}

void applyInput()
{
    const unsigned device = portDevice[0];
    const bool padEnabled = (device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
    padButtons = padEnabled ? readPad(0) : 0;
    if (core->mouseAttached() && inputState) {
        const auto mouse = [](unsigned id) { return inputState(0, RETRO_DEVICE_MOUSE, 0, id); };
        // Keep the fraction a slow speed would drop, so no motion is lost.
        mouseRemainderX += settings.mouseSpeed * mouse(RETRO_DEVICE_ID_MOUSE_X);
        mouseRemainderY += settings.mouseSpeed * mouse(RETRO_DEVICE_ID_MOUSE_Y);
        const int dx = static_cast<int>(mouseRemainderX), dy = static_cast<int>(mouseRemainderY);
        mouseRemainderX -= dx;
        mouseRemainderY -= dy;
        core->moveMouse(dx, dy, mouse(RETRO_DEVICE_ID_MOUSE_LEFT) != 0);
    }
    // The second joystick: paddles 2 and 3 and push button 2.
    if ((portDevice[1] & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD && inputState) {
        const auto paddle = [](float axis) {
            return static_cast<std::uint8_t>(std::lround((std::clamp(axis, -1.0f, 1.0f) + 1.0f) * 127.5f));
        };
        const unsigned second = readPad(1);
        const auto dpad = [second](unsigned negative, unsigned positive) {
            return float(int((second >> positive) & 1) - int((second >> negative) & 1));
        };
        const float x = dpad(RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        const float y = dpad(RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN);
        core->setPaddle(2, paddle(x != 0 ? x : stickAxis(1, RETRO_DEVICE_ID_ANALOG_X)));
        core->setPaddle(3, paddle(y != 0 ? y : stickAxis(1, RETRO_DEVICE_ID_ANALOG_Y)));
        core->setPaddleButton(2, ((second >> RETRO_DEVICE_ID_JOYPAD_B) & 1) != 0);
    }

    const bool wasOpen = padKeyboard.open;
    const bool swap = padPressed(RETRO_DEVICE_ID_JOYPAD_R3) && !(padKeyboard.held & (1u << RETRO_DEVICE_ID_JOYPAD_R3));
    const auto key = padKeyboard.update(static_cast<std::uint16_t>(padButtons));
    if (key) core->queueKey(typed(key));
    if (wasOpen || padKeyboard.open) {
        releaseKeyPad();
        core->setJoystickAxes(0, 0);
        core->setPaddleButton(0, openApple);
        core->setPaddleButton(1, solidApple);
        startHeld = padPressed(RETRO_DEVICE_ID_JOYPAD_START);
        selectHeld = padPressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
        return;
    }
    if (swap) nextDisk();
    const bool keyPad = device == kPadArrows || device == kPadIjkm || device == kPadWasd;
    if (keyPad) {
        applyKeyPad(device);
        core->setJoystickAxes(0, 0);
        core->setPaddleButton(0, openApple);
        core->setPaddleButton(1, solidApple);
    } else {
        releaseKeyPad();
        float x = padAxis(RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
        float y = padAxis(RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN);
        if (settings.swapAxes) std::swap(x, y);
        if (settings.invertX) x = -x;
        if (settings.invertY) y = -y;
        core->setJoystickAxes(x, y);
        const bool b = padPressed(RETRO_DEVICE_ID_JOYPAD_B), a = padPressed(RETRO_DEVICE_ID_JOYPAD_A);
        core->setPaddleButton(0, openApple || (settings.swapButtons ? a : b));
        core->setPaddleButton(1, solidApple || (settings.swapButtons ? b : a));
    }

    const bool start = padPressed(RETRO_DEVICE_ID_JOYPAD_START);
    const bool select = padPressed(RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (start && !startHeld) core->queueKey(0x0d);
    if (select && !selectHeld) core->queueKey(0x1b);
    startHeld = start;
    selectHeld = select;
}

void clearInput()
{
    releaseKeyPad();
    padKeyboard = {};
    padButtons = 0;
    openApple = false;
    solidApple = false;
    startHeld = false;
    selectHeld = false;
    mouseRemainderX = mouseRemainderY = 0;
    if (core) core->releaseHeldKeys();
}

// ---- Memory map: the layout RetroAchievements gives the Apple II -----------

struct MapPiece {
    pom2::RamBank bank;
    std::uint32_t start;
    const char* space;
};

constexpr MapPiece kMapPieces[] = {
    {pom2::RamBank::Main, 0x00000, "Main"},
    {pom2::RamBank::LanguageCard1, 0x0C000, "Main language card bank 1"},
    {pom2::RamBank::LanguageCard2, 0x0D000, "Main language card bank 2"},
    {pom2::RamBank::LanguageCardHigh, 0x0E000, "Main language card"},
    {pom2::RamBank::Aux, 0x10000, "Aux"},
    {pom2::RamBank::AuxLanguageCard1, 0x1C000, "Aux language card bank 1"},
    {pom2::RamBank::AuxLanguageCard2, 0x1D000, "Aux language card bank 2"},
    {pom2::RamBank::AuxLanguageCardHigh, 0x1E000, "Aux language card"},
};

std::uint8_t* memoryCell(std::uint32_t address)
{
    if (!core) return nullptr;
    for (const auto& piece : kMapPieces) {
        const auto region = core->ramRegion(piece.bank);
        if (region.data && address >= piece.start && address < piece.start + region.size)
            return region.data + (address - piece.start);
    }
    return nullptr;
}

void registerMemoryMap()
{
    static std::array<retro_memory_descriptor, std::size(kMapPieces)> descriptors;
    unsigned count = 0;
    for (const auto& piece : kMapPieces) {
        const auto region = core->ramRegion(piece.bank);
        if (!region.data) continue;
        retro_memory_descriptor descriptor{};
        descriptor.flags = RETRO_MEMDESC_SYSTEM_RAM;
        descriptor.ptr = region.data;
        descriptor.start = piece.start;
        descriptor.len = region.size;
        descriptor.addrspace = piece.space;
        descriptors[count++] = descriptor;
    }
    retro_memory_map map{descriptors.data(), count};
    environment(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
}

void applyCheats()
{
    for (const auto& cheat : cheats) {
        if (!cheat) continue;
        for (const auto& write : *cheat) {
            auto* cell = memoryCell(write.address);
            if (cell && (!write.compare || *cell == *write.compare)) *cell = write.value;
        }
    }
}

// ---- Video and audio ------------------------------------------------------

void renderVideo()
{
    if (!video) return;
    const auto frame = core->renderFrame();
    const auto width = static_cast<unsigned>(std::min(frame.width, int(kMaxWidth)));
    const auto height = static_cast<unsigned>(std::min(frame.height, int(kHeight)));
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = frame.pixels + std::size_t(y) * frame.width;
        auto* out = videoPixels.data() + std::size_t(y) * width;
        for (unsigned x = 0; x < width; ++x) {
            const auto rgba = row[x];
            // The SDK packs bytes as 0xAABBGGRR and libretro expects 0x00RRGGBB.
            out[x] = ((rgba & 0xff) << 16) | (rgba & 0xff00) | ((rgba >> 16) & 0xff);
        }
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
    cycleOvershoot = core->runVideo(budget) - budget;
    const auto frames = audioFramesForTick(machineAudioRemainder);
    core->pullAudio(audioFloat.data(), frames, kSampleRate);
    return frames;
}

// ---- Disks ------------------------------------------------------------------

std::string optionalRom(const DiskState& state, const char* name)
{
    if (!state.appleDiskFirmware) return {};
    const auto path = state.firmware / name;
    return std::filesystem::exists(path) ? path.string() : std::string{};
}

int freeSlot(const DiskState& state, std::initializer_list<int> preferred)
{
    for (int slot : preferred)
        if (!state.slotUsed[slot]) return slot;
    return 0;
}

bool attachController(pom2::Core& target, DiskState& state, MediaKind kind)
{
    const bool slotted = pom2::libretro::hasSlots(machine.kind);
    switch (kind) {
    case MediaKind::DiskII:
        if (state.controllerAttached) return true;
        // A //c's $C600 is its own ROM: only the sequencer is Apple's to give.
        if (!target.attachDiskII(slotted ? optionalRom(state, "disk2.rom") : std::string{},
                                 optionalRom(state, "diskii_p6.rom"))) break;
        state.controllerAttached = true;
        state.slotUsed[6] = true;
        return true;
    case MediaKind::SmartPort:
        if (state.smartPortAttached) return true;
        if (machine.kind != pom2::libretro::MachineKind::IIcPlus) {
            const int slot = slotted ? freeSlot(state, {5, 7, 4, 2, 1}) : 5;
            if (!slot) {
                reportError("No free slot for the 3.5\" SmartPort controller.");
                return false;
            }
            if (!target.attachSmartPort(slot)) break;
            state.smartPortSlot = slot;
            state.slotUsed[slot] = true;
        }
        state.smartPortAttached = true;
        return true;
    case MediaKind::HardDisk: {
        if (state.hardDiskAttached) return true;
        const int slot = slotted ? freeSlot(state, {7, 5, 4, 2, 1}) : 7;
        if (!slot) {
            reportError("No free slot for the hard disk controller.");
            return false;
        }
        if (!target.attachHardDisk(slot)) break;
        state.hardDiskSlot = target.hardDiskSlot();
        if (slotted) state.slotUsed[slot] = true;
        state.hardDiskAttached = true;
        return true;
    }
    }
    reportError(target.lastError());
    return false;
}

bool insertMedia(pom2::Core& target, MediaKind kind, int drive, const std::string& path)
{
    switch (kind) {
    case MediaKind::DiskII:    return target.insertDisk(drive, path);
    case MediaKind::SmartPort: return target.insertSmartPortDisk(0, path);
    case MediaKind::HardDisk:  return target.insertHardDisk(0, path);
    }
    return false;
}

bool protectMedia(pom2::Core& target, MediaKind kind, int drive)
{
    switch (kind) {
    case MediaKind::DiskII:    return target.setDiskWriteProtected(drive, true);
    case MediaKind::SmartPort: return target.setSmartPortDiskWriteProtected(0, true);
    case MediaKind::HardDisk:  return target.setHardDiskWriteProtected(0, true);
    }
    return false;
}

bool mediaProtected(pom2::Core& target, MediaKind kind, int drive)
{
    switch (kind) {
    case MediaKind::DiskII:    return target.diskWriteProtected(drive);
    case MediaKind::SmartPort: return target.smartPortDiskWriteProtected(0);
    case MediaKind::HardDisk:  return target.hardDiskWriteProtected(0);
    }
    return true;
}

bool ejectMedia(pom2::Core& target, MediaKind kind, int drive)
{
    switch (kind) {
    case MediaKind::DiskII:    return target.ejectDisk(drive);
    case MediaKind::SmartPort: return target.ejectSmartPortDisk(0);
    case MediaKind::HardDisk:  return target.ejectHardDisk(0);
    }
    return false;
}

bool mountDisk(pom2::Core& target, DiskState& state, const pom2::libretro::DiskEntry& image, int drive)
{
    const auto kind = image.kind();
    if (drive == 1 && kind != MediaKind::DiskII) {
        reportError("Drive 2 is a 5.25-inch Disk II drive: " + image.label + " goes in drive 1.");
        return false;
    }
    for (const auto& other : state.drives) {
        if (other.mounted && other.mounted->kind == kind && kind != MediaKind::DiskII) {
            reportError(image.label + ": the other drive already holds this kind of disk.");
            return false;
        }
    }
    pom2::libretro::DiskStorage storage{image.path};
    std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
    if (!settings.writeProtectAll) {
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
    }
    if (!attachController(target, state, kind)) return false;

    const auto path = storage.path.string();
    if (!insertMedia(target, kind, drive, path)) {
        reportError("Cannot insert disk " + image.path.string() + ": " + target.lastError());
        return false;
    }
    // Only a private copy with a write lease may accept guest writes.
    if (!lease && !protectMedia(target, kind, drive)) {
        const auto reason = target.lastError();
        ejectMedia(target, kind, drive);
        reportError("Cannot write-protect disk " + path + ": " + reason);
        return false;
    }
    const bool protectedDisk = mediaProtected(target, kind, drive);
    if (protectedDisk) lease.reset();
    // A hard disk's save states carry the blocks it changed from the original.
    if (kind == MediaKind::HardDisk && !target.setHardDiskStateBase(image.path.string()) && logMessage)
        logMessage(RETRO_LOG_WARN, "POM2: no save states for this hard disk: %s\n", target.lastError().c_str());
    auto& slot = state.drives[drive];
    slot.mounted = MountedDisk{storage.path, kind, !protectedDisk, std::move(lease)};
    slot.framesSinceFlush = 0;
    slot.flushWarning = false;
    return true;
}

bool flushMountedDisk(pom2::Core& target, const DiskState& state, int drive)
{
    const auto& mounted = state.drives[drive].mounted;
    if (!mounted || !mounted->writable) return true;
    switch (mounted->kind) {
    case MediaKind::DiskII:    return target.flushDisk(drive);
    case MediaKind::SmartPort: return target.flushSmartPortDisk(0);
    case MediaKind::HardDisk:  return target.flushHardDisk(0);
    }
    return true;
}

bool flushAllDisks(pom2::Core& target, const DiskState& state)
{
    bool ok = true;
    for (int drive = 0; drive < 2; ++drive) ok = flushMountedDisk(target, state, drive) && ok;
    return ok;
}

bool ejectMountedDisk(pom2::Core& target, DiskState& state, int drive)
{
    auto& mounted = state.drives[drive].mounted;
    if (!mounted) return true;
    if (!flushMountedDisk(target, state, drive)) return false;
    const bool ejected = ejectMedia(target, mounted->kind, drive);
    if (ejected) mounted.reset();
    return ejected;
}

bool keepDiskEffects()
{
    if (!supportsExecutionPhase) return true;
    auto phase = RETRO_EXECUTION_PHASE_RETAINED;
    return !environment(RETRO_ENVIRONMENT_GET_EXECUTION_PHASE, &phase)
        || phase == RETRO_EXECUTION_PHASE_RETAINED;
}

void autosaveMountedDisk()
{
    if (!keepDiskEffects()) return;
    for (int drive = 0; drive < 2; ++drive) {
        auto& slot = disks.drives[drive];
        if (!slot.mounted || !slot.mounted->writable) continue;
        if (++slot.framesSinceFlush < settings.autosaveFrames) continue;
        slot.framesSinceFlush = 0;
        try {
            if (flushMountedDisk(*core, disks, drive)) {
                slot.flushWarning = false;
                continue;
            }
            if (!slot.flushWarning) reportError(core->lastError());
        } catch (const std::exception& error) {
            if (!slot.flushWarning) reportError(error.what());
        }
        slot.flushWarning = true;
    }
}

bool RETRO_CALLCONV setDiskEjected(bool ejected)
{
    auto& slot = controlled();
    if (slot.ejected == ejected) return true;
    if (!core) return false;
    try {
        if (ejected && !ejectMountedDisk(*core, disks, int(controlDrive))) {
            reportError(core->lastError());
            return false;
        }
        if (!ejected && slot.index && !mountDisk(*core, disks, disks.images[*slot.index], int(controlDrive)))
            return false;
        slot.ejected = ejected;
        if (ejected) showDiskRead(false);
        return true;
    } catch (const std::exception& error) {
        reportError(error.what());
        return false;
    }
}

bool RETRO_CALLCONV getDiskEjected() { return controlled().ejected; }
unsigned RETRO_CALLCONV getDiskCount() { return static_cast<unsigned>(disks.images.size()); }
unsigned RETRO_CALLCONV getDiskIndex() { return controlled().index.value_or(getDiskCount()); }

bool RETRO_CALLCONV setDiskIndex(unsigned index)
{
    auto& slot = controlled();
    if (!slot.ejected) return false;
    if (index >= getDiskCount()) {
        slot.index.reset();
    } else {
        if (disks.images[index].path.empty()) return false;
        slot.index = index;
    }
    return true;
}

void nextDisk()
{
    const unsigned count = getDiskCount();
    if (count < 2 || controlled().ejected || !keepDiskEffects()) return;
    const auto previous = getDiskIndex();
    const auto& other = disks.drives[1 - controlDrive];
    const auto usable = [&](unsigned i) {
        return !disks.images[i].path.empty() && !(other.mounted && other.index == i)
            && (controlDrive == 0 || disks.images[i].kind() == MediaKind::DiskII);
    };
    unsigned next = previous < count ? (previous + 1) % count : 0;
    while (next != previous && !usable(next)) {
        next = (next + 1) % count;
        if (next == 0 && previous >= count) return;
    }
    if (next == previous || !setDiskEjected(true)) return;
    if (!setDiskIndex(next) || !setDiskEjected(false)) {
        if (setDiskIndex(previous)) setDiskEjected(false);
        return;
    }
    const std::string label = (controlDrive ? "Drive 2: " : "") + disks.images[next].label;
    retro_message message{label.c_str(), 120};
    environment(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

bool RETRO_CALLCONV replaceDisk(unsigned index, const retro_game_info* game)
{
    if (!controlled().ejected || index >= getDiskCount()) return false;
    if (!game) {
        for (auto& slot : disks.drives) {
            if (slot.index == index && slot.mounted) return false;   // in the other drive
        }
        disks.images.erase(disks.images.begin() + index);
        for (auto& slot : disks.drives) {
            if (slot.index == index) slot.index.reset();
            else if (slot.index && *slot.index > index) --*slot.index;
        }
        return true;
    }
    if (!game->path || !*game->path) return false;
    try {
        auto path = std::filesystem::u8path(game->path);
        if (pom2::libretro::isArchive(path)) {
            // An archive's first disk, from the same cache as content.
            const auto cache = (disks.saves.empty() ? disks.firmware : disks.saves / "pom2") / "archives";
            path = pom2::libretro::unpackArchive(path, cache);
            if (pom2::libretro::isProgramFile(path))
                throw std::runtime_error(path.filename().u8string() + " is a program, not a disk");
            disks.images[index] = pom2::libretro::loadDiskList(path).front();
            return true;
        }
        disks.images[index] = pom2::libretro::loadDiskImage(path);
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

// ---- Loading ----------------------------------------------------------------

// The expansion cards the options ask for. False only when the machine
// cannot be built at all; a card that does not fit is reported and skipped.
bool installCards(pom2::Core& target, DiskState& state, std::string& summary)
{
    const bool slotted = pom2::libretro::hasSlots(machine.kind);
    const auto rgb = option("pom2_rgb_card");
    if (rgb == "enabled" || (rgb == "auto" && machine.dix)) {
        const auto model = slotted ? pom2::RgbCardModel::LeChatMauveFeline
                                   : pom2::RgbCardModel::LeChatMauveIIcAdapter;
        if (target.attachRgbCard(7, model)) {
            if (slotted) state.slotUsed[7] = true;
            summary += " · Le Chat Mauve";
        } else {
            reportError("Le Chat Mauve RGB card: " + target.lastError());
        }
    }
    if (slotted && option("pom2_mouse") == "enabled") {
        const auto modelOption = option("pom2_mouse_model");
        const bool mcu = modelOption == "mcu"
            || (modelOption == "auto" && std::filesystem::exists(state.firmware / "mouse_341-0269.bin"));
        if (!target.attachMouse(4, mcu ? pom2::MouseModel::AppleMouse : pom2::MouseModel::AppleMouseAppleWin,
                                state.firmware.string())) {
            reportError("AppleMouse needs mouse_341-0270-c.bin in the pom2 firmware folder: " + target.lastError());
            return false;
        }
        state.slotUsed[4] = true;
        summary += " · AppleMouse slot 4";
    }
    const auto board = option("pom2_mockingboard");
    if (board != "disabled") {
        const auto model = board == "sound2" ? pom2::MockingboardModel::SoundII : pom2::MockingboardModel::AC;
        int slot = 4;
        const auto slotOption = option("pom2_mockingboard_slot");
        if (slotOption != "auto") slot = std::stoi(std::string(slotOption));
        else if (machine.dix || state.slotUsed[4]) slot = 2;
        if (slotted && state.slotUsed[slot]) {
            reportError("Sound card: slot " + std::to_string(slot) + " is taken; the card was left out.");
        } else if (board == "phasor") {
            if (target.attachPhasor(slot)) {
                state.slotUsed[slot] = true;
                summary += " · Phasor slot " + std::to_string(slot);
            } else {
                reportError("Phasor: " + target.lastError());
            }
        } else if (target.attachMockingboard(slot, model)) {
            if (slotted) state.slotUsed[slot] = true;
            summary += slotted ? " · Mockingboard slot " + std::to_string(slot) : " · Mockingboard 4c";
        } else {
            reportError("Mockingboard: " + target.lastError());
        }
    }
    const auto accel = option("pom2_accelerator");
    const bool plus = machine.kind == pom2::libretro::MachineKind::IIcPlus;
    const auto model = accel == "zip" || (accel == "auto" && plus) ? pom2::Accelerator::ZipChip
                     : accel == "fastchip" ? pom2::Accelerator::FastChip : pom2::Accelerator::None;
    if (model != pom2::Accelerator::None) {
        if (target.setAccelerator(model))
            summary += model == pom2::Accelerator::FastChip ? " · FASTChip" : plus ? " · 4 MHz" : " · Zip Chip";
        else
            reportError("Accelerator: " + target.lastError());
    }
    const auto clock = option("pom2_clock");
    if (clock != "disabled") {
        if (target.attachNoSlotClock(clock == "host" ? -1 : static_cast<std::int64_t>(std::time(nullptr))))
            summary += " · No-Slot Clock";
        else
            reportError("No-Slot Clock: " + target.lastError());
    }
    if (option("pom2_drive_sounds") == "enabled") {
        const auto samples = state.firmware / "floppy_samples";
        if (target.attachDriveSounds(samples.string())) summary += " · drive sounds";
        else reportError("Disk drive sounds need MAME's samples in " + samples.string());
    }
    return true;
}

bool bootMedia(pom2::Core& target, const MountedDisk& mounted)
{
    switch (mounted.kind) {
    case MediaKind::DiskII:
        // A real power-on: the ROM's reset routine, then its slot scan finds
        // the Disk II. Jumping straight to $C600 (bootDisk) skips the reset's
        // set-up, and DIGIDREAM and the fastloader WOZ stay black after it.
        // The 1977 ][ has no autostart ROM: its owner typed 6 Ctrl-P.
        if (machine.kind == pom2::libretro::MachineKind::II) return target.bootDisk();
        target.coldBoot();
        return true;
    case MediaKind::SmartPort:
        // A //e lets its ROM scan the slots and enter the SmartPort boot ROM,
        // as the DIX master expects; a //c boots its 3.5" port from $C500.
        if (!pom2::libretro::hasSlots(machine.kind) && target.bootSlot(5)) return true;
        target.coldBoot();
        return true;
    case MediaKind::HardDisk:
        // A //c's hard disk hangs off its 3.5" port, which $C500 boots.
        if (target.bootSlot(pom2::libretro::hasSlots(machine.kind) ? disks.hardDiskSlot : 5)) return true;
        target.coldBoot();
        return true;
    }
    return false;
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

    DiskState state;
    std::optional<pom2::libretro::ProgramImage> program;
    std::optional<pom2::libretro::TapePlan> tape;
    state.firmware = std::filesystem::u8path(systemDirectory) / "pom2";
    state.appleDiskFirmware = option("pom2_disk2_firmware") != "pom2";
    state.slotUsed[3] = true;   // the //e's 80 columns are internal; keep the slot clear
    const char* saveDirectory = nullptr;
    if (environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &saveDirectory) && saveDirectory && *saveDirectory)
        state.saves = std::filesystem::u8path(saveDirectory);
    if (game) {
        if (!game->path || !*game->path) {
            reportError("A filesystem path is required for a disk image or M3U playlist.");
            return false;
        }
        state.content = std::filesystem::absolute(std::filesystem::u8path(game->path));
        // An archive unpacks beside the private disk copies, so its disks
        // keep them from one session to the next.
        if (pom2::libretro::isArchive(state.content)) {
            const auto cache = (state.saves.empty() ? state.firmware : state.saves / "pom2") / "archives";
            state.content = pom2::libretro::unpackArchive(state.content, cache);
        }
        if (pom2::libretro::isProgramFile(state.content))
            program = pom2::libretro::loadProgram(state.content);
        else if (pom2::libretro::isTapeFile(state.content))
            tape = pom2::libretro::planTape(state.content);
        else
            state.images = pom2::libretro::loadDiskList(state.content);
    }

    pom2::libretro::MachineRequest request;
    request.machine = option("pom2_machine");
    request.standard = option("pom2_video_standard");
    request.cpu = option("pom2_cpu");
    request.dix = option("pom2_dix_preset");
    request.firmware = state.firmware;
    if (!state.images.empty()) {
        request.firstDisk = state.images.front().path;
        request.firstDiskIs35 = state.images.front().smartPort();
    }
    if (tape) {
        request.prefer = tape->prefer;
        request.needsCassette = true;
    }
    std::string error;
    if (!pom2::libretro::chooseMachine(request, machine, error)) {
        reportError(error);
        return false;
    }

    auto target = std::make_unique<pom2::Core>(machine.config);
    // RamWorks III: before the ROM, as the card is in the slot at power-on.
    const auto ramworks = option("pom2_ramworks");
    const int banks = ramworks == "256k" ? 4 : ramworks == "512k" ? 8 : ramworks == "1m" ? 16
                    : ramworks == "3m" ? 48 : ramworks == "8m" ? 128 : 1;
    const bool ramWorksFits = banks > 1 && pom2::libretro::hasSlots(machine.kind)
        && machine.kind != pom2::libretro::MachineKind::IIPlus && machine.kind != pom2::libretro::MachineKind::II;
    if (ramWorksFits && !target->setRamWorksBanks(banks)) reportError("RamWorks: " + target->lastError());
    if (!target->loadRom(machine.rom.string(), machine.lowerBank)) {
        reportError("Cannot load the " + machine.name + " firmware at " + machine.rom.string() + ": "
                    + target->lastError());
        return false;
    }
    if (!target->loadCharacterRom(machine.characterRom.string())) {
        reportError("Cannot load the character ROM at " + machine.characterRom.string() + ": "
                    + target->lastError());
        return false;
    }
    std::string summary = machine.name;
    if (ramWorksFits) summary += " · RamWorks " + std::string(ramworks);
    if (!installCards(*target, state, summary)) return false;
    if (machine.kind == pom2::libretro::MachineKind::IIcPlus) state.slotUsed[6] = true;
    target->setAudioSampleRate(kSampleRate);
    machineTiming = machine.config.timing == pom2::VideoTiming::PAL ? POM2_TIMING_PAL : POM2_TIMING_NTSC;
    applyRuntimeOptions();   // the write-protect setting is needed before the first mount

    state.drives[1].ejected = true;   // drive 2 starts empty
    std::string tapeNote;
    if (game && !program && !tape) {
        unsigned first = 0;
        std::error_code pathError;
        if (requestedDisk && requestedDisk->index < state.images.size()
                && std::filesystem::equivalent(state.images[requestedDisk->index].path,
                                                requestedDisk->path, pathError))
            first = requestedDisk->index;
        state.drives[0].index = first;
        disks = std::move(state);   // the controllers read the machine's choice
        if (!mountDisk(*target, disks, disks.images[first], 0)) return false;
        // Drive 2 takes the next 5.25-inch disk of the playlist, when asked.
        if (option("pom2_drive2") == "next" && disks.drives[0].mounted->kind == MediaKind::DiskII) {
            for (unsigned step = 1; step < disks.images.size(); ++step) {
                const unsigned i = (first + step) % disks.images.size();
                if (disks.images[i].kind() != MediaKind::DiskII) continue;
                disks.drives[1].index = i;
                if (!mountDisk(*target, disks, disks.images[i], 1)) return false;
                disks.drives[1].ejected = false;
                break;
            }
        }
        if (!bootMedia(*target, *disks.drives[0].mounted)) {
            reportError("Cannot boot disk " + std::string(game->path) + ": " + target->lastError());
            return false;
        }
    } else {
        disks = std::move(state);
        target->coldBoot();
        if (!pom2::libretro::hasSlots(machine.kind)) {
            // A //c's drive is built in and its ROM boots it at power-on,
            // waiting for a disk that never comes. Its owner presses
            // Ctrl-Reset after the ROM has marked the machine powered up,
            // which lands in BASIC: do the same.
            if (!attachController(*target, disks, MediaKind::DiskII)) return false;
            target->coldBoot();
            target->runVideo(static_cast<int>(machineTiming.cpuClockHz * 2));
            target->softReset();
        }
        if (program) {
            // At the BASIC prompt: the program in RAM, as LOAD or BLOAD would
            // leave it, then typed in to start it.
            target->runVideo(static_cast<int>(machineTiming.cpuClockHz * 2));
            if (!target->writeRam(pom2::RamBank::Main, program->address, program->bytes.data(),
                                  program->bytes.size())) {
                reportError("Cannot load the program: " + target->lastError());
                return false;
            }
            if (program->applesoft) {
                // TXTTAB, then VARTAB, ARYTAB, STREND and PRGEND at the end.
                const auto end = static_cast<std::uint16_t>(program->address + program->bytes.size());
                const std::uint8_t lo = end & 0xff, hi = end >> 8;
                const std::uint8_t start[2] = {0x01, 0x08};
                const std::uint8_t pointers[6] = {lo, hi, lo, hi, lo, hi};
                const std::uint8_t prgend[2] = {lo, hi};
                target->writeRam(pom2::RamBank::Main, 0x67, start, 2);
                target->writeRam(pom2::RamBank::Main, 0x69, pointers, 6);
                target->writeRam(pom2::RamBank::Main, 0xAF, prgend, 2);
            }
            target->pasteText(program->command);
        }
        if (tape) {
            // At the power-on prompt: the tape in the deck, the command that
            // loads and starts it typed, PLAY pressed. The deck starts when
            // the ROM's READ first polls $C060.
            target->runVideo(static_cast<int>(machineTiming.cpuClockHz * 2));
            if (!target->loadCassette(disks.content.u8string())) {   // `state` moved into `disks` above
                reportError("Cannot load the tape: " + target->lastError());
                return false;
            }
            const auto command = pom2::libretro::tapeCommand(*tape, machine.kind, tapeNote);
            // pasteText drops control keys (a clipboard paste); a leading one
            // (the ]['s Ctrl-B) is typed first, and the paste queues behind it.
            std::size_t typed = 0;
            while (typed < command.size() && command[typed] != '\r'
                   && static_cast<unsigned char>(command[typed]) < 0x20)
                target->queueKey(static_cast<std::uint8_t>(command[typed++]));
            if (typed < command.size()) target->pasteText(command.substr(typed));
            target->playCassette();
        }
    }

    // The ABI buffer may never grow after a content load. A state carries
    // both drives' media: reserve the two largest images of the playlist,
    // and at least a standard SmartPort disk even when starting BASIC.
    constexpr std::size_t minimumDiskBudget = 1u << 20;
    constexpr std::size_t maxEmbeddedDisk = 16u << 20;
    const bool pal = machine.config.timing == pom2::VideoTiming::PAL;
    // Every RamWorks bank rides in the state: 80 KiB each (aux + its LC).
    const std::size_t ramWorksBytes =
        static_cast<std::size_t>(std::max(0, target->ramWorksBanks() - 1)) * 80u * 1024u;
    const std::size_t machineBudget = (pal ? (1536u << 10) : (1u << 20)) + ramWorksBytes;
    std::vector<std::size_t> sizes;
    for (const auto& image : disks.images) {
        std::error_code sizeError;
        const auto size = std::filesystem::file_size(image.path, sizeError);
        if (!sizeError && size <= maxEmbeddedDisk && image.kind() != MediaKind::HardDisk)
            sizes.push_back(static_cast<std::size_t>(size));
    }
    std::sort(sizes.rbegin(), sizes.rend());
    // A hard disk's state is the blocks it changed: room for 8 MiB of them.
    const bool hardDisk = std::any_of(disks.images.begin(), disks.images.end(),
        [](const auto& image) { return image.kind() == MediaKind::HardDisk; });
    const std::size_t diskBudget = std::max(minimumDiskBudget, sizes.empty() ? 0 : sizes[0])
                                 + (sizes.size() > 1 ? sizes[1] : 0) + (hardDisk ? (8u << 20) : 0);
    stateCapacity = machineBudget + diskBudget;
    core = std::move(target);
    applyRuntimeOptions();
    registerMemoryMap();
    retro_keyboard_callback keyboard{keyboardEvent};
    environment(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);
    environment(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, inputDescriptors);
    if (!core->supportsState()) summary += " · no save states";
    if (!tapeNote.empty()) summary += " · " + tapeNote;
    notify(summary);
    return true;
}

// ---- Save states --------------------------------------------------------------

constexpr std::uint8_t kRetroStateMagic[] = {'P', '2', 'L', 'R', 4};
// Magic, then: length, playlist, 2 x (backing identity, index, frames since
// flush, flags), overshoot, two audio remainders, input flags, pad keyboard,
// held keys, repeat, checksum.
constexpr std::size_t kRetroStateHeader =
    sizeof(kRetroStateMagic) + 4 + 8 + 2 * (8 + 4 + 4 + 1) + 4 * 3 + 1 + 2 + 2 + 2 + 2 + 8;
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
    if (!core || !data || size < stateCapacity) return false;
    const auto warnOnce = [](const std::string& message) {
        if (!stateCapacityWarning) {
            stateCapacityWarning = true;
            reportError(message);
        }
    };
    if (!core->saveState(stateScratch)) {
        warnOnce("Save state unavailable: " + core->lastError());
        return false;
    }
    if (stateScratch.size() > stateCapacity - kRetroStateHeader) {
        warnOnce("Save state exceeds this session's fixed buffer. Load the large disk as content "
                 "or include it in the playlist from startup to reserve more space.");
        return false;
    }
    using namespace pom2::byteio;
    std::vector<std::uint8_t> header(std::begin(kRetroStateMagic), std::end(kRetroStateMagic));
    putU32(header, static_cast<std::uint32_t>(stateScratch.size()));
    putU64(header, playlistIdentity());
    for (const auto& slot : disks.drives) {
        const bool writable = slot.mounted && slot.mounted->writable;
        putU64(header, writable ? pathIdentity(slot.mounted->path) : 0);
        putU32(header, slot.index.value_or(kNoDisk));
        putU32(header, slot.framesSinceFlush);
        putU8(header, (slot.ejected ? 1 : 0) | (slot.mounted ? 2 : 0) | (writable ? 4 : 0));
    }
    putU32(header, static_cast<std::uint32_t>(cycleOvershoot));
    putU32(header, machineAudioRemainder);
    putU32(header, frontendAudioRemainder);
    putU8(header, (openApple ? 1 : 0) | (solidApple ? 2 : 0) | (startHeld ? 4 : 0) |
                  (selectHeld ? 8 : 0) | (driveLedOn ? 16 : 0));
    putU8(header, padKeyboard.column);
    putU8(header, padKeyboard.row | (padKeyboard.open ? 16 : 0) | (padKeyboard.shifted ? 32 : 0));
    putU16(header, padKeyboard.held);
    putU16(header, keyPadHeld);
    putU8(header, repeatKey);
    putU8(header, repeatFrames);
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
    struct SavedDrive {
        std::uint64_t backing;
        std::uint32_t index, framesSinceFlush;
        bool ejected, mounted, writable;
    };
    std::array<SavedDrive, 2> saved{};
    for (auto& drive : saved) {
        drive.backing = reader.u64();
        drive.index = reader.u32();
        drive.framesSinceFlush = reader.u32();
        const auto flags = reader.u8();
        drive.ejected = (flags & 1) != 0;
        drive.mounted = (flags & 2) != 0;
        drive.writable = (flags & 4) != 0;
        if (flags > 7) return false;
    }
    const auto overshoot = reader.u32();
    const auto machineRemainder = reader.u32(), frontendRemainder = reader.u32();
    const auto flags = reader.u8(), column = reader.u8(), rowFlags = reader.u8();
    const auto held = reader.u16();
    const auto keysHeld = reader.u16();
    const auto keyRepeated = reader.u8(), repeatCount = reader.u8();
    const auto headerHash = pom2::hashMediaBytes(bytes, reader.pos);
    const auto checksum = reader.u64();
    if (stateCapacity < reader.pos || length > stateCapacity - reader.pos || !reader.has(length) ||
        checksum != pom2::hashMediaBytes(bytes + reader.pos, length, headerHash) ||
        overshoot > 16 || machineRemainder >= static_cast<unsigned>(machineTiming.cpuClockHz) ||
        frontendRemainder >= static_cast<unsigned>(machineTiming.cpuClockHz) || flags > 31 ||
        column >= 12 || (rowFlags & 15) >= 4 || (rowFlags & 192) || keysHeld >= 64) return false;

    std::array<std::optional<MountedDisk>, 2> restored;
    std::array<std::string, 2> backing;
    for (int drive = 0; drive < 2; ++drive) {
        const auto& s = saved[drive];
        if ((s.index != kNoDisk && s.index >= disks.images.size()) || (s.mounted && s.index == kNoDisk) ||
            (s.mounted && s.ejected) || (s.writable && !s.mounted) || bool(s.backing) != s.writable)
            return false;
        if (!s.mounted) continue;
        const auto& image = disks.images[s.index];
        if (drive == 1 && image.kind() != MediaKind::DiskII) return false;
        std::filesystem::path path = image.path;
        std::shared_ptr<pom2::libretro::DiskSaveLock> lease;
        if (s.writable) {
            const auto storage = pom2::libretro::openDiskStorage(disks.saves, disks.content, image.path,
                pom2::libretro::DiskStorageAccess::ExistingOnly);
            if (!storage.privateCopy || pathIdentity(storage.path) != s.backing) return false;
            path = storage.path;
            for (const auto& live : disks.drives)
                if (live.mounted && live.mounted->path == path && live.mounted->lease) lease = live.mounted->lease;
            if (!lease) lease = pom2::libretro::lockDiskStorage(path);
            if (!lease) return false;
            backing[drive] = path.string();
        }
        restored[drive] = MountedDisk{std::move(path), image.kind(), s.writable, std::move(lease)};
    }
    pom2::Core::StateBackings backings;
    // A hard disk is restored in place, block by block, so it takes no backing.
    if (restored[0] && restored[0]->kind == MediaKind::SmartPort) backings.smartPort = backing[0];
    else if (!restored[0] || restored[0]->kind == MediaKind::DiskII) backings.disk[0] = backing[0];
    backings.disk[1] = backing[1];
    // A state rolls back its own disks. Writes to a disk it does not hold
    // are independent and would otherwise vanish when the controller is restored.
    const auto preserveOutgoingDisks = [&] {
        for (int drive = 0; drive < 2; ++drive) {
            const auto& live = disks.drives[drive].mounted;
            if (!live || !live->writable) continue;
            bool kept = false;
            for (const auto& next : restored)
                kept |= next && next->writable && next->path == live->path;
            if (!kept && !(keepDiskEffects() && flushMountedDisk(*core, disks, drive))) return false;
        }
        return true;
    };
    if (!core->loadState(bytes + reader.pos, length, backings, preserveOutgoingDisks)) return false;

    for (int drive = 0; drive < 2; ++drive) {
        auto& slot = disks.drives[drive];
        slot.index = saved[drive].index == kNoDisk ? std::nullopt : std::optional<unsigned>(saved[drive].index);
        slot.ejected = saved[drive].ejected;
        slot.mounted = std::move(restored[drive]);
        slot.framesSinceFlush = saved[drive].framesSinceFlush;
        slot.flushWarning = false;
    }
    cycleOvershoot = static_cast<int>(overshoot);
    machineAudioRemainder = machineRemainder;
    frontendAudioRemainder = frontendRemainder;
    openApple = (flags & 1) != 0;
    solidApple = (flags & 2) != 0;
    startHeld = (flags & 4) != 0;
    selectHeld = (flags & 8) != 0;
    padKeyboard = {held, column, static_cast<std::uint8_t>(rowFlags & 15),
                   (rowFlags & 16) != 0, (rowFlags & 32) != 0};
    keyPadHeld = keysHeld;
    repeatKey = keyRepeated;
    repeatFrames = repeatCount;
    showDiskRead((flags & 16) != 0);
    // A state the player loaded (or a rewind step) moves the drives; the
    // run-ahead's own restores replay a timeline the sounds already played.
    int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
    if (!environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) ||
        context == RETRO_SAVESTATE_CONTEXT_NORMAL)
        core->resyncDriveSounds();
    return true;
}

} // namespace

void retro_set_environment(retro_environment_t callback)
{
    environment = callback;
    if (!environment) return;
    bool noContent = true;
    environment(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noContent);
    pom2::libretro::registerCoreOptions(environment);
    environment(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<retro_controller_info*>(controllerInfo));
    bool achievements = true;
    environment(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
}

void retro_set_video_refresh(retro_video_refresh_t callback) { video = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audioSample = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audioBatch = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { pollInput = callback; }
void retro_set_input_state(retro_input_state_t callback) { inputState = callback; }

static void forwardLog(void*, pom2::LogLevel level, const char* tag, const char* message)
{
    if (!logMessage) return;
    static constexpr retro_log_level kLevels[] = {RETRO_LOG_DEBUG, RETRO_LOG_INFO, RETRO_LOG_WARN, RETRO_LOG_ERROR};
    logMessage(kLevels[static_cast<int>(level)], "POM2 %s: %s\n", tag, message);
}

void retro_init()
{
    supportsPadMask = environment && environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
    supportsExecutionPhase = environment && environment(RETRO_ENVIRONMENT_GET_EXECUTION_PHASE, nullptr);
    if (environment)
        environment(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &driveLed);
    retro_log_callback callback{};
    if (environment && environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &callback))
        logMessage = callback.log;
    if (logMessage) pom2::log().setSink(forwardLog, nullptr);
    if (environment) registerDiskControl();
    portDevice = {RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD};
}

void retro_deinit()
{
    retro_unload_game();
    supportsExecutionPhase = false;
    driveLed = {};
    pom2::log().setSink(nullptr, nullptr);
    logMessage = nullptr;
}

unsigned retro_api_version() { return RETRO_API_VERSION; }

void retro_get_system_info(retro_system_info* info)
{
    static const std::string version = pom2::Core::sdkInfo().version;
    *info = {"POM2", version.c_str(), "dsk|do|po|nib|woz|d13|2mg|2img|hdv|m3u|zip|gz|bin|bas|shk|sdk|bxy|sea|dc|dc42|image|a2r|wav|aci", true, true};
}

void retro_get_system_av_info(retro_system_av_info* info)
{
    *info = {{kWidth, kHeight, kMaxWidth, kHeight, 4.0f / 3.0f},
             {double(machineTiming.cpuClockHz) / machineTiming.cyclesPerFrame, kSampleRate}};
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port >= portDevice.size()) return;
    // Older frontends name the analog stick's device for the joystick.
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_ANALOG) device = RETRO_DEVICE_JOYPAD;
    if (portDevice[port] == device) return;
    if (port == 0) releaseKeyPad();
    if (port == 1 && core) {
        core->setPaddle(2, 128);
        core->setPaddle(3, 128);
        core->setPaddleButton(2, false);
    }
    portDevice[port] = device;
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
        applyRuntimeOptions();
    applyInput();
    applyCheats();
    if (core->driveSoundsAttached()) {
        // Run-ahead runs frames whose audio it discards: their head steps
        // and motor starts are heard on the frame it presents instead.
        int enabled = 3;
        const bool known = environment(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &enabled);
        core->setDriveSoundEventsEnabled(!known || (enabled & RETRO_AV_ENABLE_AUDIO) != 0);
    }
    auto frames = runFrame();
    // "Recently" in CPU cycles: an accelerated frame runs more of them.
    const auto frameCpuCycles = static_cast<std::uint64_t>(machineTiming.cyclesPerFrame * core->cpuSpeedMultiplier());
    const bool readDuringFrame = core->diskReadRecently(frameCpuCycles);
    showDiskRead(readDuringFrame);
    // Keep advancing the audio mixer for the frames skipped by the frontend.
    // Present the final frame's sound and picture at the normal output rate.
    bool keepLoading = settings.fastDiskLoading && readDuringFrame;
    for (unsigned i = 1; keepLoading && i < kMaxDiskLoadFrames; ++i) {
        frames = runFrame();
        keepLoading = core->diskReadRecently(frameCpuCycles);
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
        if (loadGame(game, requestedDisk)) return true;
    } catch (const std::exception& error) {
        reportError(error.what());
    }
    disks = {};
    return false;
}

void retro_unload_game()
{
    showDiskRead(false);
    try {
        if (core && !flushAllDisks(*core, disks)) reportError(core->lastError());
    } catch (const std::exception& error) {
        reportError(error.what());
    }
    core.reset();
    machine = {};
    machineTiming = POM2_TIMING_NTSC;
    disks = {};
    initialDisk.reset();
    clearInput();
    cycleOvershoot = 0;
    machineAudioRemainder = 0;
    frontendAudioRemainder = 0;
    stateCapacity = 0;
    stateCapacityWarning = false;
    rgbWarning = false;
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

void retro_cheat_reset()
{
    cheats.clear();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
    if (index >= 4096) return;
    if (cheats.size() <= index) cheats.resize(index + 1);
    cheats[index].reset();
    if (!enabled || !code) return;
    cheats[index] = pom2::libretro::parseCheat(code);
    if (!cheats[index] && logMessage)
        logMessage(RETRO_LOG_WARN, "POM2: cheat %u is not ADDR:VV, ADDR?CC:VV or AAAAAAVV: %s\n", index, code);
}

void* retro_get_memory_data(unsigned id)
{
    if (!core || (id & RETRO_MEMORY_MASK) != RETRO_MEMORY_SYSTEM_RAM) return nullptr;
    return core->ramRegion(pom2::RamBank::Main).data;
}

std::size_t retro_get_memory_size(unsigned id)
{
    if (!core || (id & RETRO_MEMORY_MASK) != RETRO_MEMORY_SYSTEM_RAM) return 0;
    return core->ramRegion(pom2::RamBank::Main).size;
}
