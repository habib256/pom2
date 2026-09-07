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

// Memory viewer write-path locking-contract test.
//
// Pins the 2026-08-02 whole-emulator freeze: MainWindow::renderMemoryViewerWindow
// called MemoryViewer_ImGui::render() while HOLDING EmulationController's
// stateMutex, and a byte edit (or Undo / Redo) reached the write callback
// from inside that render — a callback whose first statement re-locks the
// SAME mutex. EmulationController::stateMutex() is a plain NON-recursive
// std::mutex, so the same-thread re-lock is UB and hangs on glibc: the UI
// thread blocks forever holding the lock the CPU worker needs, and the
// emulator stops dead. Trigger was View → Memory viewer, double-click a
// byte, type two hex digits, Enter.
//
// The contract that replaced it (documented on MemoryViewer_ImGui::render()):
// render() only STAGES edits; the host drains them with flushPendingWrites()
// AFTER releasing the lock. This test drives a real headless Dear ImGui
// frame through a real double-click-edit and asserts both halves:
//
//   * the write callback is never entered while render() is on the stack
//     (checked by try_lock on the stand-in state mutex — a genuine re-lock
//     would fail it, and the pre-fix code would not even get that far);
//   * the staged byte does reach Memory, exactly once, after the flush.
//
// Headless ImGui: no backend, no GL. NewFrame/Render work as long as the
// atlas is built and DisplaySize/DeltaTime are set.

#include "MemoryViewer_ImGui.h"
#include "Memory.h"

#include "imgui.h"

#include <cassert>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace {

constexpr float kWinW = 900.0f;
constexpr float kWinH = 600.0f;

}  // namespace

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    {
        unsigned char* pixels = nullptr;
        int tw = 0, th = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &tw, &th);
        assert(tw > 0 && th > 0);
    }

    Memory memory;
    MemoryViewer_ImGui viewer(&memory);

    // Stand-in for EmulationController::stateMutex() — deliberately the same
    // kind: a plain, NON-recursive std::mutex.
    std::mutex stateMutex;

    int      writeCalls  = 0;
    uint16_t lastAddress = 0;
    uint8_t  lastValue   = 0;

    // Mirror of the sink MainWindow installs: it re-takes the state mutex so
    // the poke goes through Memory::memWrite like a CPU store would.
    viewer.setWriteCallback([&](uint16_t a, uint8_t v) -> uint8_t {
        const bool lockWasFree = stateMutex.try_lock();
        assert(lockWasFree && "write callback entered with stateMutex held — "
                              "this is the recursive-lock freeze");
        const uint8_t replaced = memory.peekCpuWriteTarget(a);
        memory.memWrite(a, v);
        stateMutex.unlock();
        ++writeCalls;
        lastAddress = a;
        lastValue   = v;
        return replaced;
    });

    // One UI frame, wired exactly like renderMemoryViewerWindow(): render
    // under the lock, flush outside it.
    bool sinkEnteredDuringRender = false;
    auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH));
        ImGui::Begin("Memory viewer", nullptr, ImGuiWindowFlags_NoSavedSettings);
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            const int before = writeCalls;
            viewer.render();
            if (writeCalls != before) sinkEnteredDuringRender = true;
        }
        const bool hovered = ImGui::IsAnyItemHovered();
        ImGui::End();
        ImGui::Render();
        viewer.flushPendingWrites();
        return hovered;
    };

    // Settle the layout (the clipper needs a frame to measure).
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    frame();
    frame();

    // ── Locate a hex cell ───────────────────────────────────────────────
    // The grid's geometry depends on the font metrics, so probe for it
    // instead of hard-coding: sweep the first few hex columns at a handful
    // of row heights until an item reports hover. The only hoverable items
    // in that band are the byte Selectables (the ASCII column is plain
    // text), so a hit is a cell.
    float cellX = -1.0f, cellY = -1.0f;
    for (float y = 380.0f; y <= 430.0f && cellY < 0.0f; y += 3.0f) {
        for (float x = 52.0f; x <= 130.0f; x += 2.0f) {
            io.AddMousePosEvent(x, y);
            if (frame()) { cellX = x; cellY = y; break; }
        }
    }
    assert(cellX >= 0.0f && "no hex cell found — grid layout changed?");

    // ── Double-click the cell, type two hex digits, press Enter ─────────
    auto editCell = [&](char hi, char lo) {
        // Let any previous click chain expire: ImGui keeps counting rapid
        // clicks at the same spot (3, 4, …) and IsMouseDoubleClicked() is
        // exactly "count == 2", so a second edit issued too soon would never
        // arm. io.MouseDoubleClickTime defaults to 0.30 s.
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        for (int i = 0; i < 25; ++i) frame();

        io.AddMousePosEvent(cellX, cellY);
        io.AddMouseButtonEvent(0, true);   frame();
        io.AddMouseButtonEvent(0, false);  frame();
        io.AddMouseButtonEvent(0, true);   frame();  // double click → edit arms
        io.AddMouseButtonEvent(0, false);  frame();  // InputText submitted
        frame();                                     // nav-move resolves: active
        // One character per frame: ImGui trickles its input queue, so both
        // digits in a single frame would lose one.
        io.AddInputCharacter(hi);  frame();
        io.AddInputCharacter(lo);  frame();
        io.AddKeyEvent(ImGuiKey_Enter, true);   frame();
        io.AddKeyEvent(ImGuiKey_Enter, false);  frame();
    };

    editCell('7', 'F');

    // The edit landed — through flushPendingWrites(), and nowhere else.
    assert(!sinkEnteredDuringRender);
    assert(writeCalls == 1);
    assert(lastValue == 0x7F);
    const uint16_t edited = lastAddress;
    assert(memory.data()[edited] == 0x7F);

    // A second edit on the same cell: the queue really is drained (a stale
    // entry would re-apply 0x7F) and the contract holds on every pass.
    editCell('0', 'A');
    assert(!sinkEnteredDuringRender);
    assert(writeCalls == 2);
    assert(lastAddress == edited);
    assert(lastValue == 0x0A);
    assert(memory.data()[edited] == 0x0A);

    // A flush with nothing staged is a no-op, not a replay.
    viewer.flushPendingWrites();
    assert(writeCalls == 2);

    ImGui::DestroyContext();
    std::printf("memory_viewer_write_lock_test: OK (%d writes applied)\n",
                writeCalls);
    return 0;
}
