#include <pom2/core.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    // tools/package_core_sdk.sh writes its manifest from what the installed
    // archive says about itself, not from what the build tree believes.
    if (argc > 1 && std::strcmp(argv[1], "--sdk-info") == 0) {
        const pom2::SdkInfo info = pom2::Core::sdkInfo();
        std::printf("version=%s\ncommit=%s\n", info.version.c_str(), info.commit.c_str());
        for (const std::string& name : info.capabilities)
            std::printf("capability=%s\n", name.c_str());
        return 0;
    }

    pom2::Core core;
    core.setAudioSampleRate(44100);
    core.setJoystickAxes(0.0f, 0.0f);
    core.write(0x0300, 0x42);
    core.hardReset();
    core.run(32);

    const pom2::CpuState cpu = core.cpuState();
    const pom2::FramebufferView frame = core.renderFrame();
    std::array<float, 512> stereo{};
    const std::size_t audioFrames = core.pullAudio(
        stereo.data(), stereo.size() / 2);
    std::printf("PC=%04X cycles=%llu RAM[0300]=%02X frame=%dx%d audio=%zu\n",
                static_cast<unsigned>(cpu.programCounter),
                static_cast<unsigned long long>(cpu.cycles),
                static_cast<unsigned>(core.read(0x0300)),
                frame.width, frame.height, audioFrames);
    return 0;
}
