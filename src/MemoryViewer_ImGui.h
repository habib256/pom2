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

// Memory viewer / hex editor. Hex grid + ASCII column, region-coloured by
// Apple II zone (zero page, stack, text pages, HGR pages, I/O, ROM).
// Inline edit with undo/redo, search, change highlighting, and a togglable
// 6502 disassembly view sharing the same address cursor.

#ifndef POM2_MEMORY_VIEWER_IMGUI_H
#define POM2_MEMORY_VIEWER_IMGUI_H

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Memory;

class MemoryViewer_ImGui
{
public:
    explicit MemoryViewer_ImGui(Memory* memory);

    // Render the entire viewer window contents. Caller owns Begin()/End()
    // and decides where the window sits in the layout.
    //
    // LOCKING CONTRACT: the host calls render() with the emulator state
    // mutex HELD (the hex grid reads Memory::data() directly and would
    // otherwise tear against the CPU worker), then calls
    // flushPendingWrites() with the mutex RELEASED. Nothing in render()
    // may reach the write sink, because that sink re-takes the same
    // non-recursive std::mutex.
    void render();

    // Apply the byte edits queued by the last render() (inline edit, Undo,
    // Redo) through the write callback, in the order the user made them.
    //
    // MUST be called with the state mutex released — see render(). The
    // callback installed by MainWindow re-locks it to route the write
    // through Memory::memWrite, and re-locking a non-recursive std::mutex
    // on the same thread is undefined behaviour (it hangs the UI thread
    // while it still holds the lock the CPU worker needs, freezing the
    // whole emulator).
    void flushPendingWrites();

    // Programmatic navigation — used by future "go to PC" buttons or a
    // disassembly listing that wants to centre on a label.
    void navigateToAddress(int address);

    // Inclusive byte range currently visible in the hex grid. Used by the
    // memory-map widgets to draw a viewport overlay so the bar and the
    // viewer stay in sync visually.
    struct ViewportRange { int startAddress; int endAddress; };
    ViewportRange getViewportRange() const {
        const int span = bytesPerRow * displayRows;
        const int end  = std::min(0xFFFF, startAddress + span - 1);
        return { startAddress, end };
    }

    // Hook fired for each queued byte edit when flushPendingWrites() runs.
    // MainWindow plumbs this through EmulationController so the write goes
    // through Memory::memWrite under stateMutex (rather than scribbling on
    // the raw array) — hence it is NEVER invoked from render().
    //
    // It RETURNS the byte it replaced, read from the write target under the
    // same lock. The undo record is built from that, not from the hex grid:
    // the grid resolves aux through RAMRD while a write resolves it through
    // RAMWRT, so under 80STORE/RAMWRT an Undo taken from the grid pushed a
    // MAIN-RAM byte into aux.
    void setWriteCallback(std::function<uint8_t(uint16_t, uint8_t)> cb) {
        writeCallback = std::move(cb);
    }

    // Push the live CPU mode so the Disasm view decodes 65C02 opcodes
    // correctly (and doesn't desync on 3-byte BBR/BBS). Set each frame.
    void setCmosMode(bool on) { cmosDisasm_ = on; }

private:
    Memory* memory;
    std::function<uint8_t(uint16_t, uint8_t)> writeCallback;
    bool cmosDisasm_ = false;

    // Per-frame PAGED copy of what the CPU would fetch (Memory::
    // snapshotCpuView). The hex grid used to index Memory::data(), the flat
    // main-bank mirror, which shows main RAM while the machine executes out
    // of aux / a RamWorks bank / the language card.
    std::vector<uint8_t> view_;

    // Layout state.
    int  startAddress  = 0x0000;
    int  bytesPerRow   = 16;
    int  displayRows   = 32;
    bool showAscii     = true;
    bool showDisasm    = false;
    bool showChanges   = true;
    bool colorizeRegions = true;

    // Change-highlight tracking. Per-byte frame counter — bytes touched in
    // the last `kChangeFadeFrames` ticks render with an orange flash.
    std::vector<uint8_t>  prevMemory;
    std::vector<uint32_t> changeFrame;
    uint32_t              frameCounter = 0;
    static constexpr uint32_t kChangeFadeFrames = 45;  // ~0.75 s at 60 fps

    // Search.
    char searchBuffer[256] = {0};
    int  searchAddress     = -1;
    bool showSearch        = false;
    bool searchAscii       = false;

    // Inline edit. Double-click on a byte arms editAddress for one frame.
    int  editAddress  = -1;
    char editBuffer[4] = {0};
    bool editFocusSet  = false;
    // Set once the edit InputText has actually held ImGui's active id.
    // SetKeyboardFocusHere() posts a nav-move request that only resolves in
    // EndFrame, so the item is NOT active on the frame that requests focus —
    // testing "lost focus" before that made the box cancel itself on its
    // first frame and no byte edit ever committed.
    bool editWasActive = false;

    struct EditRecord { uint16_t address; uint8_t oldValue; uint8_t newValue; };
    std::vector<EditRecord> undoStack;
    std::vector<EditRecord> redoStack;

    // Writes staged by render() and drained by flushPendingWrites(). The
    // indirection exists purely for the locking contract documented on
    // render(): the sink re-enters the host's state mutex, which render()
    // is already inside. Ordered, so an edit followed by an Undo in the
    // same frame lands in that order.
    //
    // `record` distinguishes a fresh user edit (which must push an undo entry
    // built from the byte the write actually replaced) from an Undo/Redo
    // replay (which already knows both values).
    struct PendingWrite { uint16_t address; uint8_t value; bool record; };
    std::vector<PendingWrite> pendingWrites;

    // Bookmarks.
    std::vector<int> bookmarks;

    // Helpers.
    void renderControls();
    void renderRegionBanner();
    void renderHexView();
    void renderDisasmView();
    void renderSearchDialog();
    void handleNavigation();
    void detectChanges();

    void jumpToAddress(int address);
    void searchHexBytes();
    void searchAsciiString();
    void applyEdit(uint16_t address, uint8_t newValue);
    void undo();
    void redo();

    uint8_t        readByte(int address) const;
    const uint8_t* memoryPointer() const;
    static char    printable(uint8_t v);

    ImVec4      regionColour(int address) const;
    const char* regionName  (int address) const;
};

#endif // POM2_MEMORY_VIEWER_IMGUI_H
