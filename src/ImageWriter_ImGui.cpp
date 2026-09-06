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

// ImageWriter_ImGui — see the header for why this panel owns GL state.

#include "ImageWriter_ImGui.h"
#include "Pom2Build.h"

#include "IconsFontAwesome6.h"
#include "AtomicFileReplace.h"
#include "ImageWriterPdf.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "Pom2GL.h"

// Declarations only — the single non-static stb_image_write implementation
// lives in Pom2HgrPaintHost.cpp (see the note at the top of that file).
#include "stb_image_write.h"

namespace pom2 {

namespace {

constexpr float kZooms[] = { 0.25f, 0.5f, 0.75f, 1.0f, 2.0f };
constexpr const char* kZoomLabels =
    "Fit width\0" "25%\0" "50%\0" "75%\0" "100%\0" "200%\0";

// Page DPI choices. 144 is the reference's default; 72/96 are cheap
// previews, 216/288 are 3x/4x the 72 dpi pin pitch for print-quality PNGs.
constexpr int kDpis[] = { 72, 96, 120, 144, 216, 288 };
constexpr const char* kDpiLabels =
    "72 dpi\0" "96 dpi\0" "120 dpi\0" "144 dpi\0" "216 dpi\0" "288 dpi\0";

std::string timestampedName(const char* prefix, const char* ext)
{
    const std::time_t t = std::time(nullptr);
    // localtime_r, not localtime: the latter hands back a shared static `tm`,
    // and ClockCard converts times of its own on the CPU thread (a ProDOS
    // timestamp under stateMutex). Same split as NoSlotClock.cpp:12-18.
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return std::string(prefix) + stamp + ext;
}

} // namespace

ImageWriter_ImGui::~ImageWriter_ImGui() { shutdown(); }

void ImageWriter_ImGui::shutdown()
{
    if (tex_) {
        GLuint t = tex_;
        glDeleteTextures(1, &t);
        tex_ = 0;
    }
    texPage_ = -2;
    texRev_  = 0;
    texW_ = texH_ = 0;
}

void ImageWriter_ImGui::uploadPage(const ImageWriter::Page& p,
                                   int pageIdx, uint32_t rev)
{
    if (p.w <= 0 || p.h <= 0) return;
    // Completed sheets never change, so their revision is irrelevant; the
    // sheet in progress re-uploads whenever the printer touched a dot.
    if (tex_ && texPage_ == pageIdx && (pageIdx >= 0 || texRev_ == rev)) return;

    ImageWriter::pageToRgba(p, rgba_);

    if (!tex_) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_  = t;
        texW_ = texH_ = 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (p.w != texW_ || p.h != texH_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.w, p.h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
        texW_ = p.w;
        texH_ = p.h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p.w, p.h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
    }
    texPage_ = pageIdx;
    texRev_  = rev;
}

void ImageWriter_ImGui::uploadRgba(const std::vector<uint8_t>& rgba,
                                   int w, int h, int key)
{
    if (w <= 0 || h <= 0 ||
        rgba.size() < static_cast<size_t>(w) * h * 4) return;
    if (tex_ && texPage_ == key) return;      // already resident

    if (!tex_) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tex_  = t;
        texW_ = texH_ = 0;
    }
    glBindTexture(GL_TEXTURE_2D, tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != texW_ || h != texH_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        texW_ = w;
        texH_ = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    }
    texPage_ = key;
    texRev_  = 0;
}

bool ImageWriter_ImGui::savePagePng(const ImageWriter::Page& p,
                                    const std::string& path, std::string& err)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path out(path);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);

    std::vector<uint8_t> rgba;
    ImageWriter::pageToRgba(p, rgba);
    const fs::path tmp = out.string() + ".pom2tmp";
    // The user picked `out`; this sibling name is ours by construction and got
    // none of that choice's scrutiny, while stb's fopen("wb") follows symlinks
    // like any other open. Clear the way (or refuse) before writing — the same
    // guard every other write-back path in POM2 runs. See prepareTempPath.
    std::error_code prepEc;
    if (!prepareTempPath(tmp, prepEc)) {
        err = "temp path unusable " + tmp.string() + ": " + prepEc.message();
        return false;
    }
    if (stbi_write_png(tmp.string().c_str(), p.w, p.h, 4,
                       rgba.data(), p.w * 4) == 0) {
        err = "stbi_write_png failed (is " + out.string() + " writable?)";
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code permEc;
    const auto perms = fs::status(out, permEc).permissions();
    if (!permEc) fs::permissions(tmp, perms, ec);
    ec.clear();
    if (!replaceFileAtomic(tmp, out, ec)) {
        err = "cannot replace " + out.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void ImageWriter_ImGui::render(bool* open, ImageWriter& iw,
                               const HostInfo& host)
{
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_PRINT " ImageWriter II###imageWriterPanel",
                      open)) {
        ImGui::End();
        return;
    }

    // ─── Front panel ─────────────────────────────────────────────────────
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT " Form feed")) {
        iw.formFeed();
        follow_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Eject the sheet in progress onto the stack.\n"
                          "A blank sheet is not ejected (like the real button).");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset printer")) {
        iw.resetPrinterHard();
        status_ = "Printer reset — pitch, margins and soft switches back to defaults.";
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Power-cycle the printer: factory settings, current "
                          "sheet discarded. Ejected sheets are kept.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear all")) {
        iw.clearAll();
        viewPage_ = -1;
        texPage_  = -2;
        status_   = "Paper tray emptied.";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", host.haveSource
                        ? host.sourceLabel.c_str()
                        : "no printer interface card plugged");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The ImageWriter is the printer, not the card. "
                          "Plug a Printer or Grappler+ card in Slot Config, "
                          "then PR#n from BASIC.");

    // ─── Sheet selection ─────────────────────────────────────────────────
    // Which sheet the panel is showing is DERIVED state over a vector the
    // printer owns, so it is resolved on demand and never cached across a
    // call into `iw`. Any eject invalidates it two ways:
    //
    //   * `newPage()` push_back()s into `pages_`, which reallocates — a
    //     `const Page&` taken before it dangles (ASan caught exactly that
    //     on the "Print now" path below, which calls flushPending());
    //   * at the 32-sheet cap `pages_.erase(pages_.begin())` shifts without
    //     reallocating, so no reference dangles but every index silently
    //     names a different sheet, and `shown` / `nDone` / the texture
    //     cache key stop agreeing with each other.
    //
    // Hence: mutate first, `resolve()` after, and pass `sheet(sel)` into
    // the call that needs it rather than holding it. "Form feed", "Reset
    // printer" and "Clear all" above sit before the first resolve for the
    // same reason.
    struct Selection {
        size_t nDone  = 0;   // completed sheets on the stack
        int    nTotal = 1;   // + the one under the head
        int    shown  = 0;   // 0 .. nTotal-1
    };
    auto resolve = [&]() -> Selection {
        Selection s;
        s.nDone  = iw.completedPageCount();
        s.nTotal = static_cast<int>(s.nDone) + 1;
        // "Follow" means "show me what is being printed". After a form feed
        // the sheet under the head is blank and the interesting one is on
        // the stack, so follow the last *inked* sheet until the new one
        // gets ink — otherwise a one-page job looks like it printed nothing
        // at all.
        if (follow_) {
            viewPage_ = (s.nDone > 0 && iw.currentPageBlank())
                            ? static_cast<int>(s.nDone) - 1
                            : -1;
        }
        s.shown = (viewPage_ < 0) ? static_cast<int>(s.nDone) : viewPage_;
        s.shown = std::clamp(s.shown, 0, s.nTotal - 1);
        return s;
    };
    auto sheet = [&iw](const Selection& s) -> const ImageWriter::Page& {
        return (s.shown < static_cast<int>(s.nDone))
                   ? iw.completedPage(static_cast<size_t>(s.shown))
                   : iw.currentPage();
    };
    // Texture-cache identity of the shown sheet (-1 = the one in progress).
    // It includes droppedPageCount() because when the cap drops the oldest
    // sheet every index renames a different page, and a bare index let the
    // cached texture show the dropped sheet's pixels under the new sheet's
    // label.
    auto cacheKey = [&iw](const Selection& s) {
        return (s.shown < static_cast<int>(s.nDone))
                   ? static_cast<int>(iw.droppedPageCount()) + s.shown
                   : -1;
    };

    Selection sel = resolve();

    // ─── Page selector ───────────────────────────────────────────────────
    ImGui::BeginDisabled(sel.shown <= 0);
    if (ImGui::Button(ICON_FA_ARROW_LEFT "##pgPrev")) {
        viewPage_ = sel.shown - 1;
        follow_   = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(sel.shown >= sel.nTotal - 1);
    if (ImGui::Button(ICON_FA_ARROW_RIGHT "##pgNext")) {
        viewPage_ = (sel.shown + 1 >= sel.nTotal - 1) ? -1 : sel.shown + 1;
        follow_   = (sel.shown + 1 >= sel.nTotal - 1);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("Sheet %d / %d%s", sel.shown + 1, sel.nTotal,
                (sel.shown == sel.nTotal - 1) ? "  (in the printer)" : "");
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow", &follow_) && follow_) viewPage_ = -1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Always show the sheet currently under the head.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##zoom", &zoomMode_, kZoomLabels);

    if (iw.droppedPageCount() > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu older sheet%s dropped)",
                            iw.droppedPageCount(),
                            iw.droppedPageCount() == 1 ? "" : "s");
    }

    // ─── Printer settings ────────────────────────────────────────────────
    // ─── Print history (printer plan phase E) ────────────────────────────
    if (ImGui::CollapsingHeader("Print history")) {
        if (host.history.empty()) {
            ImGui::TextDisabled(
                "Nothing stored yet. Every sheet the printer ejects is saved "
                "here and survives quitting.");
            if (!host.historyDir.empty())
                ImGui::TextDisabled("Folder: %s", host.historyDir.c_str());
        } else {
            ImGui::Text("%zu stored page(s)", host.history.size());
            ImGui::SameLine();
            if (viewHistory_ >= 0 && ImGui::SmallButton("Back to the platen")) {
                viewHistory_ = -1;
                historyFile_.clear();
                texPage_ = -2;          // force the live sheet to re-upload
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear all") && host.onClearHistory) {
                host.onClearHistory();
                viewHistory_ = -1;
                historyFile_.clear();
                texPage_ = -2;
            }
            if (!host.historyDir.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("Deletes every stored page from %s",
                                  host.historyDir.c_str());

            if (ImGui::BeginTable("##iwHistory", 5,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, 160))) {
                ImGui::TableSetupColumn("When");
                ImGui::TableSetupColumn("Printer");
                ImGui::TableSetupColumn("Ribbon", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Paper",  ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableHeadersRow();

                std::string deleteFile;
                for (size_t i = 0; i < host.history.size(); ++i) {
                    const auto& r = host.history[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(i));

                    ImGui::TableNextColumn();
                    const bool sel2 = (viewHistory_ == static_cast<int>(i));
                    if (ImGui::Selectable(r.savedAt.c_str(), sel2,
                                          ImGuiSelectableFlags_SpanAllColumns)) {
                        viewHistory_ = sel2 ? -1 : static_cast<int>(i);
                        if (viewHistory_ < 0) { historyFile_.clear(); texPage_ = -2; }
                    }
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.printer.c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ribbon.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f x %.2f\"", r.paperW, r.paperL);
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Delete")) deleteFile = r.file;

                    ImGui::PopID();
                }
                ImGui::EndTable();

                // Applied after the loop: deleting inside it would invalidate
                // the very vector being iterated.
                if (!deleteFile.empty() && host.onDeleteHistoryPage) {
                    host.onDeleteHistoryPage(deleteFile);
                    viewHistory_ = -1;
                    historyFile_.clear();
                    texPage_ = -2;
                }
            }
            ImGui::TextDisabled(
                "Click a row to put that sheet back on the canvas. The PNGs "
                "are in %s and can be opened directly.",
                host.historyDir.empty() ? "the printouts folder"
                                        : host.historyDir.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Printer settings")) {
        int paperIdx = static_cast<int>(iw.paperSize());
        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("Paper", ImageWriter::paperSizeName(
                                           iw.paperSize()))) {
            for (int i = 0; i < static_cast<int>(
                                    ImageWriter::PaperSize::Count); ++i) {
                const auto s = static_cast<ImageWriter::PaperSize>(i);
                if (ImGui::Selectable(ImageWriter::paperSizeName(s),
                                      i == paperIdx)) {
                    iw.setPaperSize(s);
                    texPage_ = -2;
                }
            }
            ImGui::EndCombo();
        }

        int dpiIdx = 3;
        for (int i = 0; i < static_cast<int>(sizeof(kDpis) / sizeof(kDpis[0])); ++i)
            if (kDpis[i] == iw.dpi()) dpiIdx = i;
        ImGui::SetNextItemWidth(240);
        if (ImGui::Combo("Page resolution", &dpiIdx, kDpiLabels)) {
            iw.setDpi(kDpis[dpiIdx]);
            texPage_ = -2;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Raster density of the rendered page. The "
                              "printer's own dot densities (72-320 dpi) are "
                              "set by the guest and unaffected.\n"
                              "Changing this restarts the sheet in progress.");

        // ── Which head (printer plan phase C) ─────────────────────────
        {
            int mi = static_cast<int>(iw.model());
            const char* labels[static_cast<int>(pom2::IwModel::Count)];
            for (int i = 0; i < static_cast<int>(pom2::IwModel::Count); ++i)
                labels[i] = ImageWriter::modelName(static_cast<pom2::IwModel>(i));
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo("Printer", &mi, labels,
                             static_cast<int>(pom2::IwModel::Count))) {
                iw.setModel(static_cast<pom2::IwModel>(mi));
                texPage_ = -2;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "All three speak the C. Itoh 8510 command set; the "
                    "ImageWriter II is the superset.\n"
                    "The I and the DMP have no colour ribbon and no draft/NLQ "
                    "tiers, and the DMP powers up at Pica 10 cpi.\n"
                    "Switching is a power cycle — the faces and the pitch "
                    "change, so the sheet cannot carry over.");
        }

        // ── Front panel (printer plan phase D) ───────────────────────
        {
            bool pw = iw.powered();
            if (ImGui::Checkbox("Power", &pw)) iw.setPowered(pw);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Switched off, the printer ignores everything the Apple II "
                    "sends — and KEEPS THE PAPER.\n"
                    "That is the difference from a power cycle, which clears "
                    "the sheet.");

            ImGui::SameLine();
            bool on = iw.online();
            if (ImGui::Checkbox("Select (online)", &on)) iw.setOnline(on);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Deselected, the printer is powered but unreachable — the "
                    "front-panel button.\n"
                    "This is the usual reason a real one appears to hang: the "
                    "data goes nowhere.");

            if (!iw.powered() || !iw.online()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f),
                                   iw.powered() ? "offline — data discarded"
                                                : "off — data discarded");
            }

            // Paper size, in the quarter-inch steps a tractor is adjusted in.
            float wIn = static_cast<float>(iw.paperWidthIn());
            float lIn = static_cast<float>(iw.paperLengthIn());
            ImGui::SetNextItemWidth(110);
            const bool wCh = ImGui::DragFloat("##paperW", &wIn, 0.25f,
                                              static_cast<float>(ImageWriter::kMinPaperWidthIn),
                                              static_cast<float>(ImageWriter::kMaxPaperWidthIn),
                                              "%.2f\" wide");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            const bool lCh = ImGui::DragFloat("Paper", &lIn, 0.25f,
                                              static_cast<float>(ImageWriter::kMinPaperLengthIn),
                                              static_cast<float>(ImageWriter::kMaxPaperLengthIn),
                                              "%.2f\" long");
            if (wCh || lCh) {
                // The printer clamps and snaps; take back what it committed
                // so the widget cannot disagree with the paper.
                double cw = 0, cl = 0;
                iw.setPaperDimensions(wIn, lIn, &cw, &cl);
                texPage_ = -2;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Continuous tractor paper, in 1/4\" steps. The form length "
                    "the guest sets with ESC H\noverrides this for pagination; "
                    "changing either restarts the sheet in progress.");
        }

        ImGui::Separator();

        if (host.onBackPressureChanged) {
            bool bp = host.backPressure;
            if (ImGui::Checkbox("Make the Apple II wait for the printer",
                                &bp))
                host.onBackPressureChanged(bp);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The real handshake: once the printer's 2 KB buffer is "
                    "full it stops acknowledging,\nand the guest's firmware "
                    "spins until the paper catches up.\n"
                    "Faithful — a real Apple II sat there for minutes "
                    "printing a Print Shop card — but an\nemulator that "
                    "stops responding that long is indistinguishable from a "
                    "crash, so it is off\nby default. The printout builds up "
                    "at the same speed either way.");
        }

        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("Ribbon", ImageWriter::ribbonName(iw.ribbon()))) {
            for (int i = 0; i < static_cast<int>(ImageWriter::Ribbon::Count);
                 ++i) {
                const auto rb = static_cast<ImageWriter::Ribbon>(i);
                if (ImGui::Selectable(ImageWriter::ribbonName(rb),
                                      rb == iw.ribbon()))
                    iw.setRibbon(rb);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Which cartridge is in the printer. There is no colour "
                "\"mode\" on an ImageWriter II —\ncolour is the four-band "
                "ribbon, and the software picks a band with ESC K.\n"
                "So the guest has to ask for it: in Print Shop that means "
                "Setup → Printer →\n\"Apple Imagewriter II (C)\". The (M) "
                "driver never sends colour, whatever is fitted.");

        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("Line feed after CR",
                              ImageWriter::autoFeedName(iw.autoFeedMode()))) {
            for (int i = 0; i < static_cast<int>(ImageWriter::AutoFeed::Count);
                 ++i) {
                const auto m = static_cast<ImageWriter::AutoFeed>(i);
                if (ImGui::Selectable(ImageWriter::autoFeedName(m),
                                      m == iw.autoFeedMode()))
                    iw.setAutoFeedMode(m);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The printer's SW A-8 switch.\n"
                "A bare PR#n : PRINT sends CR only, so the printer has to "
                "supply the feed.\nReal drivers send CR+LF themselves "
                "(double-spaced if the printer adds one too), and colour\n"
                "drivers put a bare CR between passes so they overprint — "
                "feed there and the colours\nstagger down the page.\n"
                "Auto watches the stream and settles it: no wrong answer "
                "to pick.");
        if (iw.autoFeedMode() == ImageWriter::AutoFeed::Auto) {
            ImGui::SameLine();
            ImGui::TextDisabled(iw.autoFeedLatchedOff()
                                ? "(guest feeds its own lines)"
                                : "(printer is feeding)");
        }

        // ─── Interface-card DIP (Grappler+ S1 printer type) ──────────────
        if (!host.cardDipOptions.empty() && host.onCardDipChanged) {
            const char* cur = "(unknown)";
            for (const auto& o : host.cardDipOptions)
                if (o.value == host.cardDipValue) cur = o.label;
            ImGui::SetNextItemWidth(240);
            if (ImGui::BeginCombo("Card emulates", cur)) {
                for (const auto& o : host.cardDipOptions) {
                    if (ImGui::Selectable(o.label, o.value == host.cardDipValue))
                        host.onCardDipChanged(o.value);
                    if (o.value == host.cardDipRecommended) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(matches this printer)");
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "The interface card's DIP switches tell its firmware "
                    "which printer is on the cable.\nSet to anything but "
                    "the ImageWriter family, the card sends another "
                    "printer's escape codes\n(Epson graphics, for one) and "
                    "this printer renders them as garbage characters.");
            if (host.cardDipRecommended >= 0 &&
                host.cardDipValue != host.cardDipRecommended) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                                   ICON_FA_TRIANGLE_EXCLAMATION
                                   " not an ImageWriter dialect");
            }
        }

        // ─── Trace log ───────────────────────────────────────────────────
        bool tracing = iw.tracing();
        if (ImGui::Checkbox("Log the printer stream to a file", &tracing)) {
            if (tracing) {
                namespace fs = std::filesystem;
                const std::string path =
                    (fs::path(host.saveDir) / "imagewriter_trace.log").string();
                std::string err;
                status_ = iw.startTrace(path, err)
                        ? "Tracing to " + path
                        : "Cannot start trace: " + err;
            } else {
                iw.stopTrace();
                status_ = "Trace stopped.";
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Writes every byte the printer receives, decoded (escape "
                "sequences, graphics setup, page ejects),\nto "
                "the per-user printouts folder. Turn this on when a "
                "printout comes out wrong —\nthe log says whether the guest "
                "is speaking this printer's language.");
        if (iw.tracing()) {
            ImGui::SameLine();
            ImGui::TextDisabled("→ %s", iw.tracePath().c_str());
        }

        // The trace has to be armed before the job; this doesn't — the
        // last 256 KB the printer received is always kept, so a printout
        // that already came out wrong can still be handed over verbatim.
        ImGui::BeginDisabled(iw.rawStream().empty() || !host.canSaveFiles);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save raw stream…")) {
            namespace fs = std::filesystem;
            const std::string path =
                (fs::path(host.saveDir) /
                 timestampedName("imagewriter-stream-", ".bin")).string();
            std::error_code ec;
            fs::create_directories(host.saveDir, ec);
            std::ofstream f(path, std::ios::binary);
            const auto& raw = iw.rawStream();
            f.write(reinterpret_cast<const char*>(raw.data()),
                    static_cast<std::streamsize>(raw.size()));
            status_ = f ? "Wrote " + std::to_string(raw.size()) +
                              " bytes → " + path
                        : "Cannot write " + path;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Dump the exact bytes this printer received (the last "
                "256 KB), no matter when they arrived.\nThat file is what "
                "settles \"is the printout wrong, or is the guest sending "
                "something else?\".");
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu B held)", iw.rawStream().size());

        ImGui::SetNextItemWidth(240);
        if (ImGui::BeginCombo("Print speed",
                              ImageWriter::speedName(iw.speed()))) {
            for (int i = 0; i < static_cast<int>(ImageWriter::Speed::Count); ++i) {
                const auto s = static_cast<ImageWriter::Speed>(i);
                if (ImGui::Selectable(ImageWriter::speedName(s), s == iw.speed()))
                    iw.setSpeed(s);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "How fast the mechanism prints what the card sends it.\n"
                "Draft / NLQ are the real ImageWriter II rates, so a page\n"
                "builds up line by line and the Apple II waits on BUSY once\n"
                "the printer's 2 KB buffer is full — like the real thing.\n"
                "Instant prints everything the moment it arrives.");
    }

    // Paper size, page DPI and Instant speed all reach `resetPrinter()` /
    // `flushPending()`, which eject whatever was on the platen — so the
    // selection resolved for the page selector above may be a sheet out of
    // date by here (see Selection).
    sel = resolve();

    // ─── Save ────────────────────────────────────────────────────────────
    if (!host.canSaveFiles) {
        ImGui::BeginDisabled();
        ImGui::Button(ICON_FA_FLOPPY_DISK " Save sheet as PNG");
        ImGui::SameLine();
        ImGui::Button("Save all sheets");
        ImGui::SameLine();
        ImGui::Button("Save PDF");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(unavailable in the browser build)");
    } else {
        namespace fs = std::filesystem;
        if (ImGui::Button(ICON_FA_FLOPPY_DISK " Save sheet as PNG")) {
            const std::string path =
                (fs::path(host.saveDir) /
                 timestampedName("imagewriter-", ".png")).string();
            std::string err;
            status_ = savePagePng(sheet(sel), path, err)
                    ? "Saved " + path
                    : "Save failed: " + err;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(sel.nDone == 0);
        if (ImGui::Button("Save all sheets")) {
            const std::string base = timestampedName("imagewriter-", "");
            size_t ok = 0;
            std::string err;
            for (size_t i = 0; i < sel.nDone; ++i) {
                char suffix[16];
                std::snprintf(suffix, sizeof(suffix), "-p%02zu.png", i + 1);
                const std::string path =
                    (fs::path(host.saveDir) / (base + suffix)).string();
                if (savePagePng(iw.completedPage(i), path, err)) ++ok;
            }
            status_ = "Saved " + std::to_string(ok) + " / " +
                      std::to_string(sel.nDone) + " sheet(s) → " +
                      host.saveDir +
                      (ok == sel.nDone ? "" : ("  (" + err + ")"));
        }
        ImGui::EndDisabled();

        // Whole job as one multi-page PDF: every completed sheet plus the
        // sheet in progress if anything is on it.
        ImGui::SameLine();
        if (ImGui::Button("Save PDF")) {
            std::vector<const ImageWriter::Page*> sheets;
            for (size_t i = 0; i < sel.nDone; ++i)
                sheets.push_back(&iw.completedPage(i));
            if (!iw.currentPageBlank())
                sheets.push_back(&iw.currentPage());
            if (sheets.empty()) {
                status_ = "Nothing to export — the paper is blank.";
            } else {
                const std::string path =
                    (fs::path(host.saveDir) /
                     timestampedName("imagewriter-", ".pdf")).string();
                std::string err;
                status_ = writeImageWriterPdf(sheets, path, err)
                        ? "Saved " + std::to_string(sheets.size()) +
                          " page(s) → " + path
                        : "PDF save failed: " + err;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("All completed sheets (plus the one in the\n"
                              "platen, if printed on) as one PDF file.");
    }

    // ─── Status line ─────────────────────────────────────────────────────
    const ImageWriter::Status st = iw.status();
    ImGui::Separator();
    ImGui::TextDisabled(
        "%llu byte%s  |  head %.2f\" x %.2f\"  |  %.1f cpi  |  %d dpi gfx"
        "  |  ribbon %s  |  %s%s",
        static_cast<unsigned long long>(iw.bytesReceived()),
        iw.bytesReceived() == 1 ? "" : "s",
        st.headX, st.headY, st.cpi, st.graphicsDpi, st.colorName,
        st.styleText.c_str(), st.inGraphics ? "  |  bit-image" : "");

    // While the mechanism is behind the card, say so and offer the
    // impatient way out (the real printer has no such button).
    if (iw.busy()) {
        ImGui::SameLine();
        const bool waiting =
            host.backPressure &&
            iw.pendingBytes() > ImageWriter::kInputBufferBytes;
        // A repeat run (ESC R / ESC V / ESC U) is work the mechanism owes
        // that never sat in the input buffer, so the byte count alone would
        // read "0 B queued" for a printer that is visibly still going.
        char queued[64];
        if (iw.pendingRepeats() > 0)
            std::snprintf(queued, sizeof queued,
                          "%zu B queued + %zu repeat%s", iw.pendingBytes(),
                          iw.pendingRepeats(),
                          iw.pendingRepeats() == 1 ? "" : "s");
        else
            std::snprintf(queued, sizeof queued, "%zu B queued",
                          iw.pendingBytes());
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                           "  |  " ICON_FA_PRINT " printing… %s%s", queued,
                           waiting ? " — the Apple II is waiting" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("Print now")) {
            // Prints the whole queue, so it can eject: re-resolve before
            // anything downstream touches the stack again (see Selection).
            iw.flushPending();
            sel = resolve();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Skip the mechanism delay for what is still "
                              "queued.");
    }
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

    // ─── Page view ───────────────────────────────────────────────────────
    // A stored page takes over the canvas while the user is browsing the
    // history; leaving it drops straight back to whatever sheet they were on.
    bool showingHistory = false;
    if (viewHistory_ >= 0 &&
        viewHistory_ < static_cast<int>(host.history.size()) &&
        host.loadHistoryPage) {
        const auto& row = host.history[static_cast<size_t>(viewHistory_)];
        if (historyFile_ != row.file) {
            std::vector<uint8_t> px;
            int hw = 0, hh = 0;
            if (host.loadHistoryPage(row.file, px, hw, hh)) {
                // Reaching here means the FILE under this row changed, so the
                // cached texture is stale by definition — invalidate before
                // uploading. The key alone cannot say so: it is derived from
                // viewHistory_, and archiving a new sheet pushes it onto the
                // front of the newest-first list, changing the file under a
                // FIXED index. That left the previously viewed page's pixels
                // (and its paper decoration) on screen under the new page's
                // label, permanently. Same lesson the live-page key learned by
                // folding droppedPageCount() into its own key.
                texPage_ = -2;
                // Negative keys: a stored page can never collide with a live
                // page index in the texture cache.
                uploadRgba(px, hw, hh, -1000 - viewHistory_);
                historyFile_ = row.file;
                showingHistory = true;
            } else {
                viewHistory_ = -1;      // vanished under us
                historyFile_.clear();
            }
        } else {
            showingHistory = true;
        }
    } else {
        viewHistory_ = -1;
        historyFile_.clear();
    }

    if (!showingHistory)
        uploadPage(sheet(sel), cacheKey(sel), iw.revision());

    ImGui::BeginChild("##iwPaper", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (iw.bytesReceived() == 0 && sel.nDone == 0) {
        ImGui::TextDisabled(
            "Nothing printed yet. Plug a Printer or Grappler+ card in\n"
            "Slot Config, then from BASIC:   PR#1 : PRINT \"HELLO\" : PR#0");
        ImGui::Separator();
    }
    if (tex_ && texW_ > 0) {
        // An ImageWriter II is fed continuous fanfold stock, not cut sheets:
        // 9.5" wide paper = the 8.5" printable body plus a 0.5" pin-feed
        // strip each side, each strip perforated off along a vertical line
        // of holes on 0.5" centres, and one sheet joined to the next by a
        // horizontal perforation. Drawing only the printable raster made
        // POM2's paper look like an inkjet A4 sheet. The strips are paper,
        // not ink — they're decoration around the raster, so the page
        // bitmap and the "Save sheet as PNG" export stay pure printable
        // area. (Apple, *ImageWriter II Owner's Manual*, "Paper".)
        const int   dpi      = iw.dpi();
        const float stripPx  = 0.5f  * dpi;     // pin-feed strip
        const float holeR    = 0.078f * dpi;    // ⌀ ~4 mm
        const float holePitch= 0.5f  * dpi;     // holes on 1/2" centres
        const float totalW   = texW_ + 2.0f * stripPx;

        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = (zoomMode_ == 0)
                          ? std::max(0.05f, avail / totalW)
                          : kZooms[zoomMode_ - 1];

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + totalW * scale, p0.y + texH_ * scale);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImU32 kPaper = IM_COL32(250, 249, 244, 255);
        const ImU32 kHole  = IM_COL32( 42,  42,  46, 255);
        const ImU32 kPerf  = IM_COL32(196, 192, 182, 255);

        dl->AddRectFilled(p0, p1, kPaper);

        // Sprocket holes down both strips.
        for (float y = holePitch * 0.5f; y < texH_; y += holePitch) {
            const float cy = p0.y + y * scale;
            for (int side = 0; side < 2; ++side) {
                const float cx = p0.x + (side ? totalW - stripPx * 0.5f
                                              : stripPx * 0.5f) * scale;
                dl->AddCircleFilled(ImVec2(cx, cy), holeR * scale, kHole, 12);
            }
        }
        // Tear-off perforations: vertical between each strip and the body,
        // horizontal where this sheet joins the next.
        auto dottedV = [&](float x) {
            const float sx = p0.x + x * scale;
            for (float y = 0; y < texH_; y += 0.06f * dpi)
                dl->AddLine(ImVec2(sx, p0.y + y * scale),
                            ImVec2(sx, p0.y + (y + 0.03f * dpi) * scale),
                            kPerf, 1.0f);
        };
        auto dottedH = [&](float y) {
            const float sy = p0.y + y * scale;
            for (float x = 0; x < totalW; x += 0.06f * dpi)
                dl->AddLine(ImVec2(p0.x + x * scale, sy),
                            ImVec2(p0.x + (x + 0.03f * dpi) * scale, sy),
                            kPerf, 1.0f);
        };
        dottedV(stripPx);
        dottedV(stripPx + texW_);
        dottedH(0.0f);
        dottedH(static_cast<float>(texH_));

        ImGui::SetCursorScreenPos(ImVec2(p0.x + stripPx * scale, p0.y));
        ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex_)),
                     ImVec2(texW_ * scale, texH_ * scale));
        // Reserve the strips + the joins so the scroll region covers them.
        ImGui::SetCursorScreenPos(p0);
        ImGui::Dummy(ImVec2(totalW * scale, texH_ * scale));
    } else {
        ImGui::TextDisabled("(no page)");
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace pom2
