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

// ImageWriter_ImGui — the paper tray: shows what the emulated Apple
// ImageWriter II has printed. Front-panel buttons (FORM FEED / RESET),
// the completed-sheet stack, a zoomable page view and PNG export.
//
// Unlike the other *_ImGui panels this one is not purely data-in /
// actions-out: it owns a GL texture (the page raster is a megapixel image,
// so it is uploaded once per change rather than rebuilt by the caller) and
// it drives the `ImageWriter` directly. That is safe because an
// `ImageWriter` is host-side only — no emulator state, no `stateMutex`,
// UI thread throughout. MainWindow still owns the printer itself and is
// the one that feeds it bytes drained from the interface card's spool.

#ifndef POM2_IMAGEWRITER_IMGUI_H
#define POM2_IMAGEWRITER_IMGUI_H

#include "ImageWriter.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class ImageWriter_ImGui
{
public:
    struct HostInfo {
        /// Where the bytes come from, e.g. "Printer card (slot 1)".
        std::string sourceLabel;
        bool        haveSource   = false;
        /// False on the browser build — nothing written to MEMFS survives.
        bool        canSaveFiles = true;
        /// Directory PNG exports land in (created on demand).
        std::string saveDir      = "printouts";

        /// Interface-card DIP block, when the plugged card has one (the
        /// Grappler+'s S1 printer-type switches). The panel is the natural
        /// home for it: what the card *says* it is talking to decides
        /// which dialect of escape codes reaches this printer, and a
        /// mismatch is exactly what makes a printout come out as noise.
        /// Empty options list = no DIP UI for this card.
        struct DipOption { const char* label; int value; };
        std::vector<DipOption>    cardDipOptions;
        int                       cardDipValue = 0;
        std::function<void(int)>  onCardDipChanged;
        /// Value the card should be on to speak this printer's language.
        int                       cardDipRecommended = -1;

        /// Whether a full printer buffer blocks the guest (real ACK
        /// handshake). Host-owned because it lives on the card, not the
        /// printer; surfaced here because this is where its effect shows.
        bool                      backPressure = false;
        std::function<void(bool)> onBackPressureChanged;

        // ── Durable print history (printer plan phase E) ──────────────────
        // The host owns the store; the panel only lists it and asks. One row
        // per stored page, newest first.
        struct HistoryRow {
            std::string file;      ///< bare filename inside historyDir
            std::string savedAt;
            std::string printer;   ///< model name at the time
            std::string ribbon;
            uint64_t    job = 0;
            int         w = 0, h = 0;
            double      paperW = 0.0, paperL = 0.0;
        };
        std::vector<HistoryRow> history;
        std::string             historyDir;
        /// Fetch a stored page as RGBA, for the re-preview. Returns false if
        /// the file has gone since the list was built.
        std::function<bool(const std::string& file, std::vector<uint8_t>& rgba,
                           int& w, int& h)> loadHistoryPage;
        std::function<void(const std::string& file)> onDeleteHistoryPage;
        std::function<void()>                        onClearHistory;
    };

    ImageWriter_ImGui() = default;
    ~ImageWriter_ImGui();

    ImageWriter_ImGui(const ImageWriter_ImGui&)            = delete;
    ImageWriter_ImGui& operator=(const ImageWriter_ImGui&) = delete;

    void render(bool* open, ImageWriter& iw, const HostInfo& host);

    /// Drop the GL texture (call before the GL context goes away).
    void shutdown();

private:
    unsigned int tex_  = 0;
    int          texW_ = 0, texH_ = 0;
    /// What the texture currently holds: page index (-1 = the sheet in
    /// progress) + the raster revision it was built from.
    int          texPage_ = -2;
    uint32_t     texRev_  = 0;

    int   viewPage_ = -1;        // -1 = sheet in progress
    /// Index into HostInfo::history when the canvas is showing a STORED page
    /// instead of one on the platen; -1 = not in history view. Kept separate
    /// from viewPage_ so leaving the history returns to whatever sheet the
    /// user was on.
    int   viewHistory_ = -1;
    std::string historyFile_;    // which stored file the texture holds
    bool  follow_   = true;      // snap to the newest sheet
    int   zoomMode_ = 0;         // 0 = fit width, else index into kZooms
    std::vector<uint8_t> rgba_;  // scratch, reused across uploads

    /// The fanfold view: one texture per sheet in view (sheetTexture).
    static constexpr int kNoSheetKey = -0x7FFFFFFF;
    struct SheetTex {
        unsigned id = 0;
        int      w = 0, h = 0;
        int      key = kNoSheetKey;
        uint32_t rev = 0;
        int      lastFrame = 0;
    };
    std::vector<SheetTex> sheetTex_;
    /// The arrows asked for a sheet: bring it to the top of the view.
    bool scrollToShown_ = false;
    unsigned sheetTexture(const ImageWriter::Page& p, int key, uint32_t rev, int frame);
    void     trimSheetTextures(int frame);
    std::string status_;

    void uploadPage(const ImageWriter::Page& p, int pageIdx, uint32_t rev);
    /// Upload an arbitrary RGBA image (a stored page). `key` identifies it in
    /// the texture cache; history pages use negative keys so they can never
    /// collide with a live page index.
    void uploadRgba(const std::vector<uint8_t>& rgba, int w, int h, int key);
    bool savePagePng(const ImageWriter::Page& p, const std::string& path,
                     std::string& err);
};

} // namespace pom2

#endif // POM2_IMAGEWRITER_IMGUI_H
