// POM2 — GPL-3.0-or-later
#include "Debugger_ImGui.h"
#include "Assembler6502.h"
#include "EmulationController.h"
#include "Memory.h"
#include "M6502.h"
#include "imgui.h"

#include <charconv>
#include <cstdio>
#include <cstring>

namespace pom2 {
void Debugger_ImGui::drawAssembler(EmulationController& ctrl, const Snapshot& snap)
{
    if (!ImGui::CollapsingHeader("Assemble instruction")) return;
    ImGui::SetNextItemWidth(85);
    ImGui::InputText("Address (hex)", assemblyAddress_, sizeof(assemblyAddress_), ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Button("Use cursor / PC"))
        std::snprintf(assemblyAddress_, sizeof(assemblyAddress_), "%04X", cursorValid_ ? cursorAddr_ : snap.pc);
    ImGui::SetNextItemWidth(300);
    const bool enter = ImGui::InputText("Instruction", assemblySource_, sizeof(assemblySource_), ImGuiInputTextFlags_EnterReturnsTrue);
    unsigned address = 0;
    const auto parsed = std::from_chars(assemblyAddress_, assemblyAddress_ + std::strlen(assemblyAddress_), address, 16);
    const bool validAddress = parsed.ec == std::errc{} && *parsed.ptr == '\0' && address <= 0xFFFF;
    std::vector<uint8_t> bytes;
    std::string error;
    const bool valid = validAddress && assemble6502(assemblySource_, uint16_t(address), snap.cmos, bytes, error);
    if (valid) {
        ImGui::TextUnformatted("Bytes:"); ImGui::SameLine();
        for (uint8_t byte : bytes) { ImGui::Text("%02X", byte); ImGui::SameLine(); }
        ImGui::NewLine();
    } else ImGui::TextWrapped("%s", validAddress ? error.c_str() : "Enter a 16-bit hexadecimal address.");
    ImGui::TextWrapped("Stop the CPU to patch RAM below $C000 using its current write mapping. $0010 forces absolute addressing; ; starts a comment.");
    ImGui::BeginDisabled(snap.running || !valid || address + bytes.size() > 0xC000);
    if (ImGui::Button("Assemble and advance") || (enter && !snap.running && valid && address + bytes.size() <= 0xC000)) {
        auto state = ctrl.lockState();
        if (ctrl.getMode() == EmulationController::Mode::Stopped &&
            (state.cpu().effectiveCpuMode() == M6502::CpuMode::CMOS) == snap.cmos) {
            for (size_t i = 0; i < bytes.size(); ++i) state.memory().memWrite(uint16_t(address + i), bytes[i]);
            ctrl.noteTimeJump();
            ctrl.rewind().clear();
            followPc_ = false; viewAddr_ = uint16_t(address);
            assemblyStatus_ = "Instruction written.";
            std::snprintf(assemblyAddress_, sizeof(assemblyAddress_), "%04X", unsigned(address + bytes.size()));
        } else assemblyStatus_ = "Stop the CPU and try again.";
    }
    ImGui::EndDisabled();
    if (!assemblyStatus_.empty()) ImGui::TextUnformatted(assemblyStatus_.c_str());
}
}
