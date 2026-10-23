// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "DebugCoordinator.h"

#include "Debugger.h"

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryViewer_ImGui.h"

#include "imgui.h"

namespace pom2 {

DebugCoordinator::DebugCoordinator(EmulationController& controller)
    : controller_(controller),
      memoryViewer_(std::make_unique<MemoryViewer_ImGui>(&controller.memory()))
{
    // All edits pass through Memory::memWrite under the same state boundary
    // as CPU stores; the viewer itself never receives an unsafe raw writer.
    // Returns the byte that was replaced, sampled at the WRITE target under
    // the same lock as the store — the viewer builds its undo record from it.
    // Reading it from the hex grid instead was wrong whenever the read and
    // write banks differ (RAMRD vs RAMWRT under 80STORE).
    memoryViewer_->setWriteCallback([this](uint16_t address,
                                           uint8_t value) -> uint8_t {
        auto state = controller_.lockState();
        const uint8_t replaced = state.memory().peekCpuWriteTarget(address);
        // A watch on the edited address would latch this as a CPU write,
        // naming whatever instruction ran last — and stop a running machine.
        // The user's own edit is not a stop (bug hunt 2026-10-23); a hit
        // that was already pending is left alone.
        pom2::Debugger& dbg = controller_.debugger();
        const bool hadHit = dbg.stopRequested();
        state.memory().memWrite(address, value);
        if (!hadHit && dbg.stopRequested()) dbg.clearHit();
        return replaced;
    });
}

DebugCoordinator::~DebugCoordinator() = default;

MemoryViewer_ImGui& DebugCoordinator::memoryViewer() noexcept
{
    return *memoryViewer_;
}

const MemoryViewer_ImGui& DebugCoordinator::memoryViewer() const noexcept
{
    return *memoryViewer_;
}

void DebugCoordinator::renderMemoryViewer(bool& open)
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &open)) {
        {
            auto state = controller_.lockState();
            memoryViewer_->setCmosMode(
                state.cpu().effectiveCpuMode() == M6502::CpuMode::CMOS);
            memoryViewer_->render();
        }
        // The write callback re-enters the non-recursive state lock, so the
        // staged edits must be drained only after the read snapshot unlocks.
        memoryViewer_->flushPendingWrites();
    }
    ImGui::End();
}

} // namespace pom2
