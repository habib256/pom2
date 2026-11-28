// HgrPaintEditor_Monitor.cpp — the editor's page row, and its read-only
// page monitor.
//
// The monitor shows one video page as the machine holds it, live, while the
// program runs: HGR / GR / DHGR / DLGR like the editor, and TEXT too. It
// writes NOTHING — no byte, and no soft switch (the editor's own page
// buttons drive the machine's display so the screen follows the canvas; a
// monitor that did that would break the very double buffering it is there
// to watch). It can follow the page on screen, or the one behind it.
//
// Idea from French Touch's AppleWin fork (fenarinarsa, "page monitor",
// v1.29.15v100FT): a second view of the page a demo is drawing into while
// the other one is displayed.
//
// This file is portable (shared with POM1): everything machine-specific
// goes through IHgrPaintHost (liveVideo, the render* pipelines).

#include "HgrPaintEditor.h"
#include "PaintLayout.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace {

struct PageBtn { const char* label; int mode; bool p2; const char* tip; };

// Modes: 0 HGR, 1 GR, 2 DHGR, 3 DLGR, 4 TEXT (the monitor only).
const PageBtn kPages[10] = {
    { "HGR",   0, false, "HIRES page 1 ($2000)" },
    { "HGR2",  0, true,  "HIRES page 2 ($4000)" },
    { "GR",    1, false, "Lo-res GR page 1 ($0400)" },
    { "GR2",   1, true,  "Lo-res GR page 2 ($0800)" },
    { "DHGR",  2, false, "Double hi-res page 1 (aux+main $2000)" },
    { "DHGR2", 2, true,  "Double hi-res page 2 (aux+main $4000)" },
    { "DLGR",  3, false, "Double lo-res page 1 (aux+main $0400)" },
    { "DLGR2", 3, true,  "Double lo-res page 2 (aux+main $0800)" },
    { "TEXT",  4, false, "Text page 1 ($0400; 80 columns when 80COL is on)" },
    { "TEXT2", 4, true,  "Text page 2 ($0800)" },
};

const char* modeName(int mode, bool col80)
{
    switch (mode) {
    case 0:  return "HGR";
    case 1:  return "GR";
    case 2:  return "DHGR";
    case 3:  return "DLGR";
    default: return col80 ? "TEXT80" : "TEXT40";
    }
}

const char* const kFollow[] = { "Page picked", "Page on screen", "Page behind" };

// The follow combo, as wide as its longest choice so the preview is never cut.
float followComboWidth()
{
    float w = 0.0f;
    for (const char* f : kFollow) w = std::max(w, ImGui::CalcTextSize(f).x);
    return w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

} // namespace

void hgrpaint::HgrPaintEditor::renderPageButtons(layout::Flow& row)
{
    const bool dbl  = host && host->supportsDhgr();
    const bool text = monitor && host && host->supportsTextPages();
    auto shown = [&](const PageBtn& b) {
        return !((b.mode == 2 || b.mode == 3) && !dbl) && !(b.mode == 4 && !text);
    };
    // The pages are one group: together on a line when they fit.
    {
        float w = 0.0f;
        int n = 0;
        for (const PageBtn& b : kPages)
            if (shown(b)) { w += layout::buttonWidth(b.label); ++n; }
        row.keep(w + ImGui::GetStyle().ItemSpacing.x * static_cast<float>(n - 1));
    }
    const int  curMode = monitor ? monMode
                                 : dlgrMode ? 3 : dhgrMode ? 2 : grMode ? 1 : 0;
    const bool curP2 = monitor ? monPage2 : page2;
    for (const PageBtn& b : kPages) {
        if (!shown(b)) continue;
        row.next(layout::buttonWidth(b.label));
        const bool sel = (curMode == b.mode && curP2 == b.p2);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(58, 96, 150, 255));
        if (ImGui::Button(b.label)) {
            if (monitor) {
                // Picking a page stops following: the monitor stays on it.
                monMode = b.mode; monPage2 = b.p2; monFollow = 0;
            } else {
                switchPage(b.mode, b.p2);
                // Drive the live machine's soft switches to the picked page/mode
                // so the screen follows the editor. Done on every click (even
                // re-picking the current page) so it also re-asserts the display
                // if a program had left the machine in text/another page.
                if (host) {
                    if (b.mode == 2)      host->setDisplayModeDhgr(b.p2);
                    else if (b.mode == 3) host->setDisplayModeDlgr(b.p2);
                    else host->setDisplayMode(b.mode == 1, b.p2);
                }
            }
        }
        if (sel) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(monitor ? "%s \xE2\x80\x94 show it (stops following)"
                                      : "%s \xE2\x80\x94 switch page/mode (clears undo history)",
                              b.tip);
    }

    // The Monitor switch (and, on, what it follows) after the pages: one
    // group, so the switch never sits alone on a line of its own.
    {
        float w = layout::checkboxWidth("Monitor");
        if (monitor) w += ImGui::GetStyle().ItemSpacing.x + followComboWidth();
        row.keep(w);
    }
    row.next(layout::checkboxWidth("Monitor"));
    if (ImGui::Checkbox("Monitor", &monitor) && monitor) followLiveVideo();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Read-only page monitor: a video page as the machine holds it, live,\n"
            "without writing a byte or a soft switch. It can follow the page on\n"
            "screen, or the one behind it (a double-buffered program's next frame).");

    if (monitor) {
        const float w = followComboWidth();
        row.next(w);
        ImGui::SetNextItemWidth(w);
        ImGui::Combo("##monfollow", &monFollow, kFollow, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Page on screen: what the machine displays now.\n"
                              "Page behind: the same mode, the other page \xE2\x80\x94 where a\n"
                              "double-buffered program draws its next frame.");
    }
}

void hgrpaint::HgrPaintEditor::followLiveVideo()
{
    IHgrPaintHost::LiveVideo v;
    if (!host || !host->liveVideo(v)) {
        monScreen = "(this host does not report its video switches)";
        return;
    }
    // The rule, graded: hgrpaint::scannedVideoPage (HgrPaintModel.h).
    const VideoPage on = scannedVideoPage(v.text, v.hires, v.page2, v.col80,
                                          v.an3Off, v.store80, v.iie);
    const bool shownP2 = on.page2;
    const int  mode    = on.mode;
    monCol80   = v.iie && v.col80;
    monAltChar = v.altChar;

    char buf[128];
    std::snprintf(buf, sizeof buf, "On screen: %s page %d%s%s",
                  modeName(mode, monCol80), shownP2 ? 2 : 1,
                  (!v.text && v.mixed) ? " + 4 text lines" : "",
                  (v.iie && v.store80) ? " (80STORE: PAGE2 = aux)" : "");
    monScreen = buf;

    if (monFollow == 0) return;
    monMode  = mode;
    monPage2 = (monFollow == 1) ? shownP2 : !shownP2;
}

void hgrpaint::HgrPaintEditor::renderMonitor(const std::vector<uint8_t>& memory,
                                             const std::vector<uint8_t>* aux)
{
    followLiveVideo();
    {
        layout::Flow row;
        renderPageButtons(row);
        row.next(layout::checkboxWidth("Colour"));
        ImGui::Checkbox("Colour", &ntscColor);
    }
    // What the machine shows and what this view shows: wrapped, never cut.
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s  \xC2\xB7  showing %s page %d  \xC2\xB7  read-only", monScreen.c_str(),
                       modeName(monMode, monCol80), monPage2 ? 2 : 1);
    ImGui::PopStyleColor();
    ImGui::Separator();

    const bool lores = (monMode == 1 || monMode == 3 || monMode == 4);
    const bool dbl   = (monMode == 2 || monMode == 3);
    const bool wide  = dbl || (monMode == 4 && monCol80);
    const uint16_t base = lores ? (monPage2 ? 0x0800 : 0x0400)
                                : (monPage2 ? 0x4000 : 0x2000);
    const size_t bytes = lores ? 0x400u : static_cast<size_t>(kHiresSize);

    // [main 8 KB][aux 8 KB]: the render pipelines read the first 1 KB of a
    // lo-res half, and the whole 8 KB of a hi-res one.
    monBuf.assign(2u * kHiresSize, 0);
    const bool haveAux = aux && aux->size() >= base + bytes;
    if (memory.size() >= base + bytes)
        std::copy(memory.begin() + base, memory.begin() + base + bytes, monBuf.begin());
    if (haveAux)
        std::copy(aux->begin() + base, aux->begin() + base + bytes,
                  monBuf.begin() + kHiresSize);

    const int w = wide ? 2 * kHiresWidth : kHiresWidth;
    monRgba.assign(static_cast<size_t>(w) * kHiresHeight, 0xFF000000u);
    if (host) {
        const uint8_t* mainP = monBuf.data();
        const uint8_t* auxP  = monBuf.data() + kHiresSize;
        switch (monMode) {
        case 0: host->renderHgrPage(mainP, monRgba.data(), !ntscColor, false); break;
        case 1: host->renderHgrPage(mainP, monRgba.data(), !ntscColor, true);  break;
        case 2: host->renderDhgrPage(auxP, mainP, monRgba.data(), !ntscColor); break;
        case 3: host->renderDlgrPage(auxP, mainP, monRgba.data(), !ntscColor); break;
        default:
            host->renderTextPage(mainP, haveAux ? auxP : nullptr, monCol80,
                                 monAltChar, monRgba.data(), !ntscColor);
            break;
        }
        monTex = host->uploadTexture(monTex, monRgba.data(), w, kHiresHeight,
                                     /*linear=*/false);
    }
    if (dbl && !haveAux) ImGui::TextDisabled("(no aux bank on this machine)");

    // Fit the page to the window, 280 × 192 logical, square pixels.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float  lineH = ImGui::GetTextLineHeightWithSpacing();
    const float  k = std::max(1.0f, std::min(avail.x / kHiresWidth,
                                             (avail.y - lineH) / kHiresHeight));
    const ImVec2 size(kHiresWidth * k, kHiresHeight * k);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (host && monTex) ImGui::Image(host->textureToImTexture(monTex), size);
    else                ImGui::Dummy(size);

    // The byte under the mouse: its address, its bank, its value.
    if (!ImGui::IsItemHovered()) return;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int lx = std::clamp(static_cast<int>((m.x - origin.x) / k), 0, kHiresWidth - 1);
    const int ly = std::clamp(static_cast<int>((m.y - origin.y) / k), 0, kHiresHeight - 1);
    // Byte column in the 40-byte grid, or the 80-cell grid of a double mode /
    // 80 columns (even cells from aux, odd from main — the order the
    // renderers use, Apple2Display::renderText80 / renderDhgr).
    const int col = wide ? (lx * 2) / 7 : lx / 7;
    const bool inAux = wide && (col % 2 == 0);
    const int byteCol = wide ? col / 2 : col;
    const int off = lores ? grBlockOffset(byteCol, ly / 4)
                          : (hgrRowAddress(ly) - kHiresBase) + byteCol;
    if (off < 0) return;
    const uint16_t addr = static_cast<uint16_t>(base + off);
    if (inAux && !haveAux) return;
    const std::vector<uint8_t>& src = inAux ? *aux : memory;
    if (addr >= src.size()) return;
    ImGui::SetTooltip("$%04X %s = $%02X\n%s %d, %s %d", addr, inAux ? "aux" : "main",
                      src[addr], lores ? "text row" : "line",
                      lores ? ly / 8 : ly, wide ? "cell" : "byte column", col);
}
