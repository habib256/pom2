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

// HgrPaintEditor_Dialogs.cpp — the Paint editor's two modal dialogs: the
// in-process file browser (Load / Save / Save PNG / Import, when the host has
// no native picker) with its overwrite questions, and the image-import
// preview (source | result side by side, live sliders, crop, Apply).
//
// Both open at a size in font units, never larger than the screen, and lay
// out to the width they get: the pictures scale down, a long folder or file
// name is elided in its middle (whole in its tooltip), buttons take their
// label's width, and the import preview's buttons stay at the bottom while
// its body scrolls (tests/paint_ui_probe.cpp, 2026-11-28).
//
// Portable (shared with POM1): std::filesystem, ImGui and the host seam.

#include "HgrPaintEditor.h"
#include "HgrConvert.h"          // image -> page conversions
#include "PaintLayout.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

void hgrpaint::HgrPaintEditor::renderFileBrowser()
{
    namespace fs = std::filesystem;
    // Overwrite confirmation for a NATIVE save whose real target is not the
    // name the OS dialog confirmed (see openFileBrowser). Opened here, not
    // where the pick happened: OpenPopup and BeginPopupModal must share an ID
    // stack, and the Save button lives in the top bar.
    if (nativeOverwriteOpen) {
        ImGui::OpenPopup("Overwrite?##nativeoverwrite");
        nativeOverwriteOpen = false;
    }
    if (ImGui::BeginPopupModal("Overwrite?##nativeoverwrite", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The picture is saved under its tagged name, which already exists:");
        ImGui::TextDisabled("%s",
            fs::path(resolvedSaveTarget(browserOverwritePath)).filename().string().c_str());
        ImGui::TextUnformatted("Replace its contents?");
        ImGui::Separator();
        if (ImGui::Button("Overwrite")) {
            performFileAction(true, browserSaveKind, false, browserOverwritePath);
            browserOverwritePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            browserOverwritePath.clear();
            status = "Save cancelled";
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (browserOpen) { ImGui::OpenPopup("HGR File##browser"); browserOpen = false; }

    const ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(layout::dialogSize(44.0f, 33.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(layout::dialogSize(22.0f, 14.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("HGR File##browser", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::TextWrapped("%s", browserForSave
        ? (browserSaveKind == 1 ? "Save PNG export"
           : dhgrMode ? "Save DHGR image (16 KB, A2FC)" : "Save HGR image (8 KB)")
        : (browserImport ? (dhgrMode ? "Import picture (PNG / JPG / BMP) — converted to DHGR"
                            : grMode ? "Import picture (PNG / JPG / BMP) — converted to GR (lo-res)"
                                     : "Import picture (PNG / JPG / BMP) — converted to HGR")
                         : (dhgrMode
                            ? "Load DHGR image (pick a file — 16 KB ones are highlighted)"
                            : "Load HGR image (pick a file — 8 KB ones are highlighted)")));
    layout::textFitted(browserDir, ImGui::GetContentRegionAvail().x);
    ImGui::Separator();

    // ── Directory + file listing ─────────────────────────────────────────────
    // A name too long for the list is elided in its middle (whole in the
    // tooltip); the size is right-aligned after it.
    const float footerRows = (!browserForSave && browserImport) ? 2.0f : 1.0f;
    ImGui::BeginChild("##fblist", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() * footerRows
                                               + ImGui::GetStyle().ItemSpacing.y)),
                      ImGuiChildFlags_Borders);
    const float sizeColW = ImGui::CalcTextSize("99999999 B").x;
    const float nameW = std::max(ImGui::GetContentRegionAvail().x - sizeColW - ImGui::GetStyle().ItemSpacing.x * 2.0f,
                                 ImGui::GetFontSize() * 4.0f);
    if (ImGui::Selectable("../", false)) {
        std::error_code ec;
        fs::path up = fs::path(browserDir).parent_path();
        if (!up.empty()) browserDir = up.string();
        (void)ec;
    }
    std::vector<fs::directory_entry> dirs, files;
    try {
        for (const auto& e : fs::directory_iterator(browserDir,
                 fs::directory_options::skip_permission_denied)) {
            std::error_code ec;
            if (e.is_directory(ec)) dirs.push_back(e);
            else if (e.is_regular_file(ec)) files.push_back(e);
        }
    } catch (...) {
        ImGui::TextDisabled("(cannot read this directory)");
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename().string() < b.path().filename().string();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (const auto& d : dirs) {
        const std::string name = d.path().filename().string() + "/";
        const std::string shown = layout::elideMiddle(name, nameW + sizeColW);
        if (ImGui::Selectable((shown + "##" + name).c_str(), false))
            browserDir = d.path().string();
        if (shown != name && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
    }
    for (const auto& f : files) {
        std::error_code ec;
        const std::uintmax_t sz = f.file_size(ec);
        const std::string name = f.path().filename().string();
        // Highlight the files that suit the current mode: 8 KB raw pages for
        // Load/Save, image files for Import.
        std::string ext = f.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(c));
        const bool isImg = (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
                            ext == ".bmp" || ext == ".gif" || ext == ".tga");
        const bool relevant = browserImport
            ? isImg
            : (!ec && (dhgrMode ? (sz >= 16000 && sz <= 16384)
                     : dlgrMode ? (sz >= 2000 && sz <= 2048)
                     : grMode   ? (sz >= 1000 && sz <= 1024)   // PAINT-4
                                : (sz >= 8000 && sz <= 8192)));
        const std::string shown = layout::elideMiddle(name, nameW);
        char sizeText[32];
        std::snprintf(sizeText, sizeof(sizeText), "%llu B", static_cast<unsigned long long>(sz));
        if (!relevant) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
        const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const bool picked = ImGui::Selectable((shown + "##" + name).c_str(), false);
        if (shown != name && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name.c_str());
        ImGui::SameLine(rowRight - ImGui::CalcTextSize(sizeText).x);
        ImGui::TextUnformatted(sizeText);
        if (picked) {
            if (browserForSave) {
                std::snprintf(filePath, sizeof(filePath), "%s", f.path().string().c_str());
                std::snprintf(browserSaveName, sizeof(browserSaveName), "%s", name.c_str());
            } else {                       // Load or Import: act immediately
                performFileAction(false, browserSaveKind, browserImport, f.path().string());
                ImGui::CloseCurrentPopup();
            }
        }
        if (!relevant) ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // (No import options here: picking a file opens the preview dialog, which
    // exposes Stretch/Dither plus the full live controls — duplicating them in
    // this browser only pre-seeded state the preview immediately supersedes.)
    if (!browserForSave && browserImport)
        layout::textFitted("Pick an image \xE2\x80\x94 crop + dither options open in the preview.",
                           ImGui::GetContentRegionAvail().x);

    // ── Action row: the name takes what the buttons leave ───────────────────
    if (browserForSave) {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - layout::buttonWidth("Save")
                                             - layout::buttonWidth("Cancel") - 2.0f * sp,
                                         ImGui::GetFontSize() * 6.0f));
        ImGui::InputText("##fbname", browserSaveName, sizeof(browserSaveName));
        ImGui::SameLine();
        if (ImGui::Button("Save") && browserSaveName[0]) {
            const std::string full = (fs::path(browserDir) / browserSaveName).string();
            // Ask before destroying somebody's picture. A raw page save may
            // retarget the basename to "NAME#06xxxx", so the file the user
            // would lose is the one performFileAction will actually write —
            // resolve the same name here rather than testing what was typed.
            std::error_code ec;
            if (fs::exists(resolvedSaveTarget(full), ec)) {
                browserOverwritePath = full;
                ImGui::OpenPopup("Overwrite?##fboverwrite");
            } else if (performFileAction(true, browserSaveKind, false, full)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
    }
    const bool cancel = ImGui::Button("Cancel");

    // Overwrite confirmation (stacked on the browser modal, so a "No" comes
    // back to the file list with the name still typed in). Closing the BROWSER
    // has to happen after EndPopup() below — inside the child popup,
    // CloseCurrentPopup() would only close the child.
    bool closeBrowserAfterOverwrite = false;
    if (ImGui::BeginPopupModal("Overwrite?##fboverwrite", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This file already exists:");
        ImGui::TextDisabled("%s",
            fs::path(resolvedSaveTarget(browserOverwritePath)).filename().string().c_str());
        ImGui::TextUnformatted("Replace its contents?");
        ImGui::Separator();
        if (ImGui::Button("Overwrite")) {
            const bool ok = performFileAction(true, browserSaveKind, false,
                                              browserOverwritePath);
            browserOverwritePath.clear();
            ImGui::CloseCurrentPopup();
            if (ok) closeBrowserAfterOverwrite = true;   // closed below, outside this popup
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            browserOverwritePath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (closeBrowserAfterOverwrite || cancel) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void hgrpaint::HgrPaintEditor::openImportPreview(const std::string& path)
{
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    std::string err;
    if (!hgrpaint::decodeImageFile(path, w, h, rgba, err)) {
        status = "Import failed: " + err;
        return;
    }
    importSrcRgba = std::move(rgba);
    importSrcW = w; importSrcH = h;
    importSrcName = std::filesystem::path(path).filename().string();
    importCropActive = false;          // fresh image → import the whole frame
    importCropDragging = false;
    importDirty = true;
    importSrcTexDirty = true;
    importPreviewOpen = true;
}

void hgrpaint::HgrPaintEditor::renderImportPreview()
{
    // Free the (potentially tens-of-MB) decoded source + its full-res GPU texture
    // once the dialog has closed — Apply / Cancel / Esc all land here next frame
    // (popup not open, no pending re-open). swap() reclaims capacity, not just size.
    if (!importPreviewOpen && !importSrcRgba.empty() &&
        !ImGui::IsPopupOpen("Import preview##hgr")) {
        std::vector<uint8_t>().swap(importSrcRgba);
        std::vector<uint8_t>().swap(importPage);
        std::vector<uint32_t>().swap(importPreview);
        if (importSrcTex && host) { host->destroyTexture(importSrcTex); importSrcTex = nullptr; }
        // The preview GPU texture also follows the popup's lifetime — until
        // we destroy it here, the ~210 KB RGBA8 stays resident on the
        // renderer for the rest of the session even though no widget
        // references it. Pair it with importSrcTex so both go together.
        if (importPreviewTex && host) { host->destroyTexture(importPreviewTex); importPreviewTex = nullptr; }
        importSrcW = importSrcH = 0;
        importSrcTexDirty = true;
    }

    if (importPreviewOpen) { ImGui::OpenPopup("Import preview##hgr"); importPreviewOpen = false; importDirty = true; }

    // Sized in font units (it follows the UI zoom) and never larger than the
    // screen; the body scrolls, the buttons stay at the bottom.
    const ImVec2 vpCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(layout::dialogSize(70.0f, 46.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(layout::dialogSize(24.0f, 18.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::BeginPopupModal("Import preview##hgr", nullptr, ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::TextWrapped("%s", dhgrMode
        ? (importDhgrModel == 0
           ? "Image \xE2\x86\x92 DHGR  (560 dots, NTSC sliding window, CAM16-UCS dithering)"
           : importDhgrModel == 1
           ? "Image \xE2\x86\x92 DHGR  (140x192 blocks, 16 colours, CAM16-UCS dithering)"
           : importDhgrModel == 2
           ? "Image \xE2\x86\x92 DHGR mono  (560x192 1-bit, luma dithering)"
           : "Image \xE2\x86\x92 DHGR NTSC  (8-px chroma, 86 colours, composite targets)")
        : dlgrMode
        ? "Image \xE2\x86\x92 DLGR  (80x48 blocks, 16 colours, CAM16-UCS dithering)"
        : grMode
        ? "Image \xE2\x86\x92 GR lo-res  (40x48 blocks, CAM16-UCS perceptual dithering)"
        : "Image \xE2\x86\x92 HGR  (ii-pix: CAM16-UCS perceptual dithering)");
    {
        char src[64];
        std::snprintf(src, sizeof src, "  \xE2\x80\x94  %d x %d source", importSrcW, importSrcH);
        layout::textFitted(importSrcName + src, ImGui::GetContentRegionAvail().x);
    }
    ImGui::Separator();

    // The body scrolls; the footer (Onion skin / Apply / Cancel) stays put.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float footerH = ImGui::GetFrameHeightWithSpacing() + st.ItemSpacing.y + 1.0f;
    ImGui::BeginChild("##importbody", ImVec2(0.0f, -footerH), ImGuiChildFlags_None);

    // The two pictures side by side, at 2x (result 560 x 384, source capped
    // at 360 wide) when there is room, scaled down to the dialog's width
    // and to about 60 % of its height otherwise — never cut.
    // Source-thumbnail dimensions (whole image): cap the long side, but also
    // keep a minimum extent so a pathological aspect ratio (very tall/narrow
    // or very wide source) can't collapse to a sub-pixel, undraggable strip.
    float ph = kHiresHeight * 2.0f;             // result preview height (384 at 1:1)
    float sw = ph * (importSrcH ? static_cast<float>(importSrcW) / importSrcH : 1.0f), sh = ph;
    if (sw > 360.0f) { sw = 360.0f; sh = importSrcW ? sw * importSrcH / importSrcW : ph; }
    float rw = kHiresWidth * 2.0f;
    {
        const float availW = ImGui::GetContentRegionAvail().x - st.ItemSpacing.x - 1.0f;
        const float availH = (ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2.0f) * 0.62f;
        float k = std::min(1.0f, availW / (sw + rw));
        k = std::min(k, std::max(availH, 96.0f) / std::max(ph, sh));
        k = std::max(k, 0.2f);
        sw *= k; sh *= k; rw *= k; ph *= k;
    }
    sw = std::max(sw, 48.0f);
    sh = std::max(sh, 48.0f);

    // Click-vs-crop discriminator measured in SCREEN pixels, not source pixels, so
    // sensitivity doesn't scale with source resolution (a 1px jitter on a 4000px
    // image no longer becomes an accidental crop). Shared by the reconvert gate,
    // the release commit, and the dimming overlay so they always agree.
    auto cropUsable = [&]() {
        if (importSrcW <= 0 || importSrcH <= 0) return false;
        return (importCropX1 - importCropX0) * sw / importSrcW >= 5.0f &&
               (importCropY1 - importCropY0) * sh / importSrcH >= 5.0f;
    };

    // ── Reconvert + re-render the HGR preview when anything changed ───────────
    if (importDirty && !importSrcRgba.empty()) {
        importDirty = false;
        hgrpaint::ImportOptions opt;
        opt.stretch    = importStretch;
        opt.dither     = importDither;
        // HGR conversion follows the NTSC decode from left to right;
        // cross-byte refinement handles the seams after error diffusion.
        opt.diffusion  = importDiffusion;
        opt.brightness = importBrightness;
        opt.contrast   = importContrast;
        opt.gamma      = importGamma;
        opt.chromaWeight = 6.0f - importColourNoise * 5.2f;   // 0→6 clean, 1→0.8 vivid
        opt.kernel = importKernel ? hgrpaint::DitherKernel::JarvisMod
                                  : hgrpaint::DitherKernel::FloydSteinberg;
        // Apply the crop both when committed AND while dragging (importCropActive is
        // only set on release), so the FIRST crop drag shows a live cropped preview
        // instead of the uncropped page. A sub-minimum rect falls through to whole.
        if ((importCropActive || importCropDragging) && cropUsable()) {
            opt.cropX0 = importCropX0; opt.cropY0 = importCropY0;
            opt.cropX1 = importCropX1; opt.cropY1 = importCropY1;
        }
        // GR (lo-res) and DHGR have no NTSC coupling in the block model — flat
        // grids of 16-colour cells — so they use the simple CAM16-UCS block
        // quantisers. 280-HIRES keeps the ii-pix analysis-by-synthesis path.
        importPage.assign(static_cast<size_t>(pageBytes()), 0);
        if (dhgrMode && importDhgrModel == 0)
            hgrpaint::imageToDhgrPage560(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode && importDhgrModel == 2)
            hgrpaint::imageToDhgrMonoPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode && importDhgrModel == 3)
            hgrpaint::imageToDhgrPage560Ntsc(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dhgrMode)
            hgrpaint::imageToDhgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (dlgrMode)
            hgrpaint::imageToDlgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else if (grMode)
            hgrpaint::imageToGrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        else
            hgrpaint::imageToHgrPage(importSrcRgba.data(), importSrcW, importSrcH, opt, importPage.data());
        importPreview.assign(static_cast<size_t>(texW()) * kHiresHeight, 0);
        if (host) {
            if (dhgrMode)
                // The mono model previews through the mono pipeline — its
                // dither patterns would read as artifact-colour confetti in
                // colour.
                host->renderDhgrPage(importPage.data(), importPage.data() + kHiresSize,
                                     importPreview.data(), importDhgrModel == 2);
            else if (dlgrMode)
                host->renderDlgrPage(importPage.data(), importPage.data() + 0x400,
                                     importPreview.data(), false);
            else
                host->renderHgrPage(importPage.data(), importPreview.data(), false, grMode);
            importPreviewTex = host->uploadTexture(importPreviewTex, importPreview.data(),
                                                   texW(), kHiresHeight, /*linear=*/false);
        }
    }

    // ── Upload the source thumbnail once (for the side-by-side view) ──────────
    if (importSrcTexDirty && !importSrcRgba.empty() && host) {
        importSrcTexDirty = false;
        importSrcTex = host->uploadTexture(importSrcTex, importSrcRgba.data(),
                                           importSrcW, importSrcH, /*linear=*/true);
    }

    // ── Side-by-side: source (left) | HGR result (right) — kept at the top ────
    ImGui::BeginGroup();
    layout::textFitted("Source \xE2\x80\x94 drag to crop", sw);
    if (importSrcTex && importSrcW > 0 && importSrcH > 0) {
        const ImVec2 imgPos = ImGui::GetCursorScreenPos();
        ImGui::Image(host->textureToImTexture(importSrcTex), ImVec2(sw, sh));

        // Overlay an invisible button at the same spot so we capture drags
        // without ImGui swallowing them as a window move.
        ImGui::SetCursorScreenPos(imgPos);
        ImGui::InvisibleButton("##cropsrc", ImVec2(sw, sh));
        const bool hov = ImGui::IsItemHovered();

        // Screen <-> source-pixel mapping (the thumbnail shows the whole image).
        auto toSrc = [&](const ImVec2& p, int& sx, int& sy) {
            float fx = (p.x - imgPos.x) / sw, fy = (p.y - imgPos.y) / sh;
            fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
            fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
            sx = static_cast<int>(fx * importSrcW + 0.5f);
            sy = static_cast<int>(fy * importSrcH + 0.5f);
        };
        auto toScreen = [&](int sx, int sy) {
            return ImVec2(imgPos.x + static_cast<float>(sx) / importSrcW * sw,
                          imgPos.y + static_cast<float>(sy) / importSrcH * sh);
        };

        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            importCropDragging = true;
            toSrc(ImGui::GetIO().MousePos, importCropAnchorX, importCropAnchorY);
        }
        if (importCropDragging) {
            int mx, my;
            toSrc(ImGui::GetIO().MousePos, mx, my);
            const int nx0 = std::min(importCropAnchorX, mx);
            const int ny0 = std::min(importCropAnchorY, my);
            const int nx1 = std::max(importCropAnchorX, mx);
            const int ny1 = std::max(importCropAnchorY, my);
            // Reconvert (a full ii-pix pass, tens of ms) ONLY when the rect actually
            // changes — holding the mouse still no longer re-runs the converter.
            if (nx0 != importCropX0 || ny0 != importCropY0 ||
                nx1 != importCropX1 || ny1 != importCropY1) {
                importCropX0 = nx0; importCropY0 = ny0;
                importCropX1 = nx1; importCropY1 = ny1;
                importDirty = true;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                importCropDragging = false;
                // A real drag (>= ~5 screen px) selects a crop; a click clears it.
                importCropActive = cropUsable();
                importDirty = true;
            }
        }

        // Draw the crop rectangle + dim the area outside it. Only dim once the rect
        // is usable, so a not-yet-large-enough drag doesn't dim the whole image
        // while the HGR result still previews uncropped (the two halves agreeing).
        if (importCropActive || importCropDragging) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = toScreen(importCropX0, importCropY0);
            const ImVec2 b = toScreen(importCropX1, importCropY1);
            if (cropUsable()) {
                const ImVec2 tl(imgPos.x, imgPos.y), br(imgPos.x + sw, imgPos.y + sh);
                const ImU32 dim = IM_COL32(0, 0, 0, 110);
                dl->AddRectFilled(tl, ImVec2(br.x, a.y), dim);            // above
                dl->AddRectFilled(ImVec2(tl.x, b.y), br, dim);            // below
                dl->AddRectFilled(ImVec2(tl.x, a.y), ImVec2(a.x, b.y), dim);  // left
                dl->AddRectFilled(ImVec2(b.x, a.y), ImVec2(br.x, b.y), dim);  // right
            }
            dl->AddRect(a, b, IM_COL32(255, 215, 0, 255), 0.0f, 0, 2.0f);
        }
    }
    // The column is the thumbnail's width: its captions elide, never widen it.
    if (importCropActive) {
        char crop[64];
        std::snprintf(crop, sizeof crop, "Crop: %d,%d  %dx%d", importCropX0, importCropY0,
                      importCropX1 - importCropX0, importCropY1 - importCropY0);
        layout::textFitted(crop, sw, false);
        if (ImGui::SmallButton("Clear crop")) {
            importCropActive = false; importCropDragging = false; importDirty = true;
        }
    } else {
        layout::textFitted("Crop: whole image", sw);
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled(dhgrMode ? "DHGR result" : dlgrMode ? "DLGR result"
                        : grMode ? "GR result" : "HGR result");
    if (importPreviewTex && host)
        ImGui::Image(host->textureToImTexture(importPreviewTex),
                     ImVec2(rw, ph));
    ImGui::EndGroup();
    ImGui::Separator();

    // ── Live controls (any change re-converts the preview next frame) ─────────
    // The sliders leave room for their longest label, measured.
    const float labelW = ImGui::CalcTextSize("Diffusion (grain)").x + st.ItemInnerSpacing.x;
    const float ctrlW = -std::max(labelW, 1.0f);
    ImGui::SetNextItemWidth(ctrlW);
    importDirty |= ImGui::SliderFloat("Colour noise", &importColourNoise, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Left: flat greys dither clean black/white.\n"
                          "Right: more vivid colour (and more colour speckle).");
    ImGui::SetNextItemWidth(ctrlW);
    importDirty |= ImGui::SliderFloat("Brightness", &importBrightness, 0.3f, 2.0f, "%.2f");
    ImGui::SetNextItemWidth(ctrlW);
    importDirty |= ImGui::SliderFloat("Contrast", &importContrast, 0.4f, 2.5f, "%.2f");
    ImGui::SetNextItemWidth(ctrlW);
    importDirty |= ImGui::SliderFloat("Gamma", &importGamma, 0.4f, 2.5f, "%.2f");
    ImGui::SetNextItemWidth(ctrlW);
    importDirty |= ImGui::SliderFloat("Diffusion (grain)", &importDiffusion, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Error-diffusion strength.\n"
                          "1 = full dithering/grain, lower = smoother but flatter.\n"
                          "0.7 is ii-pix's HGR default.");
    ImGui::SetNextItemWidth(ctrlW);
    {
        const char* kKernelNames[] = { "Floyd-Steinberg", "Jarvis-mod" };
        importDirty |= ImGui::Combo("Diffusion kernel", &importKernel, kKernelNames, 2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Jarvis-mod pushes the error 4 pixels forward - the same\n"
                              "reach as an NTSC sliding-window transition (ii-pix's HGR\n"
                              "choice, smoother). Floyd-Steinberg is the classic grain.");
    }
    if (dhgrMode) {
        static const char* kDhgrModels[] = {
            "560 dots (lookahead)", "140 px blocks (Dazzle Draw)",
            "560 mono (1-bit)", "560 NTSC 8-px (composite)" };
        ImGui::SetNextItemWidth(ctrlW);
        if (ImGui::Combo("DHGR model", &importDhgrModel, kDhgrModels, 4))
            importDirty = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "560 dots: every dot chosen through the real NTSC sliding-window\n"
                "decode + cross-column refinement (ii-pix model) - full resolution,\n"
                "fringing avoided/exploited. Slower.\n"
                "140 px: independent 4-dot colour blocks (Dazzle Draw style) -\n"
                "instant, clean blocks that are easy to retouch, but half the\n"
                "resolution and fringing at colour seams.\n"
                "560 mono: 1-bit luma dither for monochrome displays.\n"
                "560 NTSC 8-px: scores against the 86 colours the 8-dot chroma\n"
                "bleed produces - pick a composite canvas pipeline (View\n"
                "menu) to preview it faithfully.");
        if (importDhgrModel == 3 && host && host->canvasPipeline() < 4) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("NTSC 8-px targets composite displays - switch the "
                               "canvas pipeline (View menu) to AppleWin NTSC or "
                               "OE composite so this preview shows the real colours.");
            ImGui::PopStyleColor();
        }
    }
    layout::Flow opts;
    opts.next(layout::checkboxWidth("Dither"));
    importDirty |= ImGui::Checkbox("Dither", &importDither);
    // Serpentine is deprecated for HGR (the converter ignores it): the NTSC
    // half-dot carry + sliding window are physically left-to-right, so a
    // right-to-left dither row would score against a fabricated left word. The
    // refinement passes fix the diagonal smear it used to hide.
    opts.next(layout::checkboxWidth("Serpentine"));
    ImGui::BeginDisabled(true);
    ImGui::Checkbox("Serpentine", &importSerpentine);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Not applicable to HGR: the NTSC decode is physically\n"
                          "left-to-right, so alternating the scan direction would\n"
                          "dither against a fabricated left context.");
    opts.next(layout::checkboxWidth("Stretch"));
    importDirty |= ImGui::Checkbox("Stretch", &importStretch);
    opts.next(layout::buttonWidth("Reset"));
    if (ImGui::Button("Reset")) {
        importColourNoise = 0.30f; importBrightness = 1.0f; importContrast = 1.0f;
        importGamma = 1.0f; importDiffusion = 0.7f; importDither = true;
        importSerpentine = true; importStretch = false; importKernel = 1;
        importDirty = true;
        importCropActive = false; importCropDragging = false;
    }
    ImGui::EndChild();
    ImGui::Separator();

    // Arm the imported source as an onion-skin tracing layer over the canvas.
    // Placement mirrors the resampler's fit/letterbox in visual 280-eq space
    // (and the crop window becomes the texture UVs) so the overlay lines up
    // with what Apply would put on the page.
    layout::Flow footer;
    footer.next(layout::buttonWidth("Onion skin"));
    if (ImGui::Button("Onion skin") && !importSrcRgba.empty() && host) {
        int cx0 = 0, cy0 = 0, cx1 = importSrcW, cy1 = importSrcH;
        if ((importCropActive || importCropDragging) && cropUsable()) {
            cx0 = importCropX0; cy0 = importCropY0;
            cx1 = importCropX1; cy1 = importCropY1;
        }
        const float cw = static_cast<float>(cx1 - cx0), ch = static_cast<float>(cy1 - cy0);
        if (importStretch) {
            onionX0 = 0; onionY0 = 0; onionW = 280; onionH = 192;
        } else {
            const float s = std::min(280.0f / cw, 192.0f / ch);
            onionW = cw * s; onionH = ch * s;
            onionX0 = (280.0f - onionW) * 0.5f;
            onionY0 = (192.0f - onionH) * 0.5f;
        }
        onionU0 = static_cast<float>(cx0) / importSrcW;
        onionV0 = static_cast<float>(cy0) / importSrcH;
        onionU1 = static_cast<float>(cx1) / importSrcW;
        onionV1 = static_cast<float>(cy1) / importSrcH;
        onionTex = host->uploadTexture(onionTex, importSrcRgba.data(),
                                       importSrcW, importSrcH, /*linear=*/true);
        onionShow = true;
        status = "Onion skin armed: " + importSrcName;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Keep this source as a tracing overlay on the canvas\n"
                          "(show / hide + opacity in the View menu).");
    footer.next(layout::buttonWidth("Apply to page"));
    if (ImGui::Button("Apply to page") && !importPage.empty()) {
        beginStroke(true);
        // Bound by pageBytes(), not shadow.size(): in GR mode baseAddr() is $0400
        // and the live page is only 0x400 bytes (the importer fills just those),
        // so iterating the full 0x2000 shadow would poke zeros past the page — see
        // the same guard on the save path.
        const int limit = std::min({pageBytes(), static_cast<int>(shadow.size()),
                                    static_cast<int>(importPage.size())});
        for (int off = 0; off < limit; ++off) {
            // Text pages: never touch the screen holes (per plane in DLGR).
            if ((grMode || dlgrMode) && hgrpaint::grIsScreenHole(off & 0x3FF)) continue;
            if (importPage[off] == shadow[off]) continue;
            const uint8_t old = shadow[off];
            shadow[off] = importPage[off];
            emitShadowEdit(off, old);
        }
        commitStroke();
        status = "Imported " + importSrcName;
        ImGui::CloseCurrentPopup();
    }
    footer.next(layout::buttonWidth("Cancel"));
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
