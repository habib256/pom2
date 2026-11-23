#include <libretro.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
constexpr double ntscRate = 1022727.0 / 17030.0;
constexpr double audioRate = 44100;
constexpr char fastDiskOption[] = "pom2_fast_disk_loading";

void require(bool condition, const char* error)
{
    if (!condition) throw std::runtime_error(error);
}

class ScratchDirectory {
public:
    explicit ScratchDirectory(const fs::path& parent)
    {
        fs::create_directories(parent);
        std::random_device random;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = parent / ("run-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create a unique scratch directory");
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

class Module {
public:
    explicit Module(const fs::path& path)
    {
#ifdef _WIN32
        handle_ = LoadLibraryW(path.c_str());
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) throw std::runtime_error(dlerror());
#endif
        require(handle_ != nullptr, "could not open the libretro module");
    }

    ~Module()
    {
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
    }

    template<typename Function>
    Function symbol(const char* name) const
    {
#ifdef _WIN32
        const auto address = GetProcAddress(handle_, name);
#else
        const auto address = dlsym(handle_, name);
#endif
        if (!address) throw std::runtime_error(std::string("missing ABI symbol: ") + name);
        static_assert(sizeof(Function) == sizeof(address));
        Function result;
        std::memcpy(&result, &address, sizeof(result));
        return result;
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

#define ABI_FUNCTION(name) decltype(&::name) name = module.symbol<decltype(&::name)>(#name)
struct Core {
    Module module;
    ABI_FUNCTION(retro_api_version);
    ABI_FUNCTION(retro_set_environment);
    ABI_FUNCTION(retro_set_video_refresh);
    ABI_FUNCTION(retro_set_audio_sample);
    ABI_FUNCTION(retro_set_audio_sample_batch);
    ABI_FUNCTION(retro_set_input_poll);
    ABI_FUNCTION(retro_set_input_state);
    ABI_FUNCTION(retro_set_controller_port_device);
    ABI_FUNCTION(retro_init);
    ABI_FUNCTION(retro_deinit);
    ABI_FUNCTION(retro_get_system_info);
    ABI_FUNCTION(retro_get_system_av_info);
    ABI_FUNCTION(retro_load_game);
    ABI_FUNCTION(retro_unload_game);
    ABI_FUNCTION(retro_reset);
    ABI_FUNCTION(retro_run);
    ABI_FUNCTION(retro_serialize_size);
    ABI_FUNCTION(retro_serialize);
    ABI_FUNCTION(retro_unserialize);
    ABI_FUNCTION(retro_cheat_reset);
    ABI_FUNCTION(retro_cheat_set);
    ABI_FUNCTION(retro_get_memory_data);
    ABI_FUNCTION(retro_get_memory_size);
};
#undef ABI_FUNCTION

struct Frontend {
    std::string directory;
    std::string saveDirectoryOverride;
    std::string messages;
    const char* fastDiskValue = nullptr;
    bool fastDiskEnabledByDefault = false;
    unsigned executionPhaseProbes = 0;
    unsigned executionPhaseQueries = 0;
    retro_keyboard_event_t keyboard = nullptr;
    bool supportNoGame = false;
    bool xrgb8888 = false;
    bool badFrame = false;
    bool nonzeroAudio = false;
    bool supportsDriveLed = false;
    bool driveLedOn = false;
    unsigned driveReadSignals = 0;
    bool button = false;
    bool start = false;
    bool nextDisk = false;
    unsigned diskControlVersion = 1;
    unsigned extendedRegistrations = 0;
    bool rejectExtendedDiskControl = false;
    retro_disk_control_callback diskControl{};
    retro_disk_control_ext_callback diskControlExt{};
    uint64_t audioFrames = 0;
    uint64_t audioHash = 0;
    unsigned audioBatches = 0;
    unsigned videoFrames = 0;
    unsigned polls = 0;
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint32_t> pixels;
    // Core options: the version offered, what a v2 core registered, the
    // values the "user" chose, and whether they changed since the last poll.
    unsigned optionsVersion = 0;
    unsigned language = RETRO_LANGUAGE_ENGLISH;
    const retro_core_options_v2* optionsUs = nullptr;
    const retro_core_options_v2* optionsLocal = nullptr;
    std::map<std::string, std::string> options;
    bool optionsUpdated = false;
    std::vector<retro_memory_descriptor> memoryMap;
    bool achievements = false;
    unsigned pad = 0;          // RetroPad 1 buttons, as a bit mask
    unsigned pad2 = 0;         // RetroPad 2
};
Frontend frontend;

void RETRO_CALLCONV setLed(int index, int state)
{
    require(index == 0, "unexpected drive LED index");
    frontend.driveLedOn = state != 0;
    if (frontend.driveLedOn) ++frontend.driveReadSignals;
}

void logMessage(enum retro_log_level, const char* format, ...)
{
    std::array<char, 1024> message{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message.data(), message.size(), format, args);
    va_end(args);
    frontend.messages += message.data();
}

bool environment(unsigned command, void* data)
{
    switch (command) {
    case RETRO_ENVIRONMENT_GET_EXECUTION_PHASE:
        if (data) ++frontend.executionPhaseQueries;
        else ++frontend.executionPhaseProbes;
        return false;
    case RETRO_ENVIRONMENT_GET_LED_INTERFACE:
        if (!frontend.supportsDriveLed) return false;
        static_cast<retro_led_interface*>(data)->set_led_state = setLed;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = frontend.saveDirectoryOverride.empty()
            ? frontend.directory.c_str() : frontend.saveDirectoryOverride.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char**>(data) = frontend.directory.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        frontend.supportNoGame = *static_cast<const bool*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        frontend.xrgb8888 = *static_cast<const retro_pixel_format*>(data) == RETRO_PIXEL_FORMAT_XRGB8888;
        return frontend.xrgb8888;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = logMessage;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        frontend.messages += static_cast<const retro_message*>(data)->msg;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        frontend.messages += static_cast<const retro_message_ext*>(data)->msg;
        return true;
    case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
        frontend.keyboard = static_cast<const retro_keyboard_callback*>(data)->callback;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *static_cast<unsigned*>(data) = frontend.diskControlVersion;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        frontend.diskControl = *static_cast<const retro_disk_control_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        ++frontend.extendedRegistrations;
        if (frontend.rejectExtendedDiskControl) return false;
        frontend.diskControlExt = *static_cast<const retro_disk_control_ext_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        const auto* variable = static_cast<const retro_variable*>(data);
        for (; variable->key; ++variable) {
            if (std::strcmp(variable->key, fastDiskOption) != 0) continue;
            const char* values = std::strstr(variable->value, "; ");
            frontend.fastDiskEnabledByDefault = values && std::strncmp(values + 2, "enabled|", 8) == 0;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto& variable = *static_cast<retro_variable*>(data);
        if (std::strcmp(variable.key, fastDiskOption) == 0 && frontend.fastDiskValue) {
            variable.value = frontend.fastDiskValue;
            return true;
        }
        const auto chosen = frontend.options.find(variable.key);
        if (chosen == frontend.options.end()) return false;
        variable.value = chosen->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = std::exchange(frontend.optionsUpdated, false);
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = frontend.optionsVersion;
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = frontend.language;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        if (frontend.optionsVersion < 2) return false;
        const auto* intl = static_cast<const retro_core_options_v2_intl*>(data);
        frontend.optionsUs = intl->us;
        frontend.optionsLocal = intl->local;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return frontend.optionsVersion >= 2;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
        const auto* map = static_cast<const retro_memory_map*>(data);
        frontend.memoryMap.assign(map->descriptors, map->descriptors + map->num_descriptors);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        frontend.achievements = *static_cast<const bool*>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    default:
        return false;
    }
}

void video(const void* data, unsigned width, unsigned height, size_t pitch)
{
    ++frontend.videoFrames;
    if (!data || !frontend.xrgb8888 || (width != 280 && width != 560) ||
        height != 192 || pitch < width * sizeof(uint32_t)) {
        frontend.badFrame = true;
        return;
    }
    frontend.width = width;
    frontend.height = height;
    frontend.pixels.resize(static_cast<size_t>(width) * height);
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (unsigned row = 0; row < height; ++row) {
        std::memcpy(frontend.pixels.data() + row * width, bytes + row * pitch,
                    width * sizeof(uint32_t));
    }
}

void audioSample(int16_t left, int16_t right)
{
    ++frontend.audioFrames;
    frontend.nonzeroAudio |= left != 0 || right != 0;
    frontend.audioHash = (frontend.audioHash ^ static_cast<uint16_t>(left)) * 1099511628211ull;
    frontend.audioHash = (frontend.audioHash ^ static_cast<uint16_t>(right)) * 1099511628211ull;
}

size_t audioBatch(const int16_t* data, size_t frames)
{
    ++frontend.audioBatches;
    for (size_t frame = 0; frame < frames; ++frame) audioSample(data[2 * frame], data[2 * frame + 1]);
    return frames;
}

void inputPoll() { ++frontend.polls; }

int16_t inputState(unsigned port, unsigned device, unsigned, unsigned id)
{
    if (port == 1 && device == RETRO_DEVICE_JOYPAD && id < 16) return (frontend.pad2 >> id) & 1;
    if (port == 0 && device == RETRO_DEVICE_JOYPAD) {
        if (id < 16 && ((frontend.pad >> id) & 1)) return 1;
        if (id == RETRO_DEVICE_ID_JOYPAD_B) return frontend.button;
        if (id == RETRO_DEVICE_ID_JOYPAD_START) return frontend.start;
        if (id == RETRO_DEVICE_ID_JOYPAD_R3) return frontend.nextDisk;
    }
    return 0;
}

Bytes readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "could not open file for comparison");
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}

void writeFile(const fs::path& path, const Bytes& bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "could not write a test firmware fixture");
}

Bytes inputRom()
{
    // $D000: select page 1 text, seed the first cell, then echo the keyboard
    // latch to $0400 and PB0 to $0401. Toggle the speaker continuously while
    // delaying with X. The two text cells remain independent input probes.
    constexpr uint8_t program[] = {
        0x78,                    // SEI
        0xAD, 0x51, 0xC0,        // LDA $C051 (text)
        0xAD, 0x54, 0xC0,        // LDA $C054 (page 1)
        0xA9, 0xC1,              // LDA #$C1 ('A')
        0x8D, 0x00, 0x04,        // STA $0400
        0xAD, 0x00, 0xC0,        // loop: LDA $C000 (keyboard latch)
        0x10, 0x0E,              // BPL no_key
        0x8D, 0x00, 0x04,        // STA $0400
        0xA8,                    // TAY (retain key across the strobe read)
        0xAD, 0x10, 0xC0,        // LDA $C010 (clear strobe)
        0xC0, 0xA1,              // CPY #'!' (guest command to boot the drive)
        0xD0, 0x03,              // BNE no_key
        0x4C, 0x00, 0xC6,        // JMP $C600
        0xA2, 0xC2,              // no_key: LDX #$C2 ('B')
        0xAD, 0x61, 0xC0,        // LDA $C061 (pushbutton 0)
        0x10, 0x01,              // BPL released
        0xE8,                    // INX ('C')
        0x8E, 0x01, 0x04,        // released: STX $0401
        0xAD, 0x30, 0xC0,        // LDA $C030 (speaker)
        0xA2, 0xFF,              // LDX #$FF
        0xCA,                    // delay: DEX
        0xD0, 0xFD,              // BNE delay
        0x4C, 0x0C, 0xD0,        // JMP loop
    };
    Bytes rom(0x4000, 0xea);
    std::copy(std::begin(program), std::end(program), rom.begin() + 0x1000);
    rom[0x3f58] = 0x60; // RTS at the Disk II PROM's monitor slot-detection seam.
    for (size_t offset : {0x3ffau, 0x3ffcu, 0x3ffeu}) {
        rom[offset] = 0x00;
        rom[offset + 1] = 0xd0;
    }
    return rom;
}

Bytes glyphRom()
{
    // Encode each glyph's index directly into every scanline. Distinct text
    // characters then produce distinct cell pixels without any external ROM.
    Bytes bytes(4096);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i / 8) & 0x7f);
    return bytes;
}

std::vector<uint32_t> cell(unsigned index)
{
    std::vector<uint32_t> result;
    const unsigned size = frontend.width / 40;
    for (unsigned y = 0; y < 8; ++y) {
        const auto start = frontend.pixels.begin() + y * frontend.width + index * size;
        result.insert(result.end(), start, start + size);
    }
    return result;
}

std::array<uint8_t, 8> glyphMask(unsigned x, unsigned y)
{
    std::array<uint8_t, 8> rows{};
    const auto scale = frontend.width / 280;
    for (unsigned row = 0; row < rows.size(); ++row) {
        for (unsigned column = 0; column < 7; ++column) {
            const auto pixel = frontend.pixels[(y * 8 + row) * frontend.width + (x * 7 + column) * scale];
            if (pixel & 0x00ffffffu) rows[row] |= static_cast<uint8_t>(1u << column);
        }
    }
    return rows;
}

unsigned litPixels(unsigned left, unsigned top, unsigned right, unsigned bottom)
{
    const auto scale = frontend.width / 280;
    unsigned count = 0;
    for (unsigned y = top; y < bottom; ++y) {
        for (unsigned x = left; x < right; ++x) {
            count += (frontend.pixels[y * frontend.width + x * scale] & 0x00ffffffu) != 0;
        }
    }
    return count;
}

bool hasDirtBikeLogo()
{
    // The packed disk's actual HGR logo bitmap, excluding the blinking
    // prompt. Hash lit dots rather than colors. Power-on RAM can itself
    // produce a bright frame before the drive has loaded any game code.
    constexpr std::uint32_t logoHash = 0x90f673bd;
    const auto scale = frontend.width / 280;
    std::uint32_t hash = 2166136261u;
    for (unsigned y = 12; y < 45; ++y) {
        for (unsigned x = 10; x < 270; ++x) {
            const auto pixel = frontend.pixels[y * frontend.width + x * scale];
            hash = (hash ^ ((pixel & 0x00ffffffu) != 0)) * 16777619u;
        }
    }
    return hash == logoHash;
}

void run(Core& core, unsigned frames)
{
    const unsigned beforeVideo = frontend.videoFrames;
    const unsigned beforePolls = frontend.polls;
    const unsigned beforeBatches = frontend.audioBatches;
    const uint64_t beforeAudio = frontend.audioFrames;
    for (unsigned i = 0; i < frames; ++i) core.retro_run();
    require(!frontend.badFrame && frontend.videoFrames - beforeVideo == frames,
            "expected one valid 280/560 x 192 XRGB8888 frame per retro_run");
    require(frontend.polls - beforePolls == frames, "expected one input poll per retro_run");
    require(frontend.audioBatches - beforeBatches == frames, "expected one audio batch per retro_run");
    const double expected = frames * audioRate / ntscRate;
    require(std::abs(static_cast<double>(frontend.audioFrames - beforeAudio) - expected) <= 2,
            "audio sample cadence drifted from the reported frame rate");
}

Bytes saveState(Core& core)
{
    Bytes state(core.retro_serialize_size());
    require(!state.empty() && core.retro_serialize(state.data(), state.size()),
            "could not capture libretro state");
    return state;
}

void testDeterministicReplay(Core& core)
{
    frontend.keyboard(true, RETROK_n, 'n', 0);
    const auto state = saveState(core);
    auto replay = [&] {
        std::vector<std::array<uint64_t, 3>> outputs;
        for (unsigned frame = 0; frame < 600; ++frame) {
            if (frame == 2) frontend.keyboard(false, RETROK_n, 'n', 0);
            if (frame == 177) frontend.keyboard(true, RETROK_y, 'Y', RETROKMOD_SHIFT);
            if (frame == 178) frontend.keyboard(false, RETROK_y, 'Y', 0);
            frontend.button = frame % 40 < 20;
            frontend.start = frame % 137 == 10;
            frontend.audioHash = 0xcbf29ce484222325ull;
            const auto samples = frontend.audioFrames;
            core.retro_run();
            uint64_t videoHash = 0xcbf29ce484222325ull;
            for (auto pixel : frontend.pixels) videoHash = (videoHash ^ pixel) * 1099511628211ull;
            outputs.push_back({videoHash, frontend.audioHash, frontend.audioFrames - samples});
        }
        return outputs;
    };
    const auto first = replay();
    require(core.retro_unserialize(state.data(), state.size()), "could not restore a valid libretro state");
    const auto second = replay();
    for (size_t i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) {
            std::fprintf(stderr, "Replay frame %zu diverged (video %d, audio %d, samples %d)\n",
                         i, first[i][0] != second[i][0], first[i][1] != second[i][1],
                         first[i][2] != second[i][2]);
            throw std::runtime_error("video or audio diverged over 600 frames after restore");
        }
    }
    auto corrupt = state;
    corrupt[128] ^= 1;
    const auto after = saveState(core);
    require(!core.retro_unserialize(corrupt.data(), corrupt.size()) &&
            !core.retro_unserialize(state.data(), 31) && saveState(core) == after,
            "corrupt or truncated state changed the machine");
    frontend.button = false;
    frontend.start = false;
}

void testFixture(Core& core, const fs::path& firmware)
{
    require(!core.retro_load_game(nullptr), "missing firmware unexpectedly loaded");
    require(frontend.messages.find("apple2e.rom") != std::string::npos,
            "missing firmware diagnostic should name apple2e.rom");
    frontend.messages.clear();
    writeFile(firmware / "apple2e.rom", inputRom());
    require(!core.retro_load_game(nullptr), "missing character firmware unexpectedly loaded");
    require(frontend.messages.find("apple2e_char.rom") != std::string::npos,
            "missing character firmware diagnostic should name apple2e_char.rom");
    writeFile(firmware / "apple2e_char.rom", Bytes(1, 0));
    require(!core.retro_load_game(nullptr), "truncated character firmware unexpectedly loaded");
    writeFile(firmware / "apple2e_char.rom", glyphRom());
    require(core.retro_load_game(nullptr), "could not load the synthetic firmware without content");
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    retro_system_av_info av{};
    core.retro_get_system_av_info(&av);
    require(std::abs(av.timing.fps - ntscRate) < 0.01, "unexpected NTSC frame rate");
    require(av.timing.sample_rate == audioRate, "unexpected audio sample rate");
    require(std::abs(av.geometry.aspect_ratio - 4.0f / 3.0f) < 0.001f, "unexpected display aspect");
    run(core, 60);
    require(frontend.nonzeroAudio, "guest speaker toggles did not reach the audio callback");

    require(frontend.keyboard != nullptr, "frontend did not receive a keyboard callback");
    const auto beforeKey = cell(0);
    const auto beforeButton = cell(1);
    require(beforeKey != beforeButton, "test firmware did not render distinct starting glyphs");
    frontend.keyboard(true, RETROK_b, 'b', 0);
    frontend.keyboard(false, RETROK_b, 'b', 0);
    run(core, 2);
    require(cell(0) != beforeKey && cell(1) == beforeButton, "keyboard event did not reach the guest latch");
    const auto afterKey = cell(0);
    frontend.button = true;
    run(core, 2);
    require(cell(1) != beforeButton && cell(0) == afterKey, "RetroPad B did not drive guest pushbutton 0");
    frontend.button = false;
    run(core, 2);
    require(cell(1) == beforeButton, "releasing RetroPad B did not release guest pushbutton 0");
    testDeterministicReplay(core);
    core.retro_unload_game();
    require(core.retro_load_game(nullptr), "no-content reload failed");
    run(core, 2);
    require(cell(0) == beforeKey && cell(1) == beforeButton, "a new load retained previous keyboard or pad state");
    core.retro_unload_game();
}

void writeBootDisk(const fs::path& path, uint8_t marker)
{
    // Disk II loads one sector to $0800 and enters at $0801. Display a
    // disk-specific glyph, stop the motor, and enter the synthetic ROM's
    // input loop without rerunning its initialization.
    const uint8_t sector[] = {
        0x01,                    // One boot sector.
        0xAD, 0x51, 0xC0,        // LDA $C051 (text)
        0xAD, 0x54, 0xC0,        // LDA $C054 (page 1)
        0xAD, 0xE8, 0xC0,        // LDA $C0E8 (motor off)
        0xA9, marker,            // LDA #marker
        0x8D, 0x00, 0x04,        // STA $0400
        0x4C, 0x0C, 0xD0,        // JMP $D00C (input loop)
    };
    Bytes disk(35 * 16 * 256);
    std::copy(std::begin(sector), std::end(sector), disk.begin());
    writeFile(path, disk);
}

void writePaddedWoz(const fs::path& path)
{
    // A valid WOZ1 surface with one tiny track and a large ignored chunk.
    // This tests snapshot sizing without relying on an external disk.
    Bytes disk{'W', 'O', 'Z', '1', 0xff, 10, 13, 10, 0, 0, 0, 0};
    const auto chunk = [&](const char* tag, const Bytes& payload) {
        disk.insert(disk.end(), tag, tag + 4);
        const auto length = static_cast<uint32_t>(payload.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            disk.push_back(static_cast<uint8_t>(length >> shift));
        disk.insert(disk.end(), payload.begin(), payload.end());
    };
    Bytes info(60);
    info[0] = info[1] = info[2] = 1;
    chunk("INFO", info);
    Bytes map(160, 0xff);
    map.front() = 0;
    chunk("TMAP", map);
    Bytes track(6656);
    track[0] = 0xd5;
    track[6646] = 1;
    track[6648] = 8;
    track[6650] = track[6651] = 0xff;
    chunk("TRKS", track);
    chunk("JUNK", Bytes(3u << 20));
    writeFile(path, disk);
}

retro_game_info contentInfo(const std::string& path)
{
    return {path.c_str(), nullptr, 0, nullptr};
}

std::vector<uint32_t> typeMarker(Core& core, unsigned key, char character)
{
    frontend.keyboard(true, key, character, 0);
    frontend.keyboard(false, key, character, 0);
    run(core, 2);
    return cell(0);
}

void waitForBootMarker(Core& core, const std::vector<uint32_t>& expected)
{
    // The core powers the machine on and lets the ROM boot the disk. The
    // synthetic ROM has no slot scan: '!' is its boot command (JMP $C600).
    run(core, 2);
    if (cell(0) != expected) {
        frontend.keyboard(true, RETROK_EXCLAIM, '!', 0);
        frontend.keyboard(false, RETROK_EXCLAIM, '!', 0);
    }
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        run(core, 15);
        if (cell(0) == expected) return;
    }
    throw std::runtime_error("the selected boot disk did not run its own sector");
}

template<typename DiskControl>
void requireDiskCallbacks(const DiskControl& disk)
{
    require(disk.set_eject_state && disk.get_eject_state && disk.get_image_index &&
            disk.set_image_index && disk.get_num_images && disk.replace_image_index &&
            disk.add_image_index, "disk control did not register all mandatory callbacks");
}

template<typename DiskControl>
void requireEmptyDiskState(const DiskControl& disk)
{
    require(!disk.get_eject_state() && disk.get_num_images() == 0 && disk.get_image_index() == 0,
            "unloaded disk control retained a tray, disk list, or selection");
    require(!disk.add_image_index() && !disk.set_eject_state(true),
            "disk control accepted a state change after unload");
}

void requireDiskEntry(unsigned index, const std::string& path, const char* label)
{
    std::array<char, 4096> buffer{};
    const auto& disk = frontend.diskControlExt;
    require(disk.get_image_path(index, buffer.data(), buffer.size()) &&
            fs::path(buffer.data()) == fs::path(path), "incorrect or unresolvable playlist disk path");
    require(disk.get_image_label(index, buffer.data(), buffer.size()) &&
            std::strcmp(buffer.data(), label) == 0, "incorrect playlist disk label");
}

void testExtendedDisks(Core& core, const fs::path& work, const std::string& firstPath,
                       const std::string& secondPath, const std::vector<uint32_t>& firstMarker,
                       const std::vector<uint32_t>& secondMarker)
{
    const auto& disk = frontend.diskControlExt;
    requireDiskCallbacks(disk);
    require(disk.set_initial_image && disk.get_image_path && disk.get_image_label,
            "extended disk control did not register path, label, or initial selection callbacks");
    require(frontend.extendedRegistrations == 1 && !frontend.diskControl.set_eject_state,
            "frontend with disk control v1 did not use only the extended interface");
    requireEmptyDiskState(disk);

    const std::string playlistPath = (work / "Disk set.m3u").string();
    const std::string playlist = "\xef\xbb\xbf#EXTM3U\r\n#EXTINF:0,First boot\r\n# comment\r\n\r\n"
        "  disks/Boot A.dsk  \r\n#EXTINF:0,Ignored label\r\n" +
        fs::path(secondPath).generic_string() + "|Second boot\r\n";
    writeFile(playlistPath, Bytes(playlist.begin(), playlist.end()));
    const auto info = contentInfo(playlistPath);

    require(!disk.set_initial_image(1, nullptr) && !disk.set_initial_image(1, ""),
            "initial disk accepted an absent path");
    require(disk.set_initial_image(1, secondPath.c_str()) && core.retro_load_game(&info),
            "could not load a playlist with a valid initial selection");
    require(!disk.get_eject_state() && disk.get_num_images() == 2 && disk.get_image_index() == 1,
            "playlist did not select the requested second disk");
    requireDiskEntry(0, firstPath, "First boot");
    requireDiskEntry(1, secondPath, "Second boot");
    char shortBuffer[2]{};
    require(!disk.get_image_path(0, shortBuffer, sizeof shortBuffer) &&
            !disk.get_image_label(0, shortBuffer, sizeof shortBuffer) &&
            !disk.get_image_path(2, shortBuffer, sizeof shortBuffer), "disk metadata accepted invalid output arguments");
    waitForBootMarker(core, secondMarker);

    frontend.nextDisk = true;
    run(core, 4);
    require(disk.get_image_index() == 0 && cell(0) == secondMarker,
            "holding R3 should change sides once without rebooting the game");
    frontend.nextDisk = false;
    run(core, 1);
    frontend.nextDisk = true;
    run(core, 1);
    frontend.nextDisk = false;
    run(core, 1);
    require(disk.get_image_index() == 1, "R3 did not return to the second side");

    const auto first = contentInfo(firstPath);
    const auto second = contentInfo(secondPath);
    const auto insertedState = saveState(core);
    require(disk.set_eject_state(true) && disk.set_image_index(0), "could not select disk while ejected");
    const auto emptyState = saveState(core);
    require(disk.set_eject_state(false) && core.retro_unserialize(insertedState.data(), insertedState.size()) &&
            !disk.get_eject_state() && disk.get_image_index() == 1,
            "restoring loaded state did not reselect the second disk");
    require(core.retro_unserialize(emptyState.data(), emptyState.size()) &&
            disk.get_eject_state() && disk.get_image_index() == 0,
            "restoring ejected state inserted a disk or lost its selected index");
    require(core.retro_unserialize(insertedState.data(), insertedState.size()) &&
            !disk.get_eject_state() && disk.get_image_index() == 1,
            "restoring the second disk from an empty drive failed");
    require(!disk.set_image_index(0) && !disk.replace_image_index(0, &second),
            "closed tray accepted a change to the selected disk or its list entry");
    require(disk.add_image_index() && disk.get_num_images() == 3, "could not add an empty slot to a closed tray's list");
    require(disk.set_eject_state(true) && disk.get_eject_state(), "could not eject the running disk");
    require(!disk.set_image_index(2) && !disk.replace_image_index(3, &first),
            "disk control accepted an unpopulated or out-of-list slot");
    const std::string missingPath = (work / "missing.dsk").string();
    const auto missing = contentInfo(missingPath);
    require(!disk.replace_image_index(2, &missing) && !disk.replace_image_index(2, &info),
            "disk control replacement accepted a missing disk or a playlist");
    require(disk.replace_image_index(2, &first), "could not populate the added slot");
    requireDiskEntry(2, firstPath, "Boot A");
    require(disk.set_image_index(2) && disk.replace_image_index(0, nullptr) &&
            disk.get_num_images() == 2 && disk.get_image_index() == 1,
            "removal did not shift the selected disk and list index together");
    require(disk.set_eject_state(false) && !disk.get_eject_state(), "could not insert the replacement at runtime");
    run(core, 120);
    require(cell(0) == secondMarker, "hot disk change rebooted the guest into the replacement disk");
    require(typeMarker(core, RETROK_z, 'Z') != secondMarker, "guest input stopped after hot disk change");
    typeMarker(core, RETROK_EXCLAIM, '!');
    waitForBootMarker(core, firstMarker);
    core.retro_reset();
    require(!disk.get_eject_state() && disk.get_image_index() == 1, "reset changed the inserted disk or tray state");
    require(disk.set_eject_state(true), "could not eject the replacement disk");
    const std::string corruptPath = (work / "corrupt.dsk").string();
    writeFile(corruptPath, Bytes(3));
    const auto corrupt = contentInfo(corruptPath);
    require(disk.replace_image_index(1, &corrupt) && !disk.set_eject_state(false) &&
            disk.get_eject_state(), "an unmountable disk closed the tray or damaged the disk list");
    require(disk.replace_image_index(1, &first) && disk.set_eject_state(false),
            "could not recover from a failed insertion");
    require(disk.set_eject_state(true) && disk.set_image_index(disk.get_num_images()) &&
            disk.set_eject_state(false) && disk.get_image_index() == disk.get_num_images(),
            "closing the tray with no disk selected failed");
    core.retro_unload_game();
    requireEmptyDiskState(disk);

    require(core.retro_load_game(&info) && disk.get_image_index() == 0,
            "a new playlist load reused an already-consumed initial hint");
    waitForBootMarker(core, firstMarker);
    core.retro_unload_game();
    requireEmptyDiskState(disk);

    // Stale path and out-of-range index must both ignore the frontend's hint.
    for (const unsigned index : {1u, 9u}) {
        require(disk.set_initial_image(index, firstPath.c_str()) && core.retro_load_game(&info) &&
                disk.get_image_index() == 0, "invalid initial disk hint did not fall back to disk zero");
        waitForBootMarker(core, firstMarker);
        core.retro_unload_game();
        requireEmptyDiskState(disk);
    }

    const std::string invalidPath = (work / "invalid.m3u").string();
    const auto invalid = contentInfo(invalidPath);
    for (const std::string body : {"# no disks\n", "disks/Boot A.dsk\nmissing.dsk\n",
                                  "Disk set.m3u\n", "unsupported.hdv\n"}) {
        writeFile(invalidPath, Bytes(body.begin(), body.end()));
        require(!core.retro_load_game(&invalid), "malformed or unavailable playlist unexpectedly loaded");
        requireEmptyDiskState(disk);
    }
}

void testLegacyDisks(Core& core, const std::string& firstPath, const std::string& secondPath,
                     const std::vector<uint32_t>& firstMarker, const std::vector<uint32_t>& secondMarker)
{
    core.retro_deinit();
    frontend.diskControlVersion = 0;
    frontend.diskControlExt = {};
    frontend.extendedRegistrations = 0;
    core.retro_init();
    const auto& disk = frontend.diskControl;
    requireDiskCallbacks(disk);
    require(frontend.extendedRegistrations == 0, "frontend v0 was offered extended disk control");
    const auto first = contentInfo(firstPath);
    const auto second = contentInfo(secondPath);
    require(core.retro_load_game(&first) && disk.get_num_images() == 1 && disk.get_image_index() == 0 &&
            !disk.get_eject_state(), "legacy interface did not expose the inserted standalone disk");
    waitForBootMarker(core, firstMarker);
    require(disk.add_image_index() && disk.set_eject_state(true) && disk.get_eject_state() &&
            disk.replace_image_index(1, &second) && disk.set_image_index(1) && disk.set_eject_state(false) &&
            disk.get_image_index() == 1 && !disk.get_eject_state(), "legacy callbacks could not swap disks");
    run(core, 120);
    require(cell(0) == firstMarker, "legacy disk swap rebooted the guest");
    typeMarker(core, RETROK_EXCLAIM, '!');
    waitForBootMarker(core, secondMarker);
    core.retro_unload_game();
    requireEmptyDiskState(disk);

    core.retro_deinit();
    frontend.diskControlVersion = 1;
    frontend.rejectExtendedDiskControl = true;
    frontend.diskControl = {};
    core.retro_init();
    require(frontend.extendedRegistrations == 1, "the core did not try the advertised extended disk control");
    requireDiskCallbacks(disk);
    requireEmptyDiskState(disk);
    require(core.retro_load_game(nullptr), "could not boot without content through the legacy frontend");
    const auto previous = typeMarker(core, RETROK_z, 'Z');
    require(disk.get_num_images() == 0 && disk.add_image_index() && disk.set_eject_state(true) &&
            disk.replace_image_index(0, &first) && disk.set_image_index(0) && disk.set_eject_state(false),
            "could not hot-insert the first disk after a no-content boot");
    run(core, 30);
    require(cell(0) == previous, "adding the first disk rebooted a no-content session");
    typeMarker(core, RETROK_EXCLAIM, '!');
    waitForBootMarker(core, firstMarker);
    core.retro_unload_game();
    requireEmptyDiskState(disk);

    require(core.retro_load_game(nullptr), "could not boot BASIC before adding SmartPort media");
    const auto capacity = core.retro_serialize_size();
    const auto smartPortPath = (fs::path(firstPath).parent_path() / "SmartPort 800K.po").string();
    writeFile(smartPortPath, Bytes(800 * 1024));
    const auto smartPort = contentInfo(smartPortPath);
    require(disk.add_image_index() && disk.set_eject_state(true) &&
            disk.replace_image_index(0, &smartPort) && disk.set_image_index(0) &&
            disk.set_eject_state(false), "could not hot-insert an 800 KiB SmartPort disk");
    require(core.retro_serialize_size() == capacity, "hot insertion changed the save-state capacity");
    const auto inserted = saveState(core);
    require(disk.set_eject_state(true) && core.retro_unserialize(inserted.data(), inserted.size()) &&
            !disk.get_eject_state() && disk.get_image_index() == 0 &&
            core.retro_serialize_size() == capacity,
            "could not restore the hot-inserted SmartPort disk within the BASIC session's capacity");
    core.retro_unload_game();

    const auto wozPath = (fs::path(firstPath).parent_path() / "Large.woz").string();
    writePaddedWoz(wozPath);
    const auto woz = contentInfo(wozPath);
    require(core.retro_load_game(nullptr) && disk.add_image_index() && disk.set_eject_state(true) &&
            disk.replace_image_index(0, &woz) && disk.set_image_index(0) && disk.set_eject_state(false),
            "large WOZ could not be hot-inserted");
    require(core.retro_serialize_size() == capacity, "large WOZ changed the fixed save-state capacity");
    Bytes tooSmall(capacity);
    const auto previousMessages = frontend.messages.size();
    require(!core.retro_serialize(tooSmall.data(), tooSmall.size()) &&
            frontend.messages.find("fixed buffer", previousMessages) != std::string::npos,
            "large hot-inserted WOZ did not report the buffer limit");
    const auto afterWarning = frontend.messages.size();
    require(!core.retro_serialize(tooSmall.data(), tooSmall.size()) &&
            afterWarning == frontend.messages.size(), "repeated save requests spammed the buffer warning");
    core.retro_unload_game();
    require(core.retro_load_game(&woz) && core.retro_serialize_size() > capacity,
            "starting with the large WOZ did not reserve additional save-state space");
    saveState(core);
    core.retro_unload_game();
}

void testDiskControl(Core& core, const fs::path& work)
{
    require(core.retro_load_game(nullptr), "could not boot the synthetic ROM to generate test glyphs");
    run(core, 2);
    const auto firstMarker = typeMarker(core, RETROK_d, 'D');
    const auto secondMarker = typeMarker(core, RETROK_e, 'E');
    require(firstMarker != secondMarker, "synthetic firmware rendered equal markers for the two boot disks");
    core.retro_unload_game();

    fs::create_directory(work / "disks");
    const auto firstPath = fs::absolute(work / "disks" / "Boot A.dsk").string();
    const auto secondPath = fs::absolute(work / "Boot B.DO").string();
    writeBootDisk(firstPath, 0xC4);
    writeBootDisk(secondPath, 0xC5);
    const auto firstOriginal = readFile(firstPath);
    const auto secondOriginal = readFile(secondPath);
    testExtendedDisks(core, work, firstPath, secondPath, firstMarker, secondMarker);
    testLegacyDisks(core, firstPath, secondPath, firstMarker, secondMarker);
    require(readFile(firstPath) == firstOriginal && readFile(secondPath) == secondOriginal,
            "disk control modified a supplied image");
}

unsigned testDirtBike(Core& core, const fs::path& disk, bool fast)
{
    const char* mode = fast ? "default fast" : "original";
    frontend.fastDiskValue = fast ? nullptr : "disabled";
    std::printf("Booting DIRTBIKE with %s disk loading\n", mode);
    const std::string path = disk.string();
    const retro_game_info game{path.c_str(), nullptr, 0, nullptr};
    require(core.retro_load_game(&game), "could not load DIRTBIKE.dsk");
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    constexpr unsigned bootBudget = 1800;
    const auto readsBeforeBoot = frontend.driveReadSignals;
    unsigned titleFrame = 0;
    for (unsigned frame = 1; frame <= bootBudget; ++frame) {
        run(core, 1);
        if (hasDirtBikeLogo()) {
            titleFrame = frame;
            break;
        }
    }
    require(titleFrame != 0, "the disk did not reach the DIRTBIKE graphics title");
    require(frontend.driveReadSignals > readsBeforeBoot, "frontend did not see Disk II read activity");
    run(core, 15);
    require(hasDirtBikeLogo(), "the DIRTBIKE graphics title was not stable");
    std::printf("DIRTBIKE graphics title with %s loading: %u host frames (%.2f s at %.2f Hz)\n",
                mode, titleFrame, titleFrame / ntscRate, ntscRate);
    const auto pressStart = [&] {
        frontend.start = true;
        run(core, 10);
        frontend.start = false;
    };
    pressStart();
    run(core, 60);
    require(litPixels(0, 32, 110, 144) > 1800 && litPixels(140, 32, 245, 144) > 1800,
            "RetroPad Start did not open the two-column track menu");
    pressStart();
    run(core, 360);
    // Easy Ride starts on a bright flat strip of ground. Check the rider
    // above it, outside the HUD. Its starting position is around x=60.
    require(litPixels(0, 165, 280, 175) > 2500 && litPixels(35, 115, 82, 150) > 30,
            "RetroPad Start did not enter the first race");
    // Once the drive is idle, 150 host frames must cover about 2.5 seconds
    // on the race clock. Observe the seconds digit as it changes on screen.
    auto seconds = glyphMask(4, 0);
    unsigned secondsChanges = 0;
    for (unsigned frame = 0; frame < 150; ++frame) {
        run(core, 1);
        const auto next = glyphMask(4, 0);
        secondsChanges += seconds != next;
        seconds = next;
    }
    require(secondsChanges >= 2 && secondsChanges <= 3, "race clock advanced at the wrong speed");
    require(!frontend.driveLedOn, "disk LED stayed on during the race");
    require(litPixels(35, 115, 82, 150) > 30 && litPixels(85, 115, 150, 150) == 0,
            "the rider moved before throttle was applied");
    frontend.button = true;
    run(core, 90);
    frontend.button = false;
    require(litPixels(0, 165, 280, 175) > 2500 && litPixels(85, 115, 150, 150) > 30,
            "RetroPad B did not move the rider forward in the race");
    core.retro_unload_game();
    return titleFrame;
}

void testMedia(Core& core, const fs::path& work, const fs::path& firmware,
               const fs::path& roms, const fs::path& sourceDisk)
{
    require(frontend.fastDiskEnabledByDefault, "frontend did not register enabled as the default disk loading option");
    for (const char* name : {"apple2e.rom", "apple2e_char.rom"}) {
        fs::copy_file(roms / name, firmware / name);
    }
    const auto disk = work / "DIRTBIKE.dsk";
    fs::copy_file(sourceDisk, disk);
    const auto original = readFile(disk);
    require(core.retro_load_game(nullptr), "could not boot Apple IIe BASIC without content");
    run(core, 120);
    const auto basic = frontend.pixels;
    require(frontend.driveReadSignals == 0, "BASIC lit the disk LED without a disk");
    constexpr std::array<uint8_t, 8> basicPrompt{0x3e, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3e, 0x00};
    require(glyphMask(0, 2) == basicPrompt, "no-content boot did not reach the Applesoft ] prompt");
    core.retro_unload_game();
    const auto originalFrames = testDirtBike(core, disk, false);
    const auto fastFrames = testDirtBike(core, disk, true);
    require(fastFrames <= originalFrames / 2, "fast disk loading should at least halve the graphics title wait");
    require(readFile(disk) == original, "loading and running DIRTBIKE modified the supplied image");
    require(core.retro_load_game(nullptr), "could not reload BASIC after unloading a disk");
    run(core, 120);
    require(frontend.pixels == basic, "BASIC reload did not reproduce the no-content display");
    core.retro_unload_game();
}

void typeCommand(Core& core, const char* text)
{
    for (; *text; ++text) {
        unsigned key = static_cast<unsigned char>(*text);
        if (*text >= 'A' && *text <= 'Z') key += 'a' - 'A';
        frontend.keyboard(true, key, static_cast<unsigned char>(*text), 0);
        run(core, 2);
        frontend.keyboard(false, key, 0, 0);
        run(core, 2);
    }
}

void waitForDos(Core& core)
{
    constexpr std::array<uint8_t, 8> prompt{0x3e, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3e, 0x00};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        run(core, 60);
        for (unsigned row = 0; row < 24; ++row)
            if (glyphMask(0, row) == prompt) return;
    }
    require(false, "DOS 3.3 did not reach the BASIC prompt");
}

std::string visibleText(const Bytes& characters)
{
    std::string result;
    for (unsigned row = 0; row < 24; ++row) {
        for (unsigned col = 0; col < 40; ++col) {
            const auto glyph = glyphMask(col, row);
            char out = '?';
            for (unsigned c = 0x20; c <= 0x7e; ++c) {
                bool matches = true;
                for (unsigned y = 0; y < 8; ++y)
                    matches &= glyph[y] == ((characters.at((c | 0x80) * 8 + y) ^ 0x7fu) & 0x7fu);
                if (matches) { out = static_cast<char>(c); break; }
            }
            result += out;
        }
        result += '\n';
    }
    return result;
}

const retro_core_option_v2_definition* findDefinition(const retro_core_options_v2* options, const char* key)
{
    for (auto* definition = options->definitions; definition->key; ++definition)
        if (std::strcmp(definition->key, key) == 0) return definition;
    return nullptr;
}

void testCoreOptionsV2(Core& core)
{
    // A v2 frontend gets categories and every option filed under one, the
    // first value its default; a French frontend gets the translation too.
    frontend.optionsVersion = 2;
    frontend.language = RETRO_LANGUAGE_FRENCH;
    core.retro_set_environment(environment);
    require(frontend.optionsUs && frontend.optionsLocal, "core options v2 were not registered with a translation");
    unsigned categories = 0, definitions = 0;
    for (auto* category = frontend.optionsUs->categories; category->key; ++category) ++categories;
    for (auto* definition = frontend.optionsUs->definitions; definition->key; ++definition, ++definitions) {
        require(definition->category_key && definition->values[0].value && definition->default_value &&
                std::strcmp(definition->default_value, definition->values[0].value) == 0,
                "a v2 option lacks a category or does not default to its first value");
    }
    require(categories >= 5 && definitions >= 20, "the core registered fewer option categories or options than expected");
    for (const char* key : {"pom2_machine", "pom2_display", "pom2_mockingboard", "pom2_cpu", fastDiskOption}) {
        const auto* english = findDefinition(frontend.optionsUs, key);
        const auto* french = findDefinition(frontend.optionsLocal, key);
        require(english && french, "an expected core option is missing");
        if (std::strcmp(key, fastDiskOption) != 0)
            require(std::strcmp(english->desc, french->desc) != 0, "an option was not translated into French");
    }
    require(frontend.achievements, "the core did not declare achievement support");
    frontend.optionsVersion = 0;
    frontend.language = RETRO_LANGUAGE_ENGLISH;
    frontend.optionsUs = frontend.optionsLocal = nullptr;
    core.retro_set_environment(environment);
}

void testFeatures(Core& core, const fs::path& firmware)
{
    testCoreOptionsV2(core);

    // A machine whose firmware is absent names the file it needs.
    frontend.messages.clear();
    frontend.options["pom2_machine"] = "iiplus";
    require(!core.retro_load_game(nullptr) && frontend.messages.find("apple2p.rom") != std::string::npos,
            "a missing ][+ firmware was not named");
    frontend.options.erase("pom2_machine");

    frontend.options["pom2_display"] = "mono-green";
    require(core.retro_load_game(nullptr), "could not load with a monochrome display");
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    run(core, 4);
    unsigned green = 0, other = 0;
    for (auto pixel : frontend.pixels) {
        const unsigned r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
        if (!r && !g && !b) continue;
        ++(g > r && g > b ? green : other);
    }
    require(green > 0 && other == 0, "the green monitor painted a pixel that is not green");
    frontend.options["pom2_display"] = "composite";
    frontend.optionsUpdated = true;
    run(core, 2);
    const auto seeded = cell(0);   // the firmware's 'A' at $0400

    // The memory map: RetroAchievements' Apple II layout, main RAM then aux.
    require(frontend.memoryMap.size() == 8, "a //e must map main, aux and both language cards");
    const auto descriptor = [](uint64_t start) -> const retro_memory_descriptor& {
        for (const auto& d : frontend.memoryMap) if (d.start == start) return d;
        throw std::runtime_error("the memory map lacks an expected region");
    };
    require(descriptor(0x00000).len == 0xC000 && descriptor(0x0C000).len == 0x1000 &&
            descriptor(0x0D000).len == 0x1000 && descriptor(0x0E000).len == 0x2000 &&
            descriptor(0x10000).len == 0xC000 && descriptor(0x1E000).len == 0x2000,
            "the memory map regions have the wrong sizes");
    auto* main = static_cast<uint8_t*>(core.retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    require(main == descriptor(0).ptr && core.retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) == 0xC000,
            "system RAM is not the map's main region");
    main[0x400] = 0xC5;
    run(core, 2);
    require(cell(0) != seeded, "a write through the memory map did not reach the screen");

    // Cheats share the map's addresses: main, compare, aux and language card.
    core.retro_cheat_set(0, true, "$0400:C1");
    run(core, 2);
    require(cell(0) == seeded && main[0x400] == 0xC1, "a RAM cheat did not apply");
    core.retro_cheat_reset();
    main[0x400] = 0xC5;
    run(core, 2);
    require(main[0x400] == 0xC5, "a reset cheat kept applying");
    core.retro_cheat_set(1, true, "0400?C1:C2");
    run(core, 2);
    require(main[0x400] == 0xC5, "a compare cheat applied to a cell that did not match");
    core.retro_cheat_set(2, true, "10400:AA+0D000:12;1C000=34");
    core.retro_cheat_set(3, true, "not a cheat");
    run(core, 1);
    require(static_cast<uint8_t*>(descriptor(0x10000).ptr)[0x400] == 0xAA &&
            static_cast<uint8_t*>(descriptor(0x0D000).ptr)[0] == 0x12 &&
            static_cast<uint8_t*>(descriptor(0x1C000).ptr)[0] == 0x34,
            "a multi-code cheat missed aux RAM or a language-card bank");
    core.retro_cheat_reset();

    // The RetroPad as a keyboard: arrows, then I J K M, with repeat.
    core.retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0));
    frontend.pad = 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    run(core, 2);
    require(main[0x400] == 0x95, "the arrow-key RetroPad did not type Right");
    main[0x400] = 0;
    run(core, 40);
    require(main[0x400] == 0x95, "a held RetroPad direction did not repeat");
    frontend.pad = 1u << RETRO_DEVICE_ID_JOYPAD_B;
    run(core, 2);
    require(main[0x400] == 0xA0 && main[0x401] == 0xC2, "keyboard B must type Space, not press button 0");
    frontend.pad = 0;
    core.retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));
    frontend.pad = 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    run(core, 2);
    require(main[0x400] == 0xC9, "the I J K M RetroPad did not type I");
    frontend.pad = 0;
    run(core, 1);
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    // Joystick buttons swap; the keyboard folds to uppercase.
    frontend.options["pom2_swap_buttons"] = "enabled";
    frontend.options["pom2_uppercase"] = "enabled";
    frontend.optionsUpdated = true;
    frontend.pad = 1u << RETRO_DEVICE_ID_JOYPAD_A;
    run(core, 2);
    require(main[0x401] == 0xC3, "with swapped buttons A did not press button 0");
    frontend.pad = 0;
    frontend.keyboard(true, RETROK_b, 'b', 0);
    frontend.keyboard(false, RETROK_b, 'b', 0);
    run(core, 2);
    require(main[0x400] == 0xC2, "the uppercase option let a lowercase key through");
    const auto state = saveState(core);
    require(core.retro_unserialize(state.data(), state.size()), "a state with the new header did not restore");
    core.retro_unload_game();
    for (const char* key : {"pom2_display", "pom2_swap_buttons", "pom2_uppercase"}) frontend.options.erase(key);
    (void)firmware;
}

// Two 5.25-inch drives: the playlist's second face preloaded in drive 2,
// Disc Control moved to drive 2, a state carrying both writable drives, and a
// private copy for each face.
void connectFrontend(Core& core);

void testSecondDrive(Core& core, const fs::path& work, const fs::path& master)
{
    // The persistence test opens a second instance; take the disk callbacks
    // back from this one.
    core.retro_deinit();
    connectFrontend(core);
    const auto folder = work / "two-drives";
    fs::create_directories(folder);
    const Bytes original = readFile(master);
    writeFile(folder / "Boot.dsk", original);
    writeFile(folder / "Data.dsk", original);
    const std::string playlistPath = (folder / "Two drives.m3u").string();
    const std::string playlist = "Boot.dsk|Boot\nData.dsk|Data\n";
    writeFile(playlistPath, Bytes(playlist.begin(), playlist.end()));
    const auto saves = work / "two-drive-saves";
    fs::create_directories(saves);
    frontend.saveDirectoryOverride = saves.string();
    frontend.options["pom2_drive2"] = "next";
    const auto game = contentInfo(playlistPath);
    require(core.retro_load_game(&game), "could not load a two-drive playlist");
    core.retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    const auto& disk = frontend.diskControlExt;
    require(disk.get_image_index() == 0 && !disk.get_eject_state(), "drive 1 does not hold the boot disk");
    frontend.options["pom2_disk_control_drive"] = "2";
    frontend.optionsUpdated = true;
    run(core, 1);
    require(disk.get_image_index() == 1 && !disk.get_eject_state(),
            "Disc Control on drive 2 does not see the preloaded second disk");
    run(core, 120);
    const auto state = saveState(core);
    run(core, 30);
    require(core.retro_unserialize(state.data(), state.size()),
            "a state holding two writable drives did not restore");
    require(disk.get_image_index() == 1 && !disk.get_eject_state(), "the restore lost drive 2");
    require(disk.set_eject_state(true) && disk.get_eject_state() && disk.get_image_index() == 1,
            "drive 2 did not eject");
    require(disk.set_image_index(1) && disk.set_eject_state(false) && !disk.get_eject_state(),
            "drive 2 did not take its disk back");
    frontend.options["pom2_disk_control_drive"] = "1";
    frontend.optionsUpdated = true;
    run(core, 1);
    require(disk.get_image_index() == 0, "Disc Control did not return to drive 1");
    core.retro_unload_game();
    unsigned copies = 0;
    for (const auto& entry : fs::recursive_directory_iterator(saves))
        copies += entry.is_regular_file() && entry.path().extension() == ".dsk";
    require(copies == 2, "each drive's disk needs its own private copy");
    frontend.options.erase("pom2_drive2");
    frontend.options.erase("pom2_disk_control_drive");
    frontend.saveDirectoryOverride.clear();
}

void connectFrontend(Core& core)
{
    core.retro_set_environment(environment);
    core.retro_set_video_refresh(video);
    core.retro_set_audio_sample(audioSample);
    core.retro_set_audio_sample_batch(audioBatch);
    core.retro_set_input_poll(inputPoll);
    core.retro_set_input_state(inputState);
    core.retro_init();
}

void testPersistentDisks(Core& core, const fs::path& work, const fs::path& firmware,
                         const fs::path& roms, const fs::path& master, const fs::path& modulePath)
{
    for (const char* name : {"apple2e.rom", "apple2e_char.rom", "disk2.rom", "diskii_p6.rom"})
        fs::copy_file(roms / name, firmware / name);
    const auto sideA = work / "Face A.dsk";
    const auto sideB = work / "Face B.dsk";
    const Bytes original = readFile(master);
    writeFile(sideA, original);
    writeFile(sideB, original);
    const std::string playlistPath = (work / "Two sides.m3u").string();
    const std::string playlist = "Face A.dsk\nFace B.dsk\n";
    writeFile(playlistPath, Bytes(playlist.begin(), playlist.end()));
    const auto game = contentInfo(playlistPath);
    const auto& disk = frontend.diskControlExt;
    require(core.retro_load_game(&game) && disk.get_image_index() == 0,
            "could not boot DOS 3.3 from face A");
    waitForDos(core);
    run(core, 600);
    const auto cleanA = saveState(core);
    require(disk.set_eject_state(true) && disk.set_image_index(1) && disk.set_eject_state(false),
            "could not insert face B for the guest's SAVE");
    const auto beforeSave = saveState(core);
    typeCommand(core, "10 PRINT 7315\rSAVE POM2FACETEST\r");
    run(core, 160);
    const auto afterSave = saveState(core);
    const auto characters = readFile(firmware / "apple2e_char.rom");
    const auto saveScreen = visibleText(characters);
    const auto storage = work / "pom2" / "disks";
    for (const auto& entry : fs::recursive_directory_iterator(storage))
        if (entry.path().extension() == ".dsk")
            require(readFile(entry.path()) == original, "the guest SAVE was already flushed before unloading");
    require(core.retro_unserialize(cleanA.data(), cleanA.size()) &&
                !disk.get_eject_state() && disk.get_image_index() == 0,
            "could not restore face A while the guest had unsaved writes on face B");
    unsigned copiesWithChanges = 0;
    for (const auto& entry : fs::recursive_directory_iterator(storage))
        if (entry.path().extension() == ".dsk" && readFile(entry.path()) != original) ++copiesWithChanges;
    require(copiesWithChanges == 1, "restoring face A discarded or contaminated the writes to face B");
    require(frontend.executionPhaseProbes == 1 && frontend.executionPhaseQueries == 0,
            "an unsupported execution-phase command was queried after probing");
    require(disk.set_eject_state(true) && disk.set_image_index(1) && disk.set_eject_state(false),
            "could not reinsert face B after restoring A");
    typeCommand(core, "LOAD POM2FACETEST\r");
    run(core, 360);
    typeCommand(core, "RUN\r");
    run(core, 60);
    require(visibleText(characters).find("\n7315 ") != std::string::npos,
            "reinserting face B after restoring A did not recover its writes");
    require(core.retro_unserialize(beforeSave.data(), beforeSave.size()) &&
                !disk.get_eject_state() && disk.get_image_index() == 1,
            "restore did not reselect the clean face B checkpoint");
    require(disk.set_eject_state(true), "rewound face B could not be ejected");
    for (const auto& entry : fs::recursive_directory_iterator(storage))
        if (entry.path().extension() == ".dsk")
            require(readFile(entry.path()) == original, "eject after rewind left the later bytes in the working copy");
    require(core.retro_unserialize(afterSave.data(), afterSave.size()) &&
                !disk.get_eject_state() && disk.get_image_index() == 1,
            "restore did not recover the modified face B checkpoint");
    core.retro_unload_game();

    require(readFile(sideA) == original && readFile(sideB) == original,
            "guest SAVE changed an original disk image");
    unsigned unchanged = 0;
    unsigned modified = 0;
    for (const auto& entry : fs::recursive_directory_iterator(storage)) {
        if (entry.path().extension() != ".dsk") continue;
        if (readFile(entry.path()) == original) ++unchanged;
        else ++modified;
    }
    if (unchanged != 1 || modified != 1)
        throw std::runtime_error("the guest SAVE did not update just the private copy of face B\n" + saveScreen);

    core.retro_deinit();
    core.retro_init();
    require(disk.set_initial_image(1, sideB.string().c_str()) && core.retro_load_game(&game)
                && disk.get_image_index() == 1,
            "could not relaunch and boot the modified face B");
    waitForDos(core);
    run(core, 600);
    typeCommand(core, "LOAD POM2FACETEST\r");
    run(core, 360);
    typeCommand(core, "RUN\r");
    run(core, 60);
    require(visibleText(characters).find("\n7315 ") != std::string::npos,
            "the guest did not load and run its saved program after relaunch");
    core.retro_unload_game();

    // Disk II detects the 2IMG envelope from its magic. Keep a .dsk suffix
    // and set the format's own lock bit so the backing copy must honor it.
    Bytes locked(64, 0);
    std::memcpy(locked.data(), "2IMGPOM2", 8);
    locked[8] = 64;
    locked[10] = 1;
    locked[19] = 0x80;
    locked[24] = 64;
    for (std::size_t i = 0; i < 4; ++i)
        locked[28 + i] = static_cast<uint8_t>(original.size() >> (i * 8));
    locked.insert(locked.end(), original.begin(), original.end());
    const std::string lockedPath = (work / "Locked.dsk").string();
    writeFile(lockedPath, locked);
    const auto lockedGame = contentInfo(lockedPath);
    require(core.retro_load_game(&lockedGame), "the intrinsically protected disk did not mount");
    waitForDos(core);
    run(core, 600);
    typeCommand(core, "10 PRINT 43\rSAVE LOCKTEST\r");
    run(core, 160);
    require(visibleText(characters).find("WRITE PROTECTED") != std::string::npos,
            "DOS could not see the original format's write-protect flag through the backing copy");
    core.retro_unload_game();
    require(readFile(lockedPath) == locked, "the guest changed a locked original");
    unsigned lockedCopies = 0;
    for (const auto& entry : fs::recursive_directory_iterator(storage))
        if (entry.path().extension() == ".dsk" && readFile(entry.path()) == locked) ++lockedCopies;
    require(lockedCopies == 1, "the private copy of a locked disk was modified");

    // Load an independent copy of the module to obtain separate core globals.
    // The shared backing is protected by the OS lock, including inside one process.
    const auto competingPath = work / ("competing-core" + modulePath.extension().string());
    fs::copy_file(modulePath, competingPath);
    require(core.retro_load_game(&game), "could not mount the first instance's writable face");
    Core competing{Module(competingPath)};
    connectFrontend(competing);
    require(competing.retro_load_game(&game), "could not mount the second instance read-only");
    waitForDos(competing);
    run(competing, 600);
    typeCommand(competing, "10 PRINT 79\rSAVE LOCKTEST\r");
    run(competing, 160);
    require(visibleText(characters).find("WRITE PROTECTED") != std::string::npos,
            "two independent instances could write the same backing");
    competing.retro_unload_game();
    core.retro_unload_game();
    require(competing.retro_load_game(&game), "could not reopen the now-unlocked face");
    waitForDos(competing);
    run(competing, 600);
    typeCommand(competing, "10 PRINT 79\rSAVE LOCKTEST\r");
    run(competing, 160);
    require(visibleText(characters).find("WRITE PROTECTED") == std::string::npos,
            "closing the first instance did not release the write lease");
    competing.retro_deinit();

    const auto occupiedSaveRoot = work / "not-a-directory";
    writeFile(occupiedSaveRoot, Bytes{0x41});
    frontend.saveDirectoryOverride = occupiedSaveRoot.string();
    require(core.retro_load_game(&game), "unusable save directory prevented the original from loading");
    waitForDos(core);
    run(core, 600);
    typeCommand(core, "10 PRINT 84\rSAVE LOCKTEST\r");
    run(core, 160);
    require(visibleText(characters).find("WRITE PROTECTED") != std::string::npos,
            "unusable save directory did not protect the original");
    core.retro_unload_game();
    require(readFile(sideA) == original && readFile(sideB) == original &&
                readFile(occupiedSaveRoot) == Bytes{0x41},
            "concurrent or fallback mounts changed source files");
    frontend.saveDirectoryOverride.clear();

    const std::string readOnlyPath = (work / "Read-only.dsk").string();
    writeFile(readOnlyPath, original);
    const auto originalMode = fs::status(readOnlyPath).permissions();
    const auto writable = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    fs::permissions(readOnlyPath, writable, fs::perm_options::remove);
    const auto readOnlyGame = contentInfo(readOnlyPath);
    const bool loaded = core.retro_load_game(&readOnlyGame);
    if (loaded) {
        waitForDos(core);
        run(core, 600);
        typeCommand(core, "10 PRINT 87\rSAVE READONLYTEST\r");
        run(core, 160);
        core.retro_unload_game();
    }
    fs::permissions(readOnlyPath, originalMode);
    require(loaded && visibleText(characters).find("WRITE PROTECTED") != std::string::npos &&
                readFile(readOnlyPath) == original,
            "a file made read-only on the host accepted guest writes");
}
} // namespace

int main(int argc, char* argv[])
{
    try {
        const bool persistence = argc == 6 && std::strcmp(argv[3], "--disk-persistence") == 0;
        require(argc == 3 || argc == 5 || persistence,
                "usage: test_libretro_module MODULE WORKDIR [[--disk-persistence] ROMS DISK]");
        if (argc >= 5 && (!fs::exists(fs::path(argv[argc - 2]) / "apple2e.rom") ||
                          !fs::exists(fs::path(argv[argc - 2]) / "apple2e_char.rom") || !fs::exists(argv[argc - 1]))) {
            std::puts("SKIP: Apple IIe firmware or the disk test media is missing");
            return 77;
        }
        if (persistence && (!fs::exists(fs::path(argv[4]) / "disk2.rom") ||
                            !fs::exists(fs::path(argv[4]) / "diskii_p6.rom"))) {
            std::puts("SKIP: Disk II firmware is missing");
            return 77;
        }
        // The CTest suite protects its shared media. This test writes only
        // disposable working copies and must exercise the product default.
        if (persistence) {
#ifdef _WIN32
            _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
#else
            unsetenv("POM2_MEDIA_WRITE_DEFAULT");
#endif
        }
        const ScratchDirectory scratch(argv[2]);
        const auto& work = scratch.path;
        const fs::path firmware = work / "pom2";
        fs::create_directories(firmware);
        frontend.directory = work.string();
        frontend.supportsDriveLed = argc == 5;

        Core core{Module(argv[1])};
        require(core.retro_api_version() == RETRO_API_VERSION, "wrong libretro ABI version");
        retro_system_info info{};
        core.retro_get_system_info(&info);
        require(info.library_name && std::strstr(info.library_name, "POM2"), "module did not identify as POM2");
        connectFrontend(core);
        require(frontend.supportNoGame, "module did not advertise no-content boot");
        if (argc == 3) {
            testFixture(core, firmware);
            testFeatures(core, firmware);
            testDiskControl(core, work);
        } else if (persistence) {
            testPersistentDisks(core, work, firmware, argv[4], argv[5], argv[1]);
            testSecondDrive(core, work, argv[5]);
        } else {
            testMedia(core, work, firmware, argv[3], argv[4]);
        }
        core.retro_deinit();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n%s\n", error.what(), frontend.messages.c_str());
        return 1;
    }
    return 0;
}
