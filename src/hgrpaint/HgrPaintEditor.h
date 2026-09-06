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

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// HgrPaintEditor — an Apple II hi-res (HGR) paint window. Draws straight into
// the host's live HGR video RAM (via IHgrPaintHost::pokeByte) so strokes appear
// on the host screen in real time, and renders its own canvas through the host's
// NTSC pipeline (IHgrPaintHost::renderHgrPage) so what you paint is pixel-
// identical to what the emulator shows.
//
// Emulator-agnostic: depends on ImGui, GL, IconsFontAwesome6, the IHgrPaintHost
// seam, and the sibling hgrpaint/ image-import modules (HgrConvert / Cam16 /
// HgrImageDecode, which need an external stb_image decoder on the include path —
// see IHgrPaintHost.h). Copy hgrpaint/ verbatim into POM2 and supply a host. The
// pure bit-plotting model lives in HgrPaintModel.h (unit-tested without any
// GL/ImGui dependency).
//
// Concept inspired by fadden's HGRTool (hgrtool.art, Apache-2.0) — independent
// C++/ImGui reimplementation.

#ifndef HGRPAINT_EDITOR_H
#define HGRPAINT_EDITOR_H

#include <cstdint>
#include <string>
#include <vector>

#include "HgrPaintModel.h"
#include "IHgrPaintHost.h"

namespace hgrpaint {

class HgrPaintEditor
{
public:
    explicit HgrPaintEditor(IHgrPaintHost* host);
    ~HgrPaintEditor();

    // Draw the window body (caller wraps in Begin/End). `memory` is the live
    // 64 KB host memory snapshot used as the read source for the canvas +
    // shadow; `aux` is the matching auxiliary-bank snapshot for the DHGR pages
    // (nullptr on hosts without one — the DHGR page entries are then hidden).
    void render(const std::vector<uint8_t>& memory,
                const std::vector<uint8_t>* aux = nullptr);

    // Which HGR page the editor targets (false = page 1 $2000, true = page 2).
    bool targetsPage2() const { return page2; }

    // Session persistence — the host app round-trips this through its own
    // settings store so the editor reopens where it was left. `mode`:
    // 0 = HGR, 1 = GR, 2 = DHGR, 3 = DLGR (the double modes clamp to HGR
    // when the host lacks an aux bank).
    struct Session {
        int mode = 0;
        bool page2 = false;
        int zoomIdx = 2;
        bool ntscColor = true;
        bool aspect43 = false;
        int canvasPipeline = 0;
        std::string browserDir;
    };
    Session session() const;
    void restoreSession(const Session& s);

    // Destroy GPU textures via the host. Call from the app's shutdown path BEFORE
    // the GL context is destroyed (the destructor runs too late). Idempotent.
    void releaseGL();

private:
    // Order is append-only: the toolbar/status name tables and shortcuts index
    // by these values, so new tools go at the end.
    enum class Tool : uint8_t { Pencil = 0, Eraser, Line, Rectangle, Ellipse, Fill,
                                Eyedropper, Select, PaletteShift, Text };
    static constexpr int kToolCount = 10;

    // One byte change: lets us replay forward (redo) or reverse (undo). `addr`
    // is the absolute CPU address, with kAuxFlag set for the DHGR AUX plane
    // (which shares the $2000/$4000 ranges with main).
    static constexpr uint32_t kAuxFlag = 0x10000;
    struct ByteEdit { uint32_t addr; uint8_t oldVal, newVal; };

    // A copied region, stored as LOGICAL colours (not raw bytes) so paste
    // re-snaps column parity correctly at any destination. Two colour spaces:
    // HGR clips carry HgrColor per pixel; GR/DHGR clips carry a 16-colour
    // index (`sixteen` = true; index 0 = black = transparent, mirroring the
    // HGR Black-is-transparent convention). A 16-colour clip pastes into
    // either 16-colour mode (the coordinates just rescale with the grid).
    struct Clip {
        int w = 0, h = 0;
        bool sixteen = false;
        std::vector<HgrColor> px;    // HGR logical colours (sixteen == false)
        std::vector<int8_t> idx;     // 16-colour indices  (sixteen == true)
    };

    IHgrPaintHost* host;            // emulator seam (poke / render / file I/O)

    // Canvas rendering (host NTSC pipeline → local RGBA buffer → opaque
    // texture handle owned by the host's graphics backend — see IHgrPaintHost).
    std::vector<uint32_t> canvasRgba;   // kHiresWidth*kHiresHeight, host-rendered
    void* texture = nullptr;
    bool ntscColor = true;          // false → monochrome preview

    // Editing state. The top-bar selector picks one of eight pages from these
    // flags: HGR ($2000) / HGR2 ($4000) / GR ($0400) / GR2 ($0800) / DHGR /
    // DHGR2 (aux+main hires) / DLGR / DLGR2 (aux+main text — the double modes
    // are only offered when host->supportsDhgr()). The mode flags are mutually
    // exclusive; sixteenMode() groups the three 16-colour modes.
    bool page2 = false;             // false = page 1, true = page 2
    bool grMode = false;            // lo-res GR blocks (40×48)
    bool dhgrMode = false;          // IIe double hi-res (140×192, 16 colours)
    bool dlgrMode = false;          // IIe double lo-res (80×48, 16 colours)
    bool sixteenMode() const { return grMode || dhgrMode || dlgrMode; }
    int  grColor = 15;              // current 16-colour index 0..15 (15 = white)
    Tool tool = Tool::Pencil;
    Tool prevTool = Tool::Pencil;   // restored after a one-shot Eyedropper pick
    HgrColor color = HgrColor::White;
    int brushSize = 1;              // 1..7
    // MacPaint-style 8×8 fill pattern (0 = solid). Tiles in PAGE coordinates,
    // so overlapping strokes join seamlessly; a pattern bit paints the current
    // colour, a clear bit paints black (opaque, like MacPaint). On the NTSC
    // pipelines a two-colour pattern blends into real intermediate tones.
    int patternIdx = 0;
    bool patternOn(int x, int y) const;   // table lives in the .cpp
    // Pattern-gated plot: chromatic plots read the pattern; Black (eraser /
    // RMB-erase) bypasses it.
    void applyPlotPat(int x, int y, HgrColor c) {
        if (c != HgrColor::Black && !patternOn(x, y)) c = HgrColor::Black;
        applyPlot(x, y, c);
    }
    // Zoom ladder index (kZoomLadder[]). Mouse-wheel + Fit drive this.
    int zoomIdx = 2;                // → 3x
    bool showGrid = false;
    bool rectFilled = false;
    bool showConflicts = false;     // palette-seam / DHGR-fringing overlay
    bool aspect43 = false;          // aspect-correct (4:3) canvas display
    bool mirrorX = false;           // symmetric drawing about the vertical axis
    bool mirrorY = false;           // …and about the horizontal axis
    // Onion-skin tracing layer: the imported source kept as a canvas overlay.
    void* onionTex = nullptr;
    bool  onionShow = false;
    float onionAlpha = 0.40f;
    // Overlay placement in logical 280-eq space + source UVs (crop window),
    // computed once when the layer is armed so it aligns with a fit/letterbox
    // or cropped import.
    float onionX0 = 0, onionY0 = 0, onionW = 280, onionH = 192;
    float onionU0 = 0, onionV0 = 0, onionU1 = 1, onionV1 = 1;
    // Flipbook: preview the double-buffer animation by alternating the canvas
    // between this page and its sibling (page 1↔2) at flipHz, and/or ghost the
    // OTHER page over the canvas — the classic way to author page-flipped
    // animation. Editing always targets the selected page.
    bool  flipShow = false;
    float flipHz = 4.0f;
    bool  ghostOther = false;
    float ghostAlpha = 0.35f;
    std::vector<uint8_t>  flipShadow;    // other page's bytes (refreshed per frame)
    std::vector<uint32_t> flipRgba;
    void* flipTex = nullptr;
    bool wantFit = false;           // queued zoom-to-fit
    int  paletteMsbMode = 2;        // PaletteShift sub-mode: 0=clear,1=set,2=toggle

    // Text tool: a caret placed by clicking the canvas, then bbfont glyphs stamped
    // in the current colour. textHomeX is the line-start column (carriage return /
    // word-wrap target). The buffer is the pending string in the tool-options box.
    bool textPlaced = false;
    int  textX = 0, textY = 0, textHomeX = 0;
    char textBuf[256] = {0};
    // Set when the caret is (re)placed by a canvas click so the tool panel grabs
    // keyboard focus into the text box next frame. Without it the host app's key
    // path sees WantTextInput=false and routes typing to the emulated Apple-1
    // keyboard instead of this InputText.
    bool focusTextInput = false;
    // The navigator thumbnail is always shown when the image overflows the
    // viewport, and Save always stamps the POM1HGR screen-hole tag.

    // Selection + clipboard.
    bool hasSel = false;
    int  selX0 = 0, selY0 = 0, selX1 = 0, selY1 = 0;   // normalized on commit
    Clip clip;
    bool pasting = false;           // floating-paste mode active
    int  pasteX = 0, pasteY = 0;    // floating clip top-left (logical)

    // Canvas scroll plumbing for the minimap, which now lives in the LEFT tool
    // panel (drawn before the canvas each frame): renderCanvas captures these,
    // renderMinimap reads them, and a pending scroll is applied at the next
    // canvas draw.
    float canvasScrollX = 0, canvasScrollY = 0;
    float canvasScrollMaxX = 0, canvasScrollMaxY = 0;
    float canvasViewW = 0, canvasViewH = 0;
    float canvasScale = 1.0f;       // screen px per 280-eq column (zoom × aspect)
    float canvasScaleY = 1.0f;      // screen px per scanline (zoom only)
    float pendingScrollX = -1, pendingScrollY = -1;

    // 8 KB shadow of the current page, refreshed from `memory` each frame.
    std::vector<uint8_t> shadow;

    // In-progress operation.
    bool dragging = false;
    bool rmbErase = false;                // right-button quick-erase drag active
    bool panning  = false;                // middle-button canvas pan active
    bool firstFit = true;                 // queue a zoom-to-fit on first open
    int dragStartX = 0, dragStartY = 0;   // for Line/Rectangle/Ellipse
    int lastX = -1, lastY = -1;           // for Pencil/Eraser interpolation
    int lastHoverX = -1, lastHoverY = -1; // persisted for the status bar
    std::vector<ByteEdit> stroke;         // (addr, old, new) edits this op
    // Stroke bracket nesting (see beginStroke/commitStroke). `strokeNest_` is
    // the number of open brackets — only the outermost starts and pushes an
    // undo step; bit i of `strokeBatchMask_` records whether level i opened a
    // host batch, so each commit closes exactly the batch its own begin
    // opened. A single bool here left the host batcher stuck open.
    int      strokeNest_ = 0;
    uint32_t strokeBatchMask_ = 0;

    // Symmetric undo/redo: each entry is one operation's ByteEdit list.
    std::vector<std::vector<ByteEdit>> undo;
    std::vector<std::vector<ByteEdit>> redo;

    char filePath[512] = {0};
    std::string status;

    // File browser popup (Load / Save / Save PNG). Portable: std::filesystem only,
    // so it ports to POM2 with the rest of hgrpaint/. HGR images have no standard
    // extension (e.g. `PIC#062000`), so it lists every file with its byte size and
    // highlights the 8 KB ones rather than filtering by name.
    bool browserOpen = false;          // OpenPopup requested this frame
    bool browserForSave = false;       // false = Load/Import, true = Save
    bool browserImport = false;        // Load mode: import+convert an image (PNG/JPG) → HGR
    int  browserSaveKind = 0;          // 0 = raw 8 KB HGR, 1 = PNG export
    std::string browserDir;            // directory currently shown
    char browserSaveName[256] = {0};   // editable filename (Save mode)
    // Save target awaiting the "this file exists — overwrite?" confirmation
    // (empty = no confirmation pending).
    std::string browserOverwritePath;
    // Image-import (ii-pix-style) options + interactive preview dialog (decode the
    // source once, then live-reconvert as the sliders move).
    bool  importStretch = false;       // false = fit + letterbox (keep aspect)
    bool  importDither  = true;        // error diffusion
    bool  importSerpentine = true;     // greyed out for HGR (NTSC decode is left-to-right only)
    float importDiffusion  = 0.7f;     // diffusion strength (ii-pix HGR error_fraction)
    int   importKernel = 1;            // 0 = Floyd-Steinberg, 1 = Jarvis-mod (HGR default)
    float importBrightness = 1.0f;
    float importContrast   = 1.0f;
    float importGamma      = 1.0f;
    float importColourNoise = 0.30f;   // 0 = vivid colour, 1 = clean black/white greys
    // DHGR import model: 0 = 560-dot analysis-by-synthesis with cross-column
    // refinement (ii-pix "4-pixel colour", the high-quality default); 1 = the
    // fast 140-px aligned block quantiser (Dazzle Draw-style, clean blocks);
    // 2 = 560×192 1-bit monochrome (ii-pix dhr_mono).
    int   importDhgrModel = 0;
    bool  importPreviewOpen = false;   // OpenPopup requested this frame
    bool  importDirty = true;          // reconvert the preview
    bool  importSrcTexDirty = false;   // re-upload the source thumbnail texture
    std::string importSrcName;         // basename, for the status line
    std::vector<uint8_t>  importSrcRgba;   // decoded source image (RGBA)
    int importSrcW = 0, importSrcH = 0;
    // Source crop rectangle (in source pixels) selected interactively over the
    // source thumbnail; degenerate/inactive → whole image is imported.
    bool importCropActive   = false;   // a crop region is set
    bool importCropDragging = false;   // user is dragging a new crop rect
    int  importCropX0 = 0, importCropY0 = 0, importCropX1 = 0, importCropY1 = 0;
    int  importCropAnchorX = 0, importCropAnchorY = 0;   // drag anchor (source px)
    std::vector<uint8_t>  importPage;      // last converted 8 KB page
    std::vector<uint32_t> importPreview;   // rendered preview (kHiresWidth*Height)
    void* importPreviewTex = nullptr;
    void* importSrcTex = nullptr;          // source thumbnail (side-by-side preview)

    uint16_t baseAddr() const {
        if (grMode || dlgrMode) return page2 ? 0x0800 : 0x0400;   // text page
        return page2 ? 0x4000 : 0x2000;               // hi-res page (DHGR too)
    }
    // Bytes the current page occupies: 8 KB HIRES, 1 KB lo-res GR text page,
    // 16 KB DHGR pair / 2 KB DLGR pair (aux plane first — the shadow layout).
    int pageBytes() const {
        return dhgrMode ? kDhgrPairSize : dlgrMode ? kDlgrPairSize
             : grMode ? 0x400 : kHiresSize;
    }
    // Bytes of ONE plane of a double mode (0 = single-plane mode).
    int planeBytes() const {
        return dhgrMode ? kHiresSize : dlgrMode ? 0x400 : 0;
    }
    // Logical canvas geometry. DHGR paints 140 fat colour pixels per line;
    // DLGR paints in a 560-dot space (blocks of 7 dots); HGR/GR keep the
    // historical 280-wide space. Each logical pixel is drawn `pxScreenW()`×
    // wider/narrower than an HGR one so the canvas keeps the same on-screen
    // footprint (280·zoom) in every mode.
    int   logicalW()  const { return dhgrMode ? kDhgrWidth
                                   : dlgrMode ? 2 * kHiresWidth : kHiresWidth; }
    int   texW()      const { return (dhgrMode || dlgrMode) ? 2 * kHiresWidth
                                                            : kHiresWidth; }
    float pxScreenW() const { return dhgrMode ? 2.0f : dlgrMode ? 0.5f : 1.0f; }

    // ── Shadow-offset ↔ host-address plumbing (plane-aware) ─────────────────
    // Double-mode shadows are [aux plane][main plane]; HGR/GR are main-only.
    // These map a shadow offset to the ByteEdit address encoding (kAuxFlag for
    // the aux plane) and poke the right plane on the host.
    uint32_t addrOfShadowOff(int off) const {
        const int pb = planeBytes();
        if (pb)
            return off < pb
                ? (kAuxFlag | static_cast<uint32_t>(baseAddr() + off))
                : static_cast<uint32_t>(baseAddr() + off - pb);
        return static_cast<uint32_t>(baseAddr() + off);
    }
    int shadowOffOfAddr(uint32_t addr) const {
        const int pb = planeBytes();
        const int rel = static_cast<int>(addr & 0xFFFF) - baseAddr();
        if (pb) return (addr & kAuxFlag) ? rel : rel + pb;
        return rel;
    }
    void hostPoke(uint32_t addr, uint8_t val) {
        if (!host) return;
        if (addr & kAuxFlag) host->pokeAuxByte(static_cast<uint16_t>(addr), val);
        else                 host->pokeByte(static_cast<uint16_t>(addr), val);
    }
    // Record + poke one shadow byte (assumes shadow[off] already holds newVal).
    void emitShadowEdit(int off, uint8_t oldVal) {
        const uint32_t addr = addrOfShadowOff(off);
        stroke.push_back({addr, oldVal, shadow[off]});
        hostPoke(addr, shadow[off]);
    }

    // Render the shadow page through the host NTSC pipeline into a texW()×
    // kHiresHeight RGBA buffer (host-rendered, what the canvas shows).
    void renderShadow(uint32_t* out, bool mono);

    // Paint helpers (operate on shadow + emit writes + accumulate undo).
    // applyPlot honours the mirror-symmetry toggles (every brush/shape tool
    // inherits them); applyPlotRaw is the single-point primitive used by the
    // region-based operations (paste, text stamp) that must NOT mirror.
    void applyPlot(int x, int y, HgrColor c);
    void applyPlotRaw(int x, int y, HgrColor c);
    // Plot an explicit 16-colour index at logical (x,y) in GR (canvas px →
    // block) or DHGR (colour pixel) — the paste path, which must not go
    // through the current grColor.
    void applyIdxPlot(int x, int y, int colorIndex);
    // True when the clipboard content can paste into the current mode.
    bool clipUsableHere() const {
        return clip.w > 0 && clip.sixteen == sixteenMode();
    }
    // GR (lo-res) variants: map the 280×192 canvas pixel to a 40×48 block. In GR
    // mode applyPlot routes here; HgrColor::Black erases (colour 0), anything else
    // paints the current grColor. grFloodFill floods over equal block colour.
    void applyGrPlot(int x, int y, HgrColor c);
    void grFloodFill(int x, int y, int colorIndex);
    // DHGR variants: paint 140×192 16-colour pixels into the aux+main pair.
    void applyDhgrPlot(int x, int y, HgrColor c);
    void dhgrFloodFill(int x, int y, int colorIndex);
    // DLGR variants: 80×48 blocks (7 dots × 4 px each in the 560-dot space).
    void applyDlgrPlot(int x, int y, HgrColor c);
    void dlgrFloodFill(int x, int y, int colorIndex);
    // Top-bar page selector; mode: 0 = HGR, 1 = GR, 2 = DHGR, 3 = DLGR
    // (Session::mode uses the same encoding).
    void switchPage(int mode, bool toPage2);
    void paintBrush(int cx, int cy, HgrColor c);
    void paintLine(int x0, int y0, int x1, int y1, HgrColor c);
    void paintRect(int x0, int y0, int x1, int y1, HgrColor c, bool filled);
    void paintEllipse(int x0, int y0, int x1, int y1, HgrColor c, bool filled);
    void floodFill(int x, int y, HgrColor c);
    void beginStroke(bool batch = false);   // batch=true coalesces the host pokes
    void commitStroke();
    void applyOps(const std::vector<ByteEdit>& ops, bool forward);
    void doUndo();
    void doRedo();
    void clearPage();
    void handleShortcuts();

    // Selection / clipboard.
    void copySelection(bool cut);
    void pasteFloatingAt(int destX, int destY);   // commit the clip as one stroke
    // Palette-shift tool: flip a whole byte column's high bit.
    void paintPaletteByte(int lx, int ly);
    // Text tool: stamp `text` as bbfont glyphs from the caret, in colour `c`.
    void stampText(const char* text, HgrColor c);

    void renderTopBar();        // page select + file ops + help (MacPaint-style menu strip)
    void renderToolPanel();     // left vertical palette of icon tool buttons + options
    void renderColorBar();      // horizontal colour palette along the bottom
    void openFileBrowser(bool forSave, int saveKind = 0, bool importMode = false);
    // Carry out a Load / Save / Save PNG / Import on `fullPath` (shared by the
    // native picker and the ImGui browser). Returns false only on a failed save
    // (so the ImGui browser keeps its popup open); true otherwise.
    bool performFileAction(bool forSave, int saveKind, bool importMode,
                           const std::string& fullPath);
    void renderFileBrowser();   // modal file picker for Load / Save / Save PNG / Import
    // The path a Save will actually write (raw-page saves append the SD-card
    // "#06AAAA" tag) — what the overwrite check has to look at.
    std::string resolvedSaveTarget(const std::string& full) const;
    void openImportPreview(const std::string& path); // decode + open the interactive preview
    void renderImportPreview();                       // modal: sliders + live preview + apply
    void renderCanvas(const std::vector<uint8_t>& memory,
                      const std::vector<uint8_t>* aux);
    void renderMinimap();       // navigator thumbnail, drawn in the left tool panel
    void renderStatusBar(int lx, int ly, bool hovered);
    void renderFileRow();
};

} // namespace hgrpaint

#endif // HGRPAINT_EDITOR_H
