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

#include "DiskLibrary_ImGui.h"

#include "IconsFontAwesome6.h"
#include "Pom2Theme.h"   // palette() for the favourite star + mounted marker
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace pom2 {

namespace {

namespace fs = std::filesystem;

// 5.25" extensions: .dsk .do .po(143360) .nib .woz .d13. We do NOT size-gate
// on the .po here — the dedicated 3.5" scanner sees the 800K bucket. `.d13`
// is the 13-sector (DOS 3.1/3.2/3.2.1) raw image (35×13×256 = 116480 B).
// `ext` arrives already lower-cased from rescanInto.
// NOTE on `.2mg`: classifyDiskForSlot classifies that envelope by PARSING its
// header, which these filters cannot afford — they run over every file of a
// directory scan. The size rules below stay a deliberate cheap approximation
// of it; the other extensions mirror it exactly.
bool accept525(const std::string& ext, uint64_t sz) {
    // An 800K `.dsk` is a Sony 3.5" payload, not a 5.25" one — accept35
    // takes it, matching classifyDiskForSlot.
    if (ext == ".dsk" && sz == 819200) return false;
    if (ext == ".dsk" || ext == ".do" || ext == ".nib" || ext == ".woz"
        || ext == ".d13")
        return true;
    if (ext == ".po") {
        // 143 360 = 35 tracks × 16 sectors × 256 B = stock 5.25" ProDOS.
        return sz == 143360 || sz == 143360 + 64; // raw or 2IMG-wrapped
    }
    // 2IMG-wrapped 5.25" floppy (common Asimov format). Anything .2mg
    // below the 800K Sony size is a 5.25" candidate — the DiskImage
    // loader validates the payload precisely. Mirrors classifyDiskForSlot.
    if (ext == ".2mg" && sz < 819200) return true;
    return false;
}

bool accept35(const std::string& ext, uint64_t sz) {
    // A `.woz` is FLUX: its file size is a property of the dump, not of the
    // payload, so it cannot be sniffed by size and is offered to BOTH bays.
    // The loaders sort it out and say which they wanted — `Disk35Image`
    // refuses a 5.25" WOZ by name, `DiskImage` refuses a 3.5" one.
    if (ext == ".woz") return true;
    // Disk35Image takes a bare 800K payload under .po, .dsk and .image alike.
    if ((ext == ".dsk" || ext == ".image") && sz == 819200) return true;
    if (ext != ".po" && ext != ".2mg") return false;
    // 800 K = 1600 × 512 = 819 200. 2IMG envelope adds ≤ 4 KB.
    return sz == 819200 || sz == 819200 + 64
        || (sz > 819200 && sz < 819200 + 4096);
}

bool acceptHdv(const std::string& ext, uint64_t sz) {
    // `.hdv` is UNAMBIGUOUSLY a hard-disk volume at ANY valid 512-aligned
    // size, exactly 800K included (1600 blocks, e.g. AppleWorks_AW.hdv).
    // The old `sz > 819200` bound hid those from every Library tab — they
    // fail accept525 and accept35 too — while drag-drop and the kiosk scan
    // (both classifyDiskForSlot) mounted and booted them fine.
    if (ext == ".hdv")
        return sz >= 512 &&
               ((sz % 512 == 0) || (sz > 64 && (sz - 64) % 512 == 0));
    // A `.2mg` IS ambiguous, so it only reaches the HDV tab above 800K —
    // accept35 has already claimed the ones at or below it.
    if (ext != ".2mg") return false;
    if (sz <= 819200) return false;
    return (sz % 512 == 0) || ((sz - 64) % 512 == 0);
}

// The Floppy Emu's SD card is not one medium: the device emulates 5.25",
// 3.5", UniDisk and Smartport HD, so its folder legitimately holds all of
// them side by side. Accept anything the three tabs above would, WITHOUT
// their size sniffing — the sniff exists to route a file to the right bay,
// and here the bay is decided per click by the file's own classification.
bool acceptFloppyEmu(const std::string& ext, uint64_t sz) {
    (void)sz;
    return ext == ".dsk" || ext == ".do"  || ext == ".po"  || ext == ".nib"
        || ext == ".woz" || ext == ".d13" || ext == ".2mg" || ext == ".hdv";
}

// Case-insensitive substring scan — `needle` should already be lower-
// cased. ASCII-only, which is fine for filenames in this scope.
bool containsCi(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(),
                          needleLower.begin(), needleLower.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

std::string fmtSize(uint64_t sz) {
    char buf[32];
    if (sz < 1024u) {
        std::snprintf(buf, sizeof(buf), "%5llu B", (unsigned long long)sz);
    } else if (sz < 1024u * 1024u) {
        std::snprintf(buf, sizeof(buf), "%5.1f KB", static_cast<double>(sz) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%5.1f MB",
                      static_cast<double>(sz) / (1024.0 * 1024.0));
    }
    return buf;
}

std::string fmtDate(std::time_t t) {
    if (t == 0) return "?";
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

} // anon namespace

void DiskLibrary_ImGui::rescanInto(
    std::vector<Entry>&              out,
    const std::vector<const char*>&  roots,
    bool (*acceptExtAndSize)(const std::string&, uint64_t))
{
    out.clear();
    std::error_code ec;
    for (const char* dir : roots) {
        if (!fs::is_directory(dir, ec)) continue;
        const fs::path root(dir);
        for (auto it = fs::recursive_directory_iterator(root,
                 fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const auto& de = *it;
            const std::string name = de.path().filename().string();
            // Skip dotfiles + dotdirs (.git, .DS_Store, …).
            if (!name.empty() && name.front() == '.') {
                if (de.is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (!de.is_regular_file(ec)) continue;
            // Lower-case the extension so MAJUSCULE dumps (DOS32PLS.D13,
            // DOS13SEC.DSK) match the accept predicates' lower-case literals.
            std::string ext = de.path().extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto sz = static_cast<uint64_t>(de.file_size(ec));
            if (ec) continue;
            if (!acceptExtAndSize(ext, sz)) continue;

            Entry e;
            e.displayName = fs::relative(de.path(), root, ec).string();
            if (e.displayName.empty()) e.displayName = name;
            e.fullPath    = de.path().string();
            e.sizeBytes   = sz;
            // The notch: no write bit for anyone → the loaders mount it
            // write-protected (MediaNotch.h). One status() per file, at scan.
            {
                const auto perms = de.status(ec).permissions();
                e.readOnly = !ec && (perms & (fs::perms::owner_write |
                                              fs::perms::group_write |
                                              fs::perms::others_write))
                                        == fs::perms::none;
            }
            // mtime → time_t via filesystem's clock cast.
            const auto ftime = de.last_write_time(ec);
            if (!ec) {
                const auto sctp =
                    std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - decltype(ftime)::clock::now()
                              + std::chrono::system_clock::now());
                e.mtime = std::chrono::system_clock::to_time_t(sctp);
            }
            out.push_back(std::move(e));
        }
        if (!out.empty()) break;     // first existing root wins
    }
}

void DiskLibrary_ImGui::rescan()
{
    rescanInto(disk525_,
               { "disks_5.4", "../disks_5.4", "../../disks_5.4" },
               &accept525);
    rescanInto(disk35_,
               { "disks_3.5", "../disks_3.5", "../../disks_3.5",
                 "disks_5.4",   "../disks_5.4",   "../../disks_5.4" },
               &accept35);
    rescanInto(hdv_,
               { "hdv", "../hdv", "../../hdv" },
               &acceptHdv);
    rescanInto(floppyEmu_,
               { "floppyemu", "../floppyemu", "../../floppyemu" },
               &acceptFloppyEmu);
    needsRescan_ = false;
}

void DiskLibrary_ImGui::applySort(std::vector<Entry>& entries) const
{
    // Name ascending, always. The size / date orders went away with the sort
    // selector — see the header-row comment in render().
    std::sort(entries.begin(), entries.end(),
        [](const Entry& a, const Entry& b) {
            return a.displayName < b.displayName;
        });
}

bool DiskLibrary_ImGui::passesFilter(const std::string& name) const
{
    if (searchBuf_[0] == '\0') return true;
    std::string needle(searchBuf_);
    for (char& c : needle) c = static_cast<char>(std::tolower(
        static_cast<unsigned char>(c)));
    return containsCi(name, needle);
}

void DiskLibrary_ImGui::on525Left(const std::string& path, Result& r)
{
    r.request525InsertAndBoot = path;
}
void DiskLibrary_ImGui::on525Ctx(const std::string& path, int mountedMask, Result& r)
{
    (void)mountedMask;
    const CurrentlyMounted* m = mounted_;

    // Fallback when the host gave no per-card info: legacy single-target.
    if (!m || m->diskIICards.empty()) {
        if (ImGui::MenuItem("Insert + boot (slot 6 / primary)")) {
            r.request525InsertAndBoot = path;
            r.request525Slot = -1; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Insert only (no boot — hot-swap)")) {
            r.request525InsertOnly = path;
            r.request525Slot = -1; r.request525Drive = 0;
        }
        return;
    }

    // Emit the three mount targets (+ eject) for one DiskII card. drive 1 is
    // bootable; drive 2 is data-only (the boot PROM boots drive 1).
    auto emitCard = [&](const CurrentlyMounted::DiskIICardInfo& card) {
        if (ImGui::MenuItem("Drive 1: insert + boot")) {
            r.request525InsertAndBoot = path;
            r.request525Slot = card.slot; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Drive 1: insert only")) {
            r.request525InsertOnly = path;
            r.request525Slot = card.slot; r.request525Drive = 0;
        }
        if (ImGui::MenuItem("Drive 2: insert only")) {
            r.request525InsertOnly = path;
            r.request525Slot = card.slot; r.request525Drive = 1;
        }
        if (card.drive1 == path || card.drive2 == path) {
            ImGui::Separator();
            if (ImGui::MenuItem("Eject this image")) r.request525EjectPath = path;
        }
    };

    // One card → flat items; several DiskII cards → one submenu per slot.
    if (m->diskIICards.size() == 1) {
        emitCard(m->diskIICards.front());
    } else {
        for (const auto& card : m->diskIICards) {
            char hdr[24];
            std::snprintf(hdr, sizeof(hdr), "Slot %d", card.slot);
            if (ImGui::BeginMenu(hdr)) {
                emitCard(card);
                ImGui::EndMenu();
            }
        }
    }
}
void DiskLibrary_ImGui::on35Left(const std::string& path, Result& r)
{
    r.request35MountAndBoot = path;
    r.request35BootDrive    = 0;
}
void DiskLibrary_ImGui::on35Ctx(const std::string& path, int mountedMask, Result& r)
{
    if (ImGui::MenuItem("Mount on drive 1 + boot")) {
        r.request35MountAndBoot = path;
        r.request35BootDrive    = 0;
    }
    if (ImGui::MenuItem("Mount on drive 1 (no boot)")) {
        r.request35MountOnly    = path;
        r.request35MountDrive   = 0;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Mount on drive 2 + boot")) {
        r.request35MountAndBoot = path;
        r.request35BootDrive    = 1;
    }
    if (ImGui::MenuItem("Mount on drive 2 (no boot)")) {
        r.request35MountOnly    = path;
        r.request35MountDrive   = 1;
    }
    if (mountedMask & 0x3) {
        ImGui::Separator();
        if ((mountedMask & 0x1) && ImGui::MenuItem("Eject from drive 1")) {
            r.request35EjectDrive = 0;
        }
        if ((mountedMask & 0x2) && ImGui::MenuItem("Eject from drive 2")) {
            r.request35EjectDrive = 1;
        }
    }
}
void DiskLibrary_ImGui::onHdvLeft(const std::string& path, Result& r)
{
    r.requestHdvMountAndBoot = path;
}
void DiskLibrary_ImGui::onHdvCtx(const std::string& path, int mountedMask, Result& r)
{
    if (ImGui::MenuItem("Mount + boot")) {
        r.requestHdvMountAndBoot = path;
    }
    if (ImGui::MenuItem("Mount only (no boot)")) {
        r.requestHdvMountOnly = path;
    }
    if (mountedMask & 0x1) {
        ImGui::Separator();
        if (ImGui::MenuItem("Eject")) {
            r.requestHdvEject = true;
        }
    }
}

void DiskLibrary_ImGui::onFloppyEmuLeft(const std::string& path, Result& r)
{
    r.requestFloppyEmuMountAndBoot = path;
}
void DiskLibrary_ImGui::onFloppyEmuCtx(const std::string& path,
                                       int mountedMask, Result& r)
{
    (void)mountedMask;
    if (ImGui::MenuItem("Insert + boot")) {
        r.requestFloppyEmuMountAndBoot = path;
    }
    if (ImGui::MenuItem("Insert only (no boot)")) {
        r.requestFloppyEmuMountOnly = path;
    }
}

void DiskLibrary_ImGui::noteNotch(const std::string& path, bool protect)
{
    for (auto* list : { &disk525_, &disk35_, &hdv_, &floppyEmu_ })
        for (auto& e : *list)
            if (e.fullPath == path) e.readOnly = protect;
}

bool DiskLibrary_ImGui::isFavourite(const std::string& path) const
{
    if (!lists_) return false;
    for (const auto& f : lists_->favourites)
        if (f == path) return true;
    return false;
}

void DiskLibrary_ImGui::renderRow(
    const Entry&      e,
    const char*       nameOverride,
    const RowContext& ctx,
    Result&           r)
{
    ImGui::TableNextRow();
    int mountedMask = 0;
    const auto& markPaths = *ctx.markPaths;
    for (size_t i = 0; i < markPaths.size() && i < 8; ++i) {
        if (!markPaths[i].empty() && markPaths[i] == e.fullPath)
            mountedMask |= (1 << i);
    }
    const bool mounted = mountedMask != 0;
    const bool fav     = isFavourite(e.fullPath);

    ImGui::PushID(e.fullPath.c_str());

    // Name lives in column 0 because ImGui applies tree indentation to the
    // FIRST column only. An earlier cut had a narrow favourite-star column at
    // index 0, which swallowed the whole indent and left every filename flush
    // left regardless of depth — the tree had no readable hierarchy. Star and
    // mounted dot are now inline prefixes instead of their own columns.
    ImGui::TableSetColumnIndex(0);
    // Selectable first, spanning every column, so the whole row is one hit
    // target; AllowOverlap lets the text draw on top of it.
    if (ImGui::Selectable("##row", mounted,
                          ImGuiSelectableFlags_SpanAllColumns |
                          ImGuiSelectableFlags_AllowOverlap)) {
        (this->*ctx.onLeftClick)(e.fullPath, r);
    }
    if (ImGui::BeginPopupContextItem("ctx")) {
        // Favourite toggle lives in the context menu rather than as a clickable
        // star: the row is already a full-span selectable, and an overlapping
        // hit target inside it is a reliable source of mis-clicks — on a panel
        // whose left-click cold-boots the machine, that matters.
        if (ImGui::MenuItem(fav ? "Remove from favourites"
                                : "Add to favourites")) {
            r.toggleFavourite = e.fullPath;
        }
        // The notch, on THIS disk (MediaNotch.h): a sticker over the sleeve's
        // notch, a 3.5" tab, a 2IMG lock — the protection is the disk's, so
        // it is offered on every image, mounted or not, and it travels with
        // the file. Ticked = the host file has no write bit.
        if (ImGui::MenuItem("Write-protected", nullptr, e.readOnly)) {
            r.toggleNotchPath    = e.fullPath;
            r.toggleNotchProtect = !e.readOnly;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", e.readOnly
                ? "Protected: the image file is read-only. Untick to let it be written."
                : "Writable: saves land in the file. Tick to protect it (read-only bit).");
        ImGui::Separator();
        (this->*ctx.onContextMenu)(e.fullPath, mountedMask, r);
        ImGui::EndPopup();
    }
    // Inline prefixes, drawn over the selectable. The mounted marker stays a
    // glyph rather than relying on the row highlight alone — the highlight is
    // also what selection looks like.
    ImGui::SameLine(0.0f, 0.0f);
    if (mounted) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(pom2::palette().warn));
        ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT " ");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
    }
    if (fav) {
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(pom2::palette().accent));
        ImGui::TextUnformatted(ICON_FA_STAR " ");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 0.0f);
    }
    if (e.readOnly) {   // the notch — see the context menu
        ImGui::TextDisabled(ICON_FA_LOCK " ");
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::TextUnformatted(nameOverride ? nameOverride : e.displayName.c_str());
    if (nameOverride && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", e.displayName.c_str());

    if (ctx.showSizeDate) {
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(fmtSize(e.sizeBytes).c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(fmtDate(e.mtime).c_str());
    }

    ImGui::PopID();
}

void DiskLibrary_ImGui::renderTreeNode(const TreeNode& node,
                                       const RowContext& ctx,
                                       Result& r)
{
    // Folders first, then this level's files — the convention every file
    // browser uses, and it keeps the folder headers together where the eye
    // expects them.
    for (const auto& [name, kid] : node.kids) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // The name alone is a safe ImGui ID here: an open TreeNode pushes its
        // own ID scope, so two folders called "Sources" under different parents
        // never collide.
        if (ImGui::TreeNodeEx(name.c_str(),
                              ImGuiTreeNodeFlags_SpanAllColumns |
                              ImGuiTreeNodeFlags_DefaultOpen,
                              ICON_FA_FOLDER " %s", name.c_str())) {
            renderTreeNode(kid, ctx, r);
            ImGui::TreePop();
        }
    }
    for (const Entry* e : node.files) {
        const size_t slash = e->displayName.rfind('/');
        const char*  base  = (slash == std::string::npos)
                           ? e->displayName.c_str()
                           : e->displayName.c_str() + slash + 1;
        renderRow(*e, base, ctx, r);
    }
}

void DiskLibrary_ImGui::renderTab(
    const std::vector<Entry>&        entries,
    const std::vector<std::string>&  markPaths,
    const char*                      emptyHint,
    void (DiskLibrary_ImGui::*onLeftClick)(const std::string&, Result&),
    void (DiskLibrary_ImGui::*onContextMenu)(const std::string&, int, Result&),
    Result&                          r)
{
    // Filter + sort happen on a local copy so toggling sort doesn't
    // mutate the cached scan.
    std::vector<Entry> filtered;
    filtered.reserve(entries.size());
    for (const auto& e : entries) {
        if (passesFilter(e.displayName)) filtered.push_back(e);
    }
    applySort(filtered);

    if (filtered.empty()) {
        ImGui::TextDisabled("%s", emptyHint);
        return;
    }

    const bool searching = searchBuf_[0] != '\0';
    // Tree unless searching: a filtered view wants a flat list of hits with
    // their full paths, not a tree to expand looking for them.
    const bool treeView     = !searching;
    const bool showSizeDate = !(lists_ && lists_->hideSizeDate);

    const RowContext ctx{ &markPaths, onLeftClick, onContextMenu, showSizeDate };

    // Build the nested tree from the relative display names.
    TreeNode root;
    if (treeView) {
        for (const auto& e : filtered) {
            TreeNode* cur = &root;
            const std::string& dn = e.displayName;
            size_t pos = 0;
            for (;;) {
                const size_t slash = dn.find('/', pos);
                if (slash == std::string::npos) break;      // rest is the file
                cur = &cur->kids[dn.substr(pos, slash - pos)];
                pos = slash + 1;
            }
            cur->files.push_back(&e);
        }
    }

    ImGui::BeginChild("##library_table", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const int columns = showSizeDate ? 3 : 1;
    if (ImGui::BeginTable("##library_grid", columns,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (showSizeDate) {
            // Shrunk from 64/80: they were taking a column of prime real estate
            // for the least-used information in the panel. Nobody looks for a
            // disk by its byte count.
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, 72.0f);
        }
        ImGui::TableHeadersRow();

        // ── Pinned sections ───────────────────────────────────────────────
        // Favourites and Recent are drawn from THIS tab's entries only, so a
        // 5.25" favourite never shows up under HDV. Hidden while searching:
        // a filtered view should show matches, not shortcuts.
        auto section = [&](const char* label,
                           const std::vector<std::string>& paths,
                           bool defaultOpen) {
            if (searching || paths.empty()) return;
            std::vector<const Entry*> hits;
            for (const auto& p : paths)
                for (const auto& e : filtered)
                    if (e.fullPath == p) { hits.push_back(&e); break; }
            if (hits.empty()) return;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAllColumns |
                (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            char hdr[64];
            std::snprintf(hdr, sizeof hdr, "%s  (%zu)", label, hits.size());
            if (ImGui::TreeNodeEx(label, f, "%s", hdr)) {
                for (const Entry* e : hits) {
                    // Full relative path here: a shortcut list out of folder
                    // context needs it to be unambiguous.
                    renderRow(*e, nullptr, ctx, r);
                }
                ImGui::TreePop();
            }
        };
        if (lists_) {
            section(ICON_FA_STAR " Favourites", lists_->favourites, true);
            section(ICON_FA_CLOCK_ROTATE_LEFT " Recent", lists_->recents, false);
        }

        // ── Main list ─────────────────────────────────────────────────────
        if (treeView) renderTreeNode(root, ctx, r);
        else for (const auto& e : filtered) renderRow(e, nullptr, ctx, r);

        ImGui::EndTable();
    }
    ImGui::EndChild();
}

DiskLibrary_ImGui::Result DiskLibrary_ImGui::render(
    const char*               title,
    bool&                     open,
    const CurrentlyMounted&   mounted,
    const Lists&              lists)
{
    Result r;
    if (!open) return r;
    mounted_ = &mounted;     // for on525Ctx's per-drive target enumeration
    lists_   = &lists;       // favourites / recents, host-owned

    // No SetNextWindowSize here — the host pre-applies a curated default
    // via SetNextWindowPos/Size (see MainWindow::renderDiskLibraryWindow).
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    if (needsRescan_) rescan();

    // ── Header row: eject-all + refresh + search + sort ───────────────
    // Eject-All lives at the far left of the Library header (moved here
    // from the toolbar) so the one window that mounts disks also unmounts
    // them. Disabled unless something is actually mounted on any path.
    const bool anyMounted =
        !mounted.diskII.empty()        || !mounted.disk35Internal.empty() ||
        !mounted.disk35External.empty() || !mounted.hdv.empty();
    ImGui::BeginDisabled(!anyMounted);
    if (ImGui::Button(ICON_FA_EJECT " Eject All"))
        r.requestEjectAllDisks = true;
    ImGui::EndDisabled();
    if (anyMounted && ImGui::IsItemHovered())
        ImGui::SetTooltip("Eject every loaded Disk II / HDV / SmartPort image");

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE " Refresh")) needsRescan_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-scan disks_5.4/, disks_3.5/, hdv/");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##library_search", "search...",
                             searchBuf_, sizeof(searchBuf_));

    // No sort selector: the panel is a folder tree sorted by name, which is
    // what a file browser is. The Size/Date sorts it used to offer forced a
    // flat list (you cannot group by folder and order by size at the same
    // time), so they were quietly fighting the tree — and the header row is
    // more valuable as space for the search field.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    {
        // Checked = columns visible, matching the label. Storing the
        // host-side flag as "hide" and rendering it as "show" keeps the
        // checkbox reading the way the label says it does.
        bool show = !lists.hideSizeDate;
        if (ImGui::Checkbox("Size/Date", &show))
            r.toggleHideSizeDate = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show the Size and Date columns.\n"
                              "Off gives the name column the full width, "
                              "which matters in a narrow dock.");
    }

    ImGui::Separator();
    ImGui::TextDisabled(
        "left-click = insert + boot      right-click = more options "
        "(incl. favourites)");

    // ── Tabs ───────────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("##library_tabs", ImGuiTabBarFlags_Reorderable)) {
        char tabLabel[64];
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_FLOPPY_DISK " 5.25\"  (%zu)", disk525_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            renderTab(disk525_, mounted.diskII,
                      "  (drop .dsk / .do / .po / .nib / .woz / .d13 into disks_5.4/)",
                      &DiskLibrary_ImGui::on525Left,
                      &DiskLibrary_ImGui::on525Ctx,
                      r);
            ImGui::EndTabItem();
        }
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_FLOPPY_DISK " 3.5\"  (%zu)", disk35_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            std::vector<std::string> marks35 = {
                mounted.disk35Internal, mounted.disk35External };
            renderTab(disk35_, marks35,
                      "  (drop 800K .po / .2mg into disks_3.5/)",
                      &DiskLibrary_ImGui::on35Left,
                      &DiskLibrary_ImGui::on35Ctx,
                      r);
            ImGui::EndTabItem();
        }
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_HARD_DRIVE  " HDV   (%zu)", hdv_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            std::vector<std::string> marksHdv = { mounted.hdv };
            renderTab(hdv_, marksHdv,
                      "  (drop .hdv / .2mg into hdv/)",
                      &DiskLibrary_ImGui::onHdvLeft,
                      &DiskLibrary_ImGui::onHdvCtx,
                      r);
            ImGui::EndTabItem();
        }
        std::snprintf(tabLabel, sizeof(tabLabel),
                      ICON_FA_SD_CARD " Floppy Emu  (%zu)", floppyEmu_.size());
        if (ImGui::BeginTabItem(tabLabel)) {
            // The SD card is not a bay of its own — a click routes the
            // image into a real Disk II / SmartPort / HDV. So "mounted"
            // here means "this SD image is currently in SOME drive",
            // which is exactly the union of the other three tabs' marks.
            std::vector<std::string> marksEmu = mounted.diskII;
            marksEmu.push_back(mounted.disk35Internal);
            marksEmu.push_back(mounted.disk35External);
            marksEmu.push_back(mounted.hdv);
            renderTab(floppyEmu_, marksEmu,
                      "  (the Floppy Emu's SD card — drop any image into floppyemu/)",
                      &DiskLibrary_ImGui::onFloppyEmuLeft,
                      &DiskLibrary_ImGui::onFloppyEmuCtx,
                      r);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    return r;
}

} // namespace pom2
