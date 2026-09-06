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

// MainWindow_Kiosk — everything kiosk mode is, in one translation unit.
//
// Kiosk is a runtime MODE, not a startup flag: the chrome-free render path,
// the gamepad-driven in-game menu (game list, on-screen keyboard, ROM-folder
// manager, directory browser), and the GUI ⇄ full-screen window transition
// that flips between them. The machine is never touched by any of it.
//
// It was ~950 lines spread across MainWindow.cpp and MainWindow_Slots.cpp,
// interleaved with the desktop UI it deliberately replaces. Moved verbatim;
// the two file-scope helpers it owns — the on-screen key grid and the
// kiosk_romdirs.txt path — come with it, and nothing else used them.
//
// The one thing to keep in mind when editing here: a session LAUNCHED with
// `--kiosk` is settings-read-only for its whole life, even after toggling to
// the GUI (`settingsReadOnly()`), because the documented promise is that a
// kiosk run cannot disturb the user's desktop setup. Pinned by `cli_kiosk`.

#include "MainWindow.h"

#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "IconsFontAwesome6.h"
#include "JoystickInput.h"
#include "Logger.h"
#include "MediaMount.h"
#include "Memory.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "StorageCoordinator.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Apple II key grid for the SELECT band. Each cell is an ASCII code the
// running program reads straight from the keyboard latch (queueKey), so no
// make/break bookkeeping is needed (unlike a scancode make/break scheme). Arrows use
// the II's control codes (←=$08 →=$15 ↑=$0B ↓=$0A).
namespace {
struct KioskKey { const char* label; uint8_t ascii; };
const KioskKey kKioskKeys[] = {
    {"1",'1'},{"2",'2'},{"3",'3'},{"4",'4'},{"5",'5'},
    {"6",'6'},{"7",'7'},{"8",'8'},{"9",'9'},{"0",'0'},          // row 0 (10)
    {"Q",'Q'},{"W",'W'},{"E",'E'},{"R",'R'},{"T",'T'},
    {"Y",'Y'},{"U",'U'},{"I",'I'},{"O",'O'},{"P",'P'},          // row 1 (10)
    {"A",'A'},{"S",'S'},{"D",'D'},{"F",'F'},{"G",'G'},
    {"H",'H'},{"J",'J'},{"K",'K'},{"L",'L'},                    // row 2 (9)
    {"Z",'Z'},{"X",'X'},{"C",'C'},{"V",'V'},{"B",'B'},
    {"N",'N'},{"M",'M'},                                        // row 3 (7)
    {"SPACE",' '},{"RET",0x0D},{"ESC",0x1B},
    {"\xe2\x86\x90",0x08},{"\xe2\x86\x91",0x0B},
    {"\xe2\x86\x93",0x0A},{"\xe2\x86\x92",0x15},                // row 4 (7)
};
constexpr int kKioskKeyCount = int(sizeof(kKioskKeys) / sizeof(kKioskKeys[0]));
// Row start/end (half-open) indices — mirrors the layout above.
const int kKioskKeyRows[][2] = { {0,10}, {10,20}, {20,29}, {29,36}, {36,43} };
constexpr int kKioskKeyRowN = int(sizeof(kKioskKeyRows) / sizeof(kKioskKeyRows[0]));
}  // namespace

// ─── Kiosk ROM-folders manager + directory browser ──────────────────────

// Extra ROM folders persist OUTSIDE state.cfg (kiosk keeps its main config
// read-only), in a sibling "kiosk_romdirs.txt" — one absolute path per line.
static std::filesystem::path kioskRomDirsFile(const pom2::Settings& s)
{
    namespace fs = std::filesystem;
    fs::path store = s.getStorePath();
    fs::path dir = store.has_parent_path() ? store.parent_path() : fs::path(".");
    return dir / "kiosk_romdirs.txt";
}

void MainWindow::renderKiosk()
{
    // Chrome-free full-viewport window: just the Apple II screen, centred
    // and letterboxed on a black background. No title bar, no resize, no
    // background decoration — the OS window is already full-screen.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    if (ImGui::Begin("##kiosk", nullptr, flags)) {
        drawScreenImage();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

DiskIICard* MainWindow::kioskBootDiskCard()
{
    // Prefer the conventional boot slot 6; fall back to the primary card.
    for (auto* c : diskIICards()) if (c && c->getSlot() == 6) return c;
    return primaryDiskII();
}

void MainWindow::openKioskStartMenu()
{
    kioskMenuOpen_ = true;
    kioskPage_     = KioskPage::List;
    kioskZone_     = KioskZone::Games;
    kioskActSel_   = 0;
    kioskRescanDisks();
}

// Rebuild the GAMES list from the booted disk's folder + the extra ROM
// folders. Split from openKioskStartMenu so the RomDirs page can refresh the
// list on its way back — otherwise a folder added/removed there is invisible
// until the menu is closed and reopened.
void MainWindow::kioskRescanDisks()
{
    kioskDiskPaths_.clear();
    kioskDiskLabels_.clear();
    kioskDiskSel_ = 0;
    kioskStatus_.clear();

    namespace fs = std::filesystem;
    std::error_code ec;

    DiskIICard* boot = kioskBootDiskCard();
    // Copied UNDER the lock. getDiskPath() hands back a reference into live
    // DiskImage state, and the AI control server's HTTP thread reassigns that
    // very string on /disk and /eject — copy-constructing from it while its
    // heap buffer is being freed is a garbage path at best and a segfault at
    // worst. renderStatusBar already snapshots for exactly this reason.
    std::string cur;
    if (boot) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        cur = boot->getDiskPath(0);
    }

    // Scan the booted disk's own folder PLUS every configured extra ROM
    // folder. Unlike the old build we do NOT filter out unrelated titles:
    // we keep every mountable 5.25" image and SORT by name-proximity so the
    // current title's other sides float to the top while
    // the rest of the collection stays reachable below.
    std::vector<fs::path> dirs;
    auto addDir = [&](const fs::path& d) {
        if (d.empty() || !fs::is_directory(d, ec)) return;
        const fs::path norm = fs::weakly_canonical(d, ec);
        const fs::path key   = ec ? d : norm;
        for (const auto& e : dirs) if (e == key) return;   // dedup
        dirs.push_back(key);
    };
    if (!cur.empty()) addDir(fs::path(cur).parent_path());
    for (const auto& d : kioskRomDirs_) addDir(fs::path(d));

    if (dirs.empty()) {
        kioskStatus_ = boot ? "No disk folder to browse — add one via ROM folders"
                            : "No Disk II card in this config";
        return;
    }

    auto toLower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    };
    auto commonPrefix = [](const std::string& a, const std::string& b) {
        const size_t n = std::min(a.size(), b.size());
        size_t i = 0;
        while (i < n && a[i] == b[i]) ++i;
        return i;
    };
    // A candidate is a "sibling" of the mounted disk when its stem shares a
    // long common prefix (≥6 chars, ≥half the shorter stem) — the same title's
    // other sides ("… (Side A)" ↔ "… (Side B)"), not every disk in the folder.
    const std::string curName = cur.empty() ? std::string{}
                                            : fs::path(cur).filename().string();
    const std::string curKey  = cur.empty() ? std::string{}
                                            : toLower(fs::path(cur).stem().string());
    auto isSibling = [&](const fs::path& p) {
        if (curKey.empty()) return false;
        const std::string k = toLower(p.stem().string());
        if (k == curKey) return true;
        const size_t pref   = commonPrefix(curKey, k);
        const size_t minLen = std::min(curKey.size(), k.size());
        return pref >= 6 && pref * 2 >= minLen;
    };

    // Accept every image the launcher can route — 5.25", 800K 3.5" and HDV —
    // not just floppies. A 5.25" disk is hot-swapped in place (flip-disk); a
    // 3.5"/HDV is mounted + booted through insertAndBootImage on activation.
    std::vector<fs::path> found;
    for (const auto& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (classifyDiskForSlot(it->path().string()) == DiskSlotClass::Unknown)
                continue;
            found.push_back(it->path());
        }
    }
    // Sort: siblings of the mounted disk first, then alphabetical by filename.
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
        const bool sa = isSibling(a), sb = isSibling(b);
        if (sa != sb) return sa;
        return a.filename().string() < b.filename().string();
    });

    // Mark the disk currently in the boot drive so the list shows a ● next to
    // it and the cursor lands on it. Match canonically: the mounted path may
    // be relative (kiosk launched as `POM2 games/foo.dsk`) while scanned
    // entries come out of canonicalized dirs.
    std::error_code ecCur;
    const fs::path curCanon = cur.empty()
        ? fs::path{} : fs::weakly_canonical(fs::path(cur), ecCur);
    kioskMountedPath_ = cur;
    for (const auto& p : found) {
        const std::string name = p.filename().string();
        const bool isMounted = !cur.empty() &&
            (p.string() == cur ||
             (!ecCur && !curCanon.empty() && p == curCanon));
        if (isMounted) {
            kioskDiskSel_     = int(kioskDiskPaths_.size());
            // Adopt the scanned spelling so the render loop's exact string
            // compare against kioskDiskPaths_ draws the ● marker.
            kioskMountedPath_ = p.string();
        }
        kioskDiskPaths_.push_back(p.string());
        kioskDiskLabels_.push_back(name);
    }

    if (kioskDiskPaths_.empty())
        kioskStatus_ = "No disks found in the scanned folder(s)";

    pom2::log().info("Kiosk", "disk scan: " +
                     std::to_string(kioskDiskPaths_.size()) + " disk(s) across " +
                     std::to_string(dirs.size()) + " folder(s)");
}

void MainWindow::kioskMountSelected()
{
    if (kioskDiskSel_ < 0 || kioskDiskSel_ >= int(kioskDiskPaths_.size())) return;

    const std::string path  = kioskDiskPaths_[kioskDiskSel_];
    const std::string label = kioskDiskLabels_[kioskDiskSel_];

    // 3.5" and HDV volumes are boot media, not swap-in-place floppies: route
    // them into the right card and cold-boot straight away (like the CLI
    // launcher). 5.25" keeps the flip-disk gesture: hot-swap, no reboot.
    if (classifyDiskForSlot(path) != DiskSlotClass::Floppy525) {
        kioskSetPaused(false);          // let the worker run for the boot
        std::string err;
        if (insertAndBootImage(path, err)) {
            kioskMountedPath_ = path;
            kioskMenuOpen_ = false;     // booted → back to the game
        } else {
            kioskStatus_ = "Boot failed: " + err;
        }
        return;
    }

    DiskIICard* boot = kioskBootDiskCard();
    if (!boot) { kioskStatus_ = "No Disk II card in this config"; return; }

    // Two-phase: the file read happens here, unlocked; MediaMount takes the
    // lock only to swap the finished image in. In kiosk the window has no
    // other affordance, so a stall would look exactly like a hang.
    std::string mountErr;
    const bool ok = pom2::mountDiskII(*controller, *boot, 0, path, mountErr);
    if (ok) {
        // Keep the menu open so the user can chain a Restart (reboot on the
        // just-mounted disk) without reopening; B / Start dismisses it.
        kioskMountedPath_ = path;
        kioskStatus_ = "Mounted " + label + " — pick RESTART to reboot on it";
    } else {
        kioskStatus_ = "Mount failed: " + boot->getLastError(0);
    }
}

void MainWindow::kioskActivateFocused()
{
    // GAMES zone → mount the highlighted disk in place (no reboot).
    if (kioskZone_ == KioskZone::Games) {
        kioskMountSelected();
        return;
    }

    // ACTIONS zone → 0 Restart · 1 Keyboard · 2 ROM folders ·
    //                3 Exit kiosk · 4 Quit.
    switch (kioskActSel_) {
        case 0: {   // Restart — reboot on whatever disk is now in the drive
            DiskIICard* boot = kioskBootDiskCard();
            kioskSetPaused(false);          // let the worker run for the boot
            if (boot) controller->bootFromSlot(boot->getSlot());
            else      controller->coldBoot();
            controller->setMode(EmulationController::Mode::Running);
            kioskMenuOpen_ = false;
            break;
        }
        case 1:     // Keyboard band — live keys to the running game
            kioskPage_   = KioskPage::Keys;
            kioskKeySel_ = 0;
            kioskSetPaused(false);          // game keeps running under the band
            break;
        case 2:     // ROM folders manager
            if (kioskPruneRomDirs()) kioskSaveRomDirs();
            kioskRomDirSel_ = 0;
            kioskPage_      = KioskPage::RomDirs;
            break;
        case 3:     // Back to the windowed GUI — machine keeps running
                    // (setKioskModeRuntime closes the menu and un-pauses).
            setKioskModeRuntime(false);
            break;
        case 4:     // Quit — ask for confirmation first
            kioskPage_ = KioskPage::Quit;
            break;
    }
}

void MainWindow::kioskInjectSelectedKey()
{
    if (kioskKeySel_ < 0 || kioskKeySel_ >= kKioskKeyCount) return;
    // The band leaves the machine running, so the latch is read live by the
    // program. queueKey masks to 7 bits and sets the strobe like the hardware.
    controller->memory().queueKey(kKioskKeys[kioskKeySel_].ascii);
    kioskStatus_ = std::string("Sent ") + kKioskKeys[kioskKeySel_].label;
}

void MainWindow::kioskSetPaused(bool want)
{
    if (want == kioskPausedByMenu_) return;
    if (want) {
        // Remember whether the machine was ALREADY stopped (user paused it
        // from the GUI toolbar before entering kiosk, or it never started).
        // Without this, closing the menu resumed a machine the user had
        // deliberately paused — the menu's pause is not ours to undo when
        // it was a no-op in the first place.
        kioskPauseWasAlreadyStopped_ =
            controller->getMode() != EmulationController::Mode::Running;
    } else if (kioskPauseWasAlreadyStopped_) {
        kioskPausedByMenu_ = false;      // give the pause back to the user
        return;
    }
    if (!want) {
        // Resuming: while Stopped, the audio thread kept advancing the
        // speaker's reconstruction cursor over silence, so it now sits far
        // ahead of the (frozen) production. Flush it — otherwise the catch-up
        // logic would swallow the game's first sounds for ~the pause duration.
        controller->speaker().reset();
    }
    controller->setMode(want ? EmulationController::Mode::Stopped
                             : EmulationController::Mode::Running);
    kioskPausedByMenu_ = want;
}

void MainWindow::updateKioskMenu()
{
    // Load the persisted extra ROM folders once (feeds the disk scan below).
    if (!kioskRomDirsLoaded_) { kioskLoadRomDirs(); kioskRomDirsLoaded_ = true; }

    // The pad was already polled this frame (pollJoystickAndPushToMemory).
    const JoystickInput::UiNav nav = joystick->uiNav();

    // Keyboard fallbacks work even when the controller isn't a recognized
    // GLFW gamepad (so the gamepad-mapped buttons never fire). They mirror
    // the pad: F1/Start opens the Start menu, K/Select the keyboard band,
    // arrows move, Enter validates, Esc goes back.
    //
    // F1, NOT F10: F10 is the global full-screen ⇄ windowed toggle, so
    // using it here meant entering kiosk ALSO opened this menu in the same
    // frame (onKey runs during glfwPollEvents, before render) — the user
    // asked for the game to go full-screen, not for a menu.
    const bool eStart   = nav.menu    || ImGui::IsKeyPressed(ImGuiKey_F1,     false);
    const bool eSelect  = nav.select  || ImGui::IsKeyPressed(ImGuiKey_K,      false);
    const bool eConfirm = nav.confirm || ImGui::IsKeyPressed(ImGuiKey_Enter,  false);
    const bool eCancel  = nav.cancel  || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    // Left/right zone-swap is a one-shot edge (never auto-repeats).
    const bool eLeft    = nav.left    || ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false);
    const bool eRight   = nav.right   || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);

    // ── SELECT: open/close the keyboard band directly, even mid-game ─────
    // (Back/Select toggles the live keyboard without pausing).
    if (eSelect) {
        if (kioskMenuOpen_ && kioskPage_ == KioskPage::Keys) {
            kioskMenuOpen_ = false;
        } else {
            kioskMenuOpen_ = true;
            kioskPage_     = KioskPage::Keys;
            kioskKeySel_   = 0;
            kioskStatus_.clear();
        }
    }

    // ── START: open/close the Start menu ────────────────────────────────
    if (eStart) {
        if (kioskMenuOpen_ && kioskPage_ != KioskPage::Keys) kioskMenuOpen_ = false;
        else openKioskStartMenu();
    }

    // Pause the machine on every Start-menu page, but NOT the keyboard band
    // (its keys must reach a running game). Closed menu → running.
    const bool wantPause = kioskMenuOpen_ && kioskPage_ != KioskPage::Keys;
    kioskSetPaused(wantPause);
    // Re-park if something else resumed the worker behind the open menu
    // (e.g. an F6 hold released across the menu-open frame ends in
    // rewindEndAndResume → Mode::Running); kioskSetPaused alone early-outs
    // because kioskPausedByMenu_ still says "paused".
    if (wantPause && controller->getMode() != EmulationController::Mode::Stopped)
        controller->setMode(EmulationController::Mode::Stopped);

    if (!kioskMenuOpen_) return;

    // ── Temporal auto-repeat for held directions (400ms delay, 150ms rate) ──
    // The paused loop runs unthrottled, so a per-frame step would be
    // unaimable; gate held-direction steps on the wall clock instead. Edge
    // presses from the keyboard already single-step via IsKeyPressed below,
    // so `step` covers the *held* pad/keys case only.
    const bool upHeld   = nav.upHeld   || ImGui::IsKeyDown(ImGuiKey_UpArrow);
    const bool downHeld = nav.downHeld || ImGui::IsKeyDown(ImGuiKey_DownArrow);
    const bool leftHeld = nav.leftHeld || ImGui::IsKeyDown(ImGuiKey_LeftArrow);
    const bool rightHeld= nav.rightHeld|| ImGui::IsKeyDown(ImGuiKey_RightArrow);
    const bool pgUpHeld = nav.pageUpHeld  || ImGui::IsKeyDown(ImGuiKey_PageUp);
    const bool pgDnHeld = nav.pageDownHeld|| ImGui::IsKeyDown(ImGuiKey_PageDown);
    const bool navHeld  = upHeld || downHeld || leftHeld || rightHeld ||
                          pgUpHeld || pgDnHeld;
    const double tNow = ImGui::GetTime();
    bool step = false;
    if (navHeld) {
        if (!kioskNavHeld_) { step = true; kioskNavHeld_ = true; kioskNavNextT_ = tNow + 0.40; }
        else if (tNow >= kioskNavNextT_) { step = true; kioskNavNextT_ = tNow + 0.15; }
    } else {
        kioskNavHeld_ = false;
    }
    // Resolve directional intents: a fresh keyboard edge OR a repeat `step`.
    const bool up    = ImGui::IsKeyPressed(ImGuiKey_UpArrow,   false) || (step && upHeld);
    const bool down  = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || (step && downHeld);
    const bool left  = step && leftHeld;   // (edge handled separately below)
    const bool right = step && rightHeld;
    const bool pgUp  = ImGui::IsKeyPressed(ImGuiKey_PageUp,   false) || (step && pgUpHeld);
    const bool pgDn  = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || (step && pgDnHeld);

    switch (kioskPage_) {

    case KioskPage::Quit:
        if (eConfirm) { if (window) glfwSetWindowShouldClose(window, 1); kioskMenuOpen_ = false; }
        if (eCancel)  kioskPage_ = KioskPage::List;
        break;

    case KioskPage::Keys: {
        // 2D grid: up/down change row (clamp column), left/right within row.
        if (up || down) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            int col = kioskKeySel_ - kKioskKeyRows[row][0];
            row = (row + (down ? 1 : -1) + kKioskKeyRowN) % kKioskKeyRowN;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            if (col >= len) col = len - 1;
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (left || right || eLeft || eRight) {
            int row = 0;
            for (int r = 0; r < kKioskKeyRowN; ++r)
                if (kioskKeySel_ >= kKioskKeyRows[r][0] && kioskKeySel_ < kKioskKeyRows[r][1]) row = r;
            const int len = kKioskKeyRows[row][1] - kKioskKeyRows[row][0];
            int col = kioskKeySel_ - kKioskKeyRows[row][0] + ((right || eRight) ? 1 : -1);
            col = std::max(0, std::min(len - 1, col));
            kioskKeySel_ = kKioskKeyRows[row][0] + col;
        }
        if (eConfirm) kioskInjectSelectedKey();   // send key, band stays open
        if (eCancel)  kioskMenuOpen_ = false;      // (B) close → resume game
        break;
    }

    case KioskPage::RomDirs: {
        const int total = 1 + int(kioskRomDirs_.size());   // [0]=ADD, [1..]=folders
        if (up || down) {
            kioskRomDirSel_ += down ? 1 : -1;
            kioskRomDirSel_ = (kioskRomDirSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskRomDirSel_ == 0) {                     // + ADD → browser
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path start = (!kioskRomDirs_.empty() &&
                                  fs::is_directory(kioskRomDirs_.back(), ec))
                                     ? fs::path(kioskRomDirs_.back())
                                     : fs::current_path(ec);
                fs::path abs = fs::absolute(start, ec);
                kioskBrowseDir_ = (ec ? start : abs).lexically_normal().string();
                kioskComputeShortcuts();
                kioskScanBrowse(kioskBrowseDir_);
                kioskPage_ = KioskPage::Browse;
            } else {                                        // remove this folder
                const int idx = kioskRomDirSel_ - 1;
                if (idx >= 0 && idx < int(kioskRomDirs_.size()))
                    kioskRomDirs_.erase(kioskRomDirs_.begin() + idx);
                kioskSaveRomDirs();
                if (kioskRomDirSel_ > int(kioskRomDirs_.size()))
                    kioskRomDirSel_ = int(kioskRomDirs_.size());
            }
        }
        if (eCancel) {
            kioskPage_ = KioskPage::List;
            kioskZone_ = KioskZone::Actions;
            kioskRescanDisks();   // pick up folders added/removed just now
        }
        break;
    }

    case KioskPage::Browse: {
        namespace fs = std::filesystem;
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        if (up || down) {
            kioskBrowseSel_ += down ? 1 : -1;
            kioskBrowseSel_ = (kioskBrowseSel_ % total + total) % total;
        }
        if (eConfirm) {
            if (kioskBrowseSel_ == 0) {                     // USE THIS FOLDER
                if (std::find(kioskRomDirs_.begin(), kioskRomDirs_.end(), kioskBrowseDir_)
                        == kioskRomDirs_.end())
                    kioskRomDirs_.push_back(kioskBrowseDir_);
                kioskSaveRomDirs();
                kioskRomDirSel_ = 0;
                kioskPage_ = KioskPage::RomDirs;
            } else if (kioskBrowseSel_ == 1) {              // .. parent
                const fs::path p(kioskBrowseDir_);
                if (p.has_parent_path() && p.parent_path() != p)
                    kioskBrowseDir_ = p.parent_path().string();
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else if (kioskBrowseSel_ < 2 + nShort) {      // shortcut
                kioskBrowseDir_ = kioskBrowseShortcutPaths_[kioskBrowseSel_ - 2];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            } else {                                        // descend
                kioskBrowseDir_ = kioskBrowseSubdirs_[kioskBrowseSel_ - 2 - nShort];
                kioskScanBrowse(kioskBrowseDir_);
                kioskBrowseSel_ = 0;
            }
        }
        if (eCancel) kioskPage_ = KioskPage::RomDirs;
        break;
    }

    case KioskPage::List: default: {
        const int nd = int(kioskDiskPaths_.size());
        // LEFT/RIGHT (edge) swaps focus between the GAMES and ACTIONS zones.
        if (eLeft || eRight)
            kioskZone_ = (kioskZone_ == KioskZone::Games) ? KioskZone::Actions
                                                          : KioskZone::Games;
        if (kioskZone_ == KioskZone::Games) {
            if (nd > 0) {
                constexpr int kPage = 10;   // L1/R1 fast jump size
                int delta = 0;
                if      (down) delta =  1;
                else if (up)   delta = -1;
                else if (pgDn) delta =  kPage;
                else if (pgUp) delta = -kPage;
                if (delta != 0) {
                    kioskDiskSel_ += delta;
                    if (delta == 1 || delta == -1)          // step: wrap
                        kioskDiskSel_ = (kioskDiskSel_ % nd + nd) % nd;
                    else                                    // page jump: clamp
                        kioskDiskSel_ = std::max(0, std::min(nd - 1, kioskDiskSel_));
                }
            }
        } else if (up || down) {                            // ACTIONS column
            kioskActSel_ += down ? 1 : -1;
            kioskActSel_ = (kioskActSel_ % kKioskActionCount + kKioskActionCount)
                           % kKioskActionCount;
        }
        if (eConfirm) kioskActivateFocused();
        if (eCancel)  kioskMenuOpen_ = false;               // (B) resume game
        break;
    }
    }

    // A close on any page must let the machine run again.
    if (!kioskMenuOpen_) kioskSetPaused(false);
}

void MainWindow::renderKioskMenu()
{
    if (!kioskMenuOpen_) return;
    namespace fs = std::filesystem;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 disp = vp->Size;
    const bool keysPage = (kioskPage_ == KioskPage::Keys);

    const ImVec4 kYellow(1.0f, 0.85f, 0.30f, 1.0f);
    const ImVec4 kGreen (0.47f, 0.90f, 0.47f, 1.0f);
    const ImVec4 kDim   (0.50f, 0.50f, 0.50f, 1.0f);
    const ImVec4 kGrey  (0.60f, 0.60f, 0.60f, 1.0f);

    // Full-screen dim veil behind every page EXCEPT the keyboard band (there
    // the game must stay visible so you see keys land).
    if (!keysPage) {
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(disp);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##kioskVeil", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();
    }

    // Geometry: centered panel for full-screen pages, bottom band for keys.
    if (keysPage) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.98f),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.66f, disp.y * 0.34f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.72f);
    } else {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + disp.x * 0.5f, vp->Pos.y + disp.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(disp.x * 0.82f, disp.y * 0.80f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(12, 12, 18, 245));
    }
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(220, 200, 80, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    // The kiosk screen is a full-viewport OPAQUE window — force the menu to
    // the front every frame or it renders hidden behind the black background.
    ImGui::SetNextWindowFocus();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNavInputs;

    ImGui::Begin("##kioskMenu", nullptr, flags);

    auto rowText = [&](bool sel, const ImVec4& col, const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[512]; std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s %s", sel ? "\xe2\x96\xb6" : "  ", buf);
        if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
    };

    // ================= LIST — two zones (games / actions) ================
    if (kioskPage_ == KioskPage::List) {
        const int nd = int(kioskDiskLabels_.size());
        const bool zGames = (kioskZone_ == KioskZone::Games);

        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(kYellow, ICON_FA_GAMEPAD " MENU");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "\xe2\x97\x80\xe2\x96\xb6 switch zone   \xc2\xb7   "
                                  "up/down select   \xc2\xb7   L1/R1 fast   \xc2\xb7   "
                                  "A confirm   \xc2\xb7   B resume   \xc2\xb7   SELECT keyboard");
        ImGui::Separator();

        // Footer reserve = the 4 action rows @2.3 + a "disks found" line @1.3.
        const float sp = ImGui::GetStyle().ItemSpacing.y;
        const float bf = ImGui::GetFontSize();
        // 5.0f = the five action rows @2.3 (Restart / Keyboard / ROM
        // folders / Exit kiosk / Quit). Reserving four clipped the last
        // one below the panel edge — the window is NoScrollbar and a
        // gamepad user cannot scroll to it.
        const float footer = 5.0f * (bf * 2.3f + sp) + (bf * 1.3f + sp) + (sp + 6.0f);

        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextColored(zGames ? kYellow : kDim,
                           zGames ? "\xe2\x96\xb6 " ICON_FA_COMPACT_DISC " GAMES"
                                  : "  " ICON_FA_COMPACT_DISC " GAMES");
        ImGui::BeginChild("##kdList", ImVec2(0, ImGui::GetContentRegionAvail().y - footer), true);
        ImGui::SetWindowFontScale(2.6f);
        if (nd == 0)
            ImGui::TextColored(kDim, "(no disks — add a ROM folder)");
        for (int i = 0; i < nd; ++i) {
            const bool sel = (i == kioskDiskSel_);
            const ImVec4 col = zGames ? kGreen : kDim;
            // ● marks the disk currently in the boot drive (flip-disk anchor).
            const bool mounted = !kioskMountedPath_.empty() &&
                                 kioskDiskPaths_[i] == kioskMountedPath_;
            rowText(sel, col, "%s%s", mounted ? ICON_FA_COMPACT_DISC " " : "",
                    kioskDiskLabels_[i].c_str());
        }
        ImGui::EndChild();

        // Actions column.
        ImGui::SetWindowFontScale(2.3f);
        const bool zAct = (kioskZone_ == KioskZone::Actions);
        auto actionRow = [&](int idx, const ImVec4& col, const char* label) {
            const bool sel = (kioskActSel_ == idx);
            ImGui::PushStyleColor(ImGuiCol_Text, (sel && zAct) ? kGreen : (zAct ? col : kDim));
            ImGui::Text("%s %s", (sel && zAct) ? "\xe2\x96\xb6" : "  ", label);
            ImGui::PopStyleColor();
        };
        actionRow(0, ImVec4(1.0f, 0.60f, 0.15f, 1.0f), ICON_FA_ROTATE " RESTART MACHINE");
        actionRow(1, ImVec4(0.55f, 0.80f, 1.0f, 1.0f), ICON_FA_KEYBOARD " KEYBOARD");
        actionRow(2, ImVec4(0.60f, 0.95f, 0.60f, 1.0f), ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        // Exit to the windowed GUI. Discoverable here because a kiosk user
        // has no menu bar and may not know about Ctrl+Alt+F / F10.
        actionRow(3, ImVec4(0.80f, 0.80f, 1.0f, 1.0f),
                  ICON_FA_COMPRESS " EXIT KIOSK (WINDOWED)");
        actionRow(4, ImVec4(1.0f, 0.50f, 0.40f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT");

        ImGui::SetWindowFontScale(1.3f);
        ImGui::Separator();
        ImGui::TextColored(kYellow, ICON_FA_COMPACT_DISC " Disks found: %d", nd);
        if (!kioskStatus_.empty()) ImGui::TextColored(kGrey, "%s", kioskStatus_.c_str());
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= KEYBOARD band (game runs underneath) ==============
    else if (kioskPage_ == KioskPage::Keys) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(kYellow, ICON_FA_KEYBOARD " KEYBOARD");
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "  A send  \xc2\xb7  B close  \xc2\xb7  game keeps running");
        ImGui::Separator();
        for (int r = 0; r < kKioskKeyRowN; ++r) {
            ImGui::SetWindowFontScale(2.2f);
            for (int i = kKioskKeyRows[r][0]; i < kKioskKeyRows[r][1]; ++i) {
                const bool sel = (i == kioskKeySel_);
                if (i > kKioskKeyRows[r][0]) ImGui::SameLine();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                char cell[24];
                std::snprintf(cell, sizeof cell, sel ? "[%s]" : " %s ", kKioskKeys[i].label);
                ImGui::TextUnformatted(cell);
                if (sel) ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 4));
        }
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= QUIT confirmation =================================
    else if (kioskPage_ == KioskPage::Quit) {
        ImGui::SetWindowFontScale(3.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), ICON_FA_RIGHT_FROM_BRACKET " QUIT?");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 30));
        ImGui::SetWindowFontScale(2.2f);
        ImGui::TextColored(kGreen, ICON_FA_POWER_OFF " (A) Yes, quit");
        ImGui::SetWindowFontScale(1.8f);
        ImGui::TextColored(kGrey, "(B) No, back to menu");
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= ROM folders manager ==============================
    else if (kioskPage_ == KioskPage::RomDirs) {
        ImGui::SetWindowFontScale(2.6f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " ROM FOLDERS");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A add / remove   \xc2\xb7   B back");
        ImGui::Separator();
        ImGui::BeginChild("##kRomDirs", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.2f);
        const int total = 1 + int(kioskRomDirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskRomDirSel_);
            if (i == 0) {
                rowText(sel, kGreen, ICON_FA_PLUS " [ ADD A FOLDER ]");
            } else {
                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%s " ICON_FA_XMARK " %s", sel ? "\xe2\x96\xb6" : "  ",
                            kioskRomDirs_[i - 1].c_str());
                if (sel) { ImGui::PopStyleColor(); ImGui::SetScrollHereY(0.5f); }
            }
        }
        if (kioskRomDirs_.empty()) {
            ImGui::SetWindowFontScale(1.3f);
            ImGui::TextColored(kDim, "   (only the booted disk's folder is scanned)");
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    // ================= Directory browser ================================
    else if (kioskPage_ == KioskPage::Browse) {
        ImGui::SetWindowFontScale(2.4f);
        ImGui::TextColored(kYellow, ICON_FA_FOLDER_OPEN " SELECT ROM FOLDER");
        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextColored(kGrey, "up/down move   \xc2\xb7   A enter / select   \xc2\xb7   B cancel");
        ImGui::Separator();
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "%s", kioskBrowseDir_.c_str());
        ImGui::Separator();
        ImGui::BeginChild("##kBrowse", ImVec2(0, 0), true);
        ImGui::SetWindowFontScale(2.1f);
        const int nShort = int(kioskBrowseShortcutPaths_.size());
        const int total  = 2 + nShort + int(kioskBrowseSubdirs_.size());
        for (int i = 0; i < total; ++i) {
            const bool sel = (i == kioskBrowseSel_);
            if (i == 0)                 rowText(sel, kGreen, ICON_FA_STAR " [ USE THIS FOLDER ]");
            else if (i == 1)            rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " ..");
            else if (i < 2 + nShort)    rowText(sel, kGreen, "%s", kioskBrowseShortcutLabels_[i - 2].c_str());
            else                        rowText(sel, kGreen, ICON_FA_FOLDER_OPEN " %s",
                                                fs::path(kioskBrowseSubdirs_[i - 2 - nShort]).filename().string().c_str());
        }
        ImGui::EndChild();
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(keysPage ? 1 : 2);
}

void MainWindow::kioskScanBrowse(const std::string& dir)
{
    namespace fs = std::filesystem;
    kioskBrowseSubdirs_.clear();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_directory(e2)) kioskBrowseSubdirs_.push_back(it->path().string());
    }
    std::sort(kioskBrowseSubdirs_.begin(), kioskBrowseSubdirs_.end(),
              [](const std::string& a, const std::string& b) {
                  return fs::path(a).filename().string() < fs::path(b).filename().string();
              });
    kioskBrowseSel_ = 0;
}

void MainWindow::kioskComputeShortcuts()
{
    namespace fs = std::filesystem;
    kioskBrowseShortcutPaths_.clear();
    kioskBrowseShortcutLabels_.clear();
    auto add = [&](const std::string& path, const std::string& label) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return;
        if (std::find(kioskBrowseShortcutPaths_.begin(), kioskBrowseShortcutPaths_.end(),
                      path) != kioskBrowseShortcutPaths_.end()) return;   // dedup
        kioskBrowseShortcutPaths_.push_back(path);
        kioskBrowseShortcutLabels_.push_back(label);
    };
    add("/", std::string(ICON_FA_SERVER) + " / (filesystem root)");
    if (const char* home = std::getenv("HOME"))
        add(home, std::string(ICON_FA_FOLDER_OPEN) + " Home");
    // Removable-media mount points (guarded so each OS only shows what exists).
    const char* user = std::getenv("USER");
    std::vector<std::string> roots{ "/Volumes" };
    if (user) { roots.push_back(std::string("/run/media/") + user);
                roots.push_back(std::string("/media/") + user); }
    roots.push_back("/mnt");
    for (const auto& r : roots) {
        std::error_code ec;
        if (!fs::is_directory(r, ec)) continue;
        for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_directory(e2))
                add(it->path().string(),
                    std::string(ICON_FA_HARD_DRIVE) + " " + it->path().filename().string());
        }
    }
}

void MainWindow::kioskLoadRomDirs()
{
    namespace fs = std::filesystem;
    kioskRomDirs_.clear();
    if (!settings) return;
    std::ifstream f(kioskRomDirsFile(*settings));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::error_code ec;
        if (!line.empty() && fs::is_directory(line, ec)) kioskRomDirs_.push_back(line);
    }
}

void MainWindow::kioskSaveRomDirs()
{
    if (!settings) return;
    std::ofstream f(kioskRomDirsFile(*settings), std::ios::trunc);
    if (!f) return;
    for (const auto& d : kioskRomDirs_) f << d << '\n';
}

bool MainWindow::kioskPruneRomDirs()
{
    namespace fs = std::filesystem;
    bool changed = false;
    for (size_t i = 0; i < kioskRomDirs_.size(); ) {
        std::error_code ec;
        if (!fs::is_directory(kioskRomDirs_[i], ec)) {
            kioskRomDirs_.erase(kioskRomDirs_.begin() + long(i));
            changed = true;
        } else ++i;
    }
    return changed;
}

void MainWindow::setKioskMode(bool k)
{
    kiosk_           = k;
    launchedInKiosk_ = k;
    if (k && settings) settings->setReadOnly(true);
}

void MainWindow::setKioskModeRuntime(bool k)
{
    if (k == kiosk_) return;

    if (k) {
        // Entering kiosk. Persist first: kiosk deliberately never writes
        // state.cfg, so anything the user changed in the GUI session would
        // otherwise be lost if they quit from kiosk. (A session LAUNCHED
        // with --kiosk was read-only from the start and stays that way —
        // see settingsReadOnly().)
        if (window) {
#ifdef __EMSCRIPTEN__
            // The browser build must not touch the window/monitor pair at
            // all: Emscripten's GLFW port defines glfwSetWindowMonitor as
            // `abort('glfwSetWindowMonitor not implemented.')` (upstream
            // src/lib/libglfw.js), and an abort() tears the whole module
            // down — the page reports it as a load/init failure and the
            // machine is gone. So the canvas keeps its size and we take the
            // same path as a host with no usable monitor: chrome-free, and
            // nothing else. Real full-screen inside a page belongs to the
            // browser (F11, or the page's own control), not to us.
            pom2::log().info("Kiosk",
                "browser build — chrome-free; canvas size unchanged");
#else
            // Record the windowed geometry to come back to. A MAXIMIZED
            // window reports its maximized size here, so remember the flag
            // separately and re-maximize on the way out — otherwise the
            // user gets an un-maximized window of the maximized size, which
            // most WMs then reposition somewhere unexpected.
            captureWindowGeometryNow();
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (mon && vm) {
                glfwSetWindowMonitor(window, mon, 0, 0,
                                     vm->width, vm->height, vm->refreshRate);
            } else {
                // No monitor info (headless/odd WM): stay windowed but
                // still enter the chrome-free path — the user asked for it.
                pom2::log().warn("Kiosk",
                    "no primary monitor / video mode — kiosk stays windowed");
            }
#endif
        }
        // Persist AFTER measuring, and BEFORE the flag flips: kiosk never
        // writes state.cfg, so this is the last chance to record both the
        // geometry we just captured and anything the user changed in the
        // GUI session. Without it there was nothing to restore from after a
        // quit-from-kiosk, and a --kiosk launch toggling to the GUI got a
        // hard-coded default size instead of the user's real window.
        // `settings` is optional (a MainWindow built without one, as the
        // headless/test paths do): settingsReadOnly() only reports the kiosk
        // flags, so it is no guard against a null here.
        if (settings && !settingsReadOnly()) {
            saveWindowGeometryToSettings();
            settings->save();
        }
        kiosk_ = true;
        if (settings) settings->setReadOnly(true);   // covers every UI save site
        pom2::log().info("Kiosk", "entered (full-screen, chrome-free, "
                                  "settings read-only)");
    } else {
        // Leaving kiosk. Release the host pointer first, before anything
        // touches the window. Kiosk is the mode where a captured pointer is
        // least of a problem (there is no UI to click), and the GUI is the
        // mode where it is most of one: the user comes back to menus, panels
        // and a docked layout, and every one of those needs a real cursor.
        // Doing it here rather than leaving it to the user also avoids a
        // GLFW_CURSOR_DISABLED pointer riding through the full-screen →
        // windowed monitor change, where the OS re-warps it. Entering kiosk
        // deliberately does NOT touch the grab — a captured mouse is what a
        // game in full screen wants.
        setMouseGrab(false);
        // Close the in-kiosk menu next so its captured
        // key handling doesn't leak into the GUI frame — and un-pause: the
        // menu pauses the machine while it is up, and leaving kiosk from an
        // open menu would otherwise strand the user in the GUI with a
        // silently stopped CPU.
        kioskMenuOpen_ = false;
        // Undo only the pause the MENU imposed — kioskSetPaused keeps a
        // user-initiated pause intact (see kioskPauseWasAlreadyStopped_).
        kioskSetPaused(false);
        if (window) {
#ifdef __EMSCRIPTEN__
            // Nothing to restore: entering kiosk never moved the canvas (see
            // the matching guard above), and glfwSetWindowMonitor would
            // abort() the module here exactly as it does there. This is the
            // path the user actually hits — F10 to leave full-screen — so it
            // is the one that used to kill the emulator mid-session.
            pom2::log().info("Kiosk", "browser build — canvas size unchanged");
#else
            if (savedWinW_ > 0) {
                glfwSetWindowMonitor(window, nullptr, savedWinX_, savedWinY_,
                                     savedWinW_, savedWinH_, GLFW_DONT_CARE);
                // Many window managers IGNORE the position/size passed to
                // glfwSetWindowMonitor when leaving full-screen (they just
                // un-fullscreen and keep their own idea of the geometry) —
                // this is the standard GLFW workaround. Harmless when the
                // WM already honoured the call.
                glfwSetWindowSize(window, savedWinW_, savedWinH_);
                glfwSetWindowPos (window, savedWinX_, savedWinY_);
                if (savedWinMaximized_) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window " + std::to_string(savedWinW_) + "x" +
                    std::to_string(savedWinH_) + " at " +
                    std::to_string(savedWinX_) + "," +
                    std::to_string(savedWinY_) +
                    (savedWinMaximized_ ? " (maximized)" : ""));
            } else if (loadWindowGeometryFromSettings()) {
                // Launched with --kiosk: nothing was measured this session,
                // but a previous GUI session persisted its geometry.
                glfwSetWindowMonitor(window, nullptr, savedWinX_, savedWinY_,
                                     savedWinW_, savedWinH_, GLFW_DONT_CARE);
                glfwSetWindowSize(window, savedWinW_, savedWinH_);
                glfwSetWindowPos (window, savedWinX_, savedWinY_);
                if (savedWinMaximized_) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window from settings " +
                    std::to_string(savedWinW_) + "x" +
                    std::to_string(savedWinH_));
            } else {
                // Never ran windowed on this machine: centred default.
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
                const int w = 1280, h = 850;
                const int x = vm ? (vm->width  - w) / 2 : 64;
                const int y = vm ? (vm->height - h) / 2 : 64;
                glfwSetWindowMonitor(window, nullptr, x, y, w, h, GLFW_DONT_CARE);
                glfwSetWindowSize(window, w, h);
                glfwSetWindowPos (window, x, y);
                savedWinX_ = x; savedWinY_ = y; savedWinW_ = w; savedWinH_ = h;
            }
#endif
        }
        kiosk_ = false;
        // A session LAUNCHED with --kiosk stays read-only for life (the
        // documented "can't disturb your desktop setup" promise); a GUI
        // session that merely visited kiosk resumes writing.
        if (settings) settings->setReadOnly(launchedInKiosk_);
        pom2::log().info("Kiosk", "left (windowed, full UI)");
    }
}

bool MainWindow::toggleKioskMode()
{
    setKioskModeRuntime(!kiosk_);
    return kiosk_;
}
