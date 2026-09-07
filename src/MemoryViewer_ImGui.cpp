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

#include "MemoryViewer_ImGui.h"
#include "Disassembler6502.h"
#include "Memory.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

MemoryViewer_ImGui::MemoryViewer_ImGui(Memory* mem)
    : memory(mem)
{
    prevMemory.assign(0x10000, 0);
    changeFrame.assign(0x10000, 0);
    view_.assign(0x10000, 0);
}

const uint8_t* MemoryViewer_ImGui::memoryPointer() const
{
    return memory ? view_.data() : nullptr;
}

uint8_t MemoryViewer_ImGui::readByte(int address) const
{
    const uint8_t* p = memoryPointer();
    return p ? p[address & 0xFFFF] : 0;
}

char MemoryViewer_ImGui::printable(uint8_t v)
{
    // ASCII column: strip the high bit the firmware uses for video mode so
    // the column shows the glyph the user actually sees on screen.
    uint8_t a = v & 0x7F;
    return (a >= 0x20 && a < 0x7F) ? static_cast<char>(a) : '.';
}

void MemoryViewer_ImGui::detectChanges()
{
    const uint8_t* mem = memoryPointer();
    if (!mem) return;
    ++frameCounter;
    for (int i = 0; i < 0x10000; ++i) {
        if (mem[i] != prevMemory[i]) {
            changeFrame[i] = frameCounter;
            prevMemory[i]  = mem[i];
        }
    }
}

// ─── Apple II memory regions ─────────────────────────────────────────────

const char* MemoryViewer_ImGui::regionName(int a) const
{
    if (a < 0x0100)  return "Zero page";
    if (a < 0x0200)  return "Stack";
    if (a < 0x0400)  return "User RAM (low)";
    if (a < 0x0800)  return "Text page 1";
    if (a < 0x0C00)  return "Text page 2";
    if (a < 0x2000)  return "User RAM";
    if (a < 0x4000)  return "HGR page 1";
    if (a < 0x6000)  return "HGR page 2";
    if (a < 0xC000)  return "User RAM (high)";
    if (a < 0xC080)  return "Soft switches / I/O";
    if (a < 0xC100)  return "I/O reserved";
    if (a < 0xC800)  return "Slot ROMs";
    if (a < 0xD000)  return "Expansion ROM";
    if (a < 0xF800)  return "Applesoft ROM";
    return                  "Monitor ROM + vectors";
}

ImVec4 MemoryViewer_ImGui::regionColour(int a) const
{
    // Muted hues so byte text stays readable on a dark background.
    if (a < 0x0100)  return ImVec4(0.95f, 0.85f, 0.50f, 1.0f);  // zp = sand
    if (a < 0x0200)  return ImVec4(1.00f, 0.65f, 0.55f, 1.0f);  // stack = coral
    if (a < 0x0400)  return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);  // RAM
    if (a < 0x0C00)  return ImVec4(0.65f, 1.00f, 0.75f, 1.0f);  // text = green
    if (a < 0x2000)  return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    if (a < 0x6000)  return ImVec4(0.55f, 0.85f, 1.00f, 1.0f);  // HGR = sky
    if (a < 0xC000)  return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    if (a < 0xC100)  return ImVec4(1.00f, 0.55f, 0.95f, 1.0f);  // I/O = pink
    if (a < 0xD000)  return ImVec4(0.75f, 0.65f, 1.00f, 1.0f);  // slots = lavender
    return                  ImVec4(1.00f, 0.85f, 0.45f, 1.0f);  // ROM = amber
}

// ─── Top-level render ────────────────────────────────────────────────────

void MemoryViewer_ImGui::render()
{
    // Refresh the paged view ONCE per frame, under the caller's state lock.
    // Everything below (grid, ASCII column, disasm, search, change tracking,
    // the undo early-out) then reads a single coherent picture of what the
    // CPU would fetch — not the flat main-bank mirror Memory::data() exposes.
    if (memory) {
        if (view_.size() != 0x10000) view_.assign(0x10000, 0);
        memory->snapshotCpuView(view_.data());
    }

    if (showChanges) detectChanges();

    handleNavigation();
    renderControls();
    ImGui::Separator();
    renderRegionBanner();

    if (showDisasm) renderDisasmView();
    else            renderHexView();

    if (showSearch) renderSearchDialog();
}

void MemoryViewer_ImGui::navigateToAddress(int address)
{
    jumpToAddress(address);
}

void MemoryViewer_ImGui::handleNavigation()
{
    // Page up / down move by displayRows × bytesPerRow. Arrow up/down move
    // by one row. Only fire when the viewer window is focused so editing
    // a byte doesn't hijack key events.
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    if (editAddress >= 0) return;  // editing — let InputText eat keys

    const int rowStep  = bytesPerRow;
    const int pageStep = bytesPerRow * displayRows;
    if      (ImGui::IsKeyPressed(ImGuiKey_PageDown)) jumpToAddress(startAddress + pageStep);
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))   jumpToAddress(startAddress - pageStep);
    else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) jumpToAddress(startAddress + rowStep);
    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   jumpToAddress(startAddress - rowStep);
    else if (ImGui::IsKeyPressed(ImGuiKey_Home))     jumpToAddress(0x0000);
    else if (ImGui::IsKeyPressed(ImGuiKey_End))      jumpToAddress(0x10000 - pageStep);
}

void MemoryViewer_ImGui::jumpToAddress(int address)
{
    if (address < 0)        address = 0;
    if (address > 0xFFFF)   address = 0xFFFF;
    // Snap to row boundary so the requested address lands on the first
    // displayed row instead of in the middle of one.
    startAddress = (address / bytesPerRow) * bytesPerRow;
}

// ─── Controls bar ────────────────────────────────────────────────────────

void MemoryViewer_ImGui::renderControls()
{
    static char addressBuffer[8] = "0000";
    auto parseHex = [](const char* buf, unsigned& out) -> bool {
        const char* end = buf + std::strlen(buf);
        auto [p, ec] = std::from_chars(buf, end, out, 16);
        return ec == std::errc{} && p == end;
    };

    ImGui::Text("Goto:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputText("##addr", addressBuffer, sizeof(addressBuffer),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase
            | ImGuiInputTextFlags_EnterReturnsTrue)) {
        unsigned a = 0;
        if (parseHex(addressBuffer, a)) jumpToAddress(static_cast<int>(a));
    }
    ImGui::SameLine();
    if (ImGui::Button("Go")) {
        unsigned a = 0;
        if (parseHex(addressBuffer, a)) jumpToAddress(static_cast<int>(a));
    }

    ImGui::SameLine();
    if (ImGui::Button("Search")) showSearch = !showSearch;

    ImGui::SameLine();
    ImGui::BeginDisabled(undoStack.empty());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redoStack.empty());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();

    // Display options on a second row.
    ImGui::SetNextItemWidth(110);
    ImGui::SliderInt("##bpr", &bytesPerRow, 8, 32, "%d bytes/row");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderInt("##rows", &displayRows, 16, 64, "%d rows");
    ImGui::SameLine(); ImGui::Checkbox("ASCII",    &showAscii);
    ImGui::SameLine(); ImGui::Checkbox("Colours",  &colorizeRegions);
    ImGui::SameLine(); ImGui::Checkbox("Changes",  &showChanges);
    ImGui::SameLine(); ImGui::Checkbox("Disasm",   &showDisasm);

    // Quick jumps to the Apple II's notable addresses.
    if (ImGui::SmallButton("$0000")) { jumpToAddress(0x0000); } ImGui::SameLine();
    if (ImGui::SmallButton("ZP"))    { jumpToAddress(0x0000); } ImGui::SameLine();
    if (ImGui::SmallButton("Stack")) { jumpToAddress(0x0100); } ImGui::SameLine();
    if (ImGui::SmallButton("Text1")) { jumpToAddress(0x0400); } ImGui::SameLine();
    if (ImGui::SmallButton("HGR1"))  { jumpToAddress(0x2000); } ImGui::SameLine();
    if (ImGui::SmallButton("I/O"))   { jumpToAddress(0xC000); } ImGui::SameLine();
    if (ImGui::SmallButton("BASIC")) { jumpToAddress(0xD000); } ImGui::SameLine();
    if (ImGui::SmallButton("MON"))   { jumpToAddress(0xF800); } ImGui::SameLine();
    if (ImGui::SmallButton("Vec"))   { jumpToAddress(0xFFFA); }

    // Bookmarks.
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Bookmark")) {
        if (std::find(bookmarks.begin(), bookmarks.end(), startAddress) == bookmarks.end()) {
            bookmarks.push_back(startAddress);
        }
    }
    for (size_t i = 0; i < bookmarks.size() && i < 6; ++i) {
        ImGui::SameLine();
        char label[32];
        std::snprintf(label, sizeof(label), "$%04X##bm%zu", bookmarks[i], i);
        if (ImGui::SmallButton(label)) jumpToAddress(bookmarks[i]);
    }
}

void MemoryViewer_ImGui::renderRegionBanner()
{
    const char* name = regionName(startAddress);
    ImVec4 col = regionColour(startAddress);
    ImGui::TextColored(col, "[$%04X-$%04X] %s",
                       startAddress,
                       std::min(startAddress + bytesPerRow * displayRows - 1, 0xFFFF),
                       name);

    // Clarify the Language Card overlay: the viewer reads the flat memory mirror
    // (Memory::data()), while the CPU bus may see Language Card RAM at $D000-$FFFF
    // depending on $C080-$C08F latch state.
    if (memory && startAddress >= 0xD000) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", memory->busStateSummary().c_str());
        ImGui::TextDisabled("Note: $D000-$FFFF view is the flat mirror; LC RAM may differ.");
    }
}

// ─── Hex view ────────────────────────────────────────────────────────────

void MemoryViewer_ImGui::renderHexView()
{
    ImGui::BeginChild("HexView", ImVec2(0, 0), true);

    const float addrW    = ImGui::CalcTextSize("$0000  ").x;
    const float cellW    = ImGui::CalcTextSize("FF").x + ImGui::GetStyle().ItemSpacing.x;
    const float hexX     = ImGui::GetCursorPosX() + addrW;

    // Column header.
    ImGui::Text("Addr");
    for (int i = 0; i < bytesPerRow; ++i) {
        ImGui::SameLine(hexX + i * cellW);
        ImGui::Text("%02X", i);
    }
    if (showAscii) {
        ImGui::SameLine(hexX + bytesPerRow * cellW + ImGui::GetStyle().ItemSpacing.x);
        ImGui::Text("ASCII");
    }
    ImGui::Separator();

    const int maxRows  = (0x10000 - startAddress + bytesPerRow - 1) / bytesPerRow;
    const int totalRows = std::max(0, std::min(displayRows, maxRows));

    ImGuiListClipper clipper;
    clipper.Begin(totalRows);
    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const int rowAddr = startAddress + row * bytesPerRow;

            ImGui::Text("$%04X", rowAddr);

            char asciiLine[33] = {0};
            int  asciiIdx = 0;

            for (int col = 0; col < bytesPerRow; ++col) {
                const int currentAddr = rowAddr + col;
                if (currentAddr > 0xFFFF) break;

                const uint8_t value = readByte(currentAddr);

                ImGui::SameLine(hexX + col * cellW);

                // Change-flash overlay.
                if (showChanges && frameCounter > 0) {
                    const uint32_t age = frameCounter - changeFrame[currentAddr];
                    if (age < kChangeFadeFrames) {
                        const float alpha = 1.0f - static_cast<float>(age) / kChangeFadeFrames;
                        const ImVec2 p = ImGui::GetCursorScreenPos();
                        const float h = ImGui::GetTextLineHeight();
                        const float w = cellW - ImGui::GetStyle().ItemSpacing.x;
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            p, ImVec2(p.x + w, p.y + h),
                            IM_COL32(255, 120, 40, static_cast<int>(alpha * 160)));
                    }
                }

                bool pushed = false;
                if (currentAddr == searchAddress) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                    pushed = true;
                } else if (colorizeRegions) {
                    ImGui::PushStyleColor(ImGuiCol_Text, regionColour(currentAddr));
                    pushed = true;
                }

                if (editAddress == currentAddr) {
                    ImGui::SetNextItemWidth(cellW - ImGui::GetStyle().ItemSpacing.x);
                    if (!editFocusSet) {
                        ImGui::SetKeyboardFocusHere();
                        editFocusSet = true;
                    }
                    char id[16];
                    std::snprintf(id, sizeof(id), "##e%04X", currentAddr);
                    const bool enter = ImGui::InputText(id, editBuffer, sizeof(editBuffer),
                        ImGuiInputTextFlags_CharsHexadecimal |
                        ImGuiInputTextFlags_CharsUppercase   |
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                    // "Focus went elsewhere → abandon the edit" must only be
                    // tested once the box has HELD focus. SetKeyboardFocusHere
                    // posts a nav-move request that ImGui resolves in
                    // EndFrame, so IsItemActive() is still false on the frame
                    // that asked for focus; cancelling on that made the editor
                    // close itself one frame after the double-click and no
                    // edit ever reached applyEdit().
                    if (ImGui::IsItemActive()) editWasActive = true;
                    if (enter) {
                        unsigned parsed = 0;
                        const char* b = editBuffer;
                        const char* e = editBuffer + std::strlen(editBuffer);
                        auto [p, ec] = std::from_chars(b, e, parsed, 16);
                        if (ec == std::errc{} && p == e && parsed <= 0xFF) {
                            applyEdit(static_cast<uint16_t>(currentAddr),
                                      static_cast<uint8_t>(parsed));
                        }
                        editAddress = -1;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                               (editWasActive && !ImGui::IsItemActive())) {
                        editAddress = -1;
                    }
                } else {
                    char hexStr[4];
                    std::snprintf(hexStr, sizeof(hexStr), "%02X", value);
                    // Unique ID per CELL, not per label: hundreds of cells
                    // share the same two-digit text (a zeroed page is all
                    // "00"), which collides ImGui's active-ID tracking and
                    // intermittently mis-routes double-click-to-edit. The
                    // edit InputText already keys itself "##e%04X".
                    ImGui::PushID(currentAddr);
                    if (ImGui::Selectable(hexStr, false,
                            ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(cellW - ImGui::GetStyle().ItemSpacing.x, 0))) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            editAddress = currentAddr;
                            std::snprintf(editBuffer, sizeof(editBuffer), "%02X", value);
                            editFocusSet  = false;
                            editWasActive = false;
                        }
                    }
                    ImGui::PopID();
                }

                if (pushed) ImGui::PopStyleColor();

                if (asciiIdx < 32) asciiLine[asciiIdx++] = printable(value);
            }

            if (showAscii) {
                ImGui::SameLine(hexX + bytesPerRow * cellW
                                + ImGui::GetStyle().ItemSpacing.x);
                asciiLine[asciiIdx] = '\0';
                ImGui::TextDisabled("%s", asciiLine);
            }
        }
    }
    ImGui::EndChild();
}

// ─── Disasm view ─────────────────────────────────────────────────────────

void MemoryViewer_ImGui::renderDisasmView()
{
    ImGui::BeginChild("DisasmView", ImVec2(0, 0), true);

    const uint8_t* mem = memoryPointer();
    if (!mem) {
        ImGui::TextDisabled("memory not attached");
        ImGui::EndChild();
        return;
    }

    int address = startAddress;
    for (int i = 0; i < displayRows; ++i) {
        if (address > 0xFFFF) break;
        int len = 1;
        const std::string mnem =
            pom2::disassemble6502(mem, static_cast<uint16_t>(address), len, cmosDisasm_);

        if (colorizeRegions)
            ImGui::PushStyleColor(ImGuiCol_Text, regionColour(address));

        // Address + raw bytes + mnemonic on one line.
        char raw[16] = {0};
        for (int b = 0; b < len; ++b) {
            char tmp[4];
            std::snprintf(tmp, sizeof(tmp), "%02X ", mem[(address + b) & 0xFFFF]);
            std::strncat(raw, tmp, sizeof(raw) - std::strlen(raw) - 1);
        }
        ImGui::Text("$%04X  %-9s %s", address, raw, mnem.c_str());

        if (colorizeRegions) ImGui::PopStyleColor();

        address += len;
    }

    ImGui::EndChild();
}

// ─── Search ──────────────────────────────────────────────────────────────

void MemoryViewer_ImGui::renderSearchDialog()
{
    ImGui::OpenPopup("Search memory");
    if (ImGui::BeginPopupModal("Search memory", &showSearch,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Checkbox("ASCII string", &searchAscii); ImGui::SameLine();
        ImGui::TextDisabled(searchAscii ? "(letters)" : "(hex bytes, e.g. A9 FF 48)");

        ImGui::SetNextItemWidth(360);
        ImGui::InputText("##search", searchBuffer, sizeof(searchBuffer));

        if (ImGui::Button("Find next")) {
            if (searchAscii) searchAsciiString();
            else             searchHexBytes();
            showSearch = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) showSearch = false;
        ImGui::EndPopup();
    }
}

void MemoryViewer_ImGui::searchHexBytes()
{
    // Parse "A9 FF 48" → bytes.
    std::vector<uint8_t> needle;
    const char* p = searchBuffer;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        unsigned v = 0;
        const char* start = p;
        while (*p && *p != ' ' && *p != ',') ++p;
        auto [end, ec] = std::from_chars(start, p, v, 16);
        if (ec != std::errc{} || v > 0xFF) return;
        needle.push_back(static_cast<uint8_t>(v));
    }
    if (needle.empty()) return;

    const uint8_t* mem = memoryPointer();
    if (!mem) return;

    const int from = (searchAddress >= 0 ? searchAddress + 1 : startAddress);
    for (int a = from; a + (int)needle.size() <= 0x10000; ++a) {
        if (std::memcmp(mem + a, needle.data(), needle.size()) == 0) {
            searchAddress = a;
            jumpToAddress(a);
            return;
        }
    }
    searchAddress = -1;  // not found — clear highlight
}

void MemoryViewer_ImGui::searchAsciiString()
{
    const size_t n = std::strlen(searchBuffer);
    if (n == 0) return;
    const uint8_t* mem = memoryPointer();
    if (!mem) return;
    const int from = (searchAddress >= 0 ? searchAddress + 1 : startAddress);
    for (int a = from; a + (int)n <= 0x10000; ++a) {
        bool match = true;
        for (size_t i = 0; i < n; ++i) {
            // Match against either the raw ASCII byte or the firmware's
            // video-mode form (high bit set). Lets a search for "APPLE"
            // hit both ROM strings and on-screen text without the user
            // caring which form is in memory.
            const uint8_t target = static_cast<uint8_t>(searchBuffer[i]);
            const uint8_t got    = mem[a + i];
            if (got != target && got != (target | 0x80)) {
                match = false; break;
            }
        }
        if (match) {
            searchAddress = a;
            jumpToAddress(a);
            return;
        }
    }
    searchAddress = -1;
}

// ─── Edit / undo / redo ──────────────────────────────────────────────────

// All three of these run from render(), i.e. under the host's state mutex,
// so they only STAGE the write. flushPendingWrites() performs it once the
// host has let the lock go — see the contract on MemoryViewer_ImGui::render().

void MemoryViewer_ImGui::applyEdit(uint16_t address, uint8_t newValue)
{
    if (!writeCallback) return;
    // The undo record is NOT built here. render() runs under the state lock
    // and the write only happens later, in flushPendingWrites(); the byte the
    // write replaces is read there, at the WRITE target (RAMWRT), instead of
    // here from the read view (RAMRD). Under 80STORE/RAMWRT those are
    // different banks, and an Undo built from the read view shoved a main-RAM
    // byte into aux.
    pendingWrites.push_back({address, newValue, /*record=*/true});
    redoStack.clear();
}

void MemoryViewer_ImGui::undo()
{
    if (undoStack.empty() || !writeCallback) return;
    EditRecord r = undoStack.back();
    undoStack.pop_back();
    pendingWrites.push_back({r.address, r.oldValue, /*record=*/false});
    redoStack.push_back(r);
}

void MemoryViewer_ImGui::redo()
{
    if (redoStack.empty() || !writeCallback) return;
    EditRecord r = redoStack.back();
    redoStack.pop_back();
    pendingWrites.push_back({r.address, r.newValue, /*record=*/false});
    undoStack.push_back(r);
}

void MemoryViewer_ImGui::flushPendingWrites()
{
    if (pendingWrites.empty()) return;
    // Move first: the callback re-enters the host, and a re-entrant call
    // back into the viewer must not see a half-drained queue.
    std::vector<PendingWrite> batch;
    batch.swap(pendingWrites);
    if (!writeCallback) return;
    for (const PendingWrite& w : batch) {
        const uint8_t replaced = writeCallback(w.address, w.value);
        if (!w.record) continue;
        if (replaced == w.value) continue;   // nothing actually changed
        undoStack.push_back({w.address, replaced, w.value});
        if (undoStack.size() > 256) undoStack.erase(undoStack.begin());
    }
}
