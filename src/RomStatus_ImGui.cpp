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

// RomStatus_ImGui — see the header for why this panel exists.

#include "RomStatus_ImGui.h"

#include "IconsFontAwesome6.h"
#include "Pom2Theme.h"
#include "ResourcePaths.h"
#include "CharRomCatalog.h"
#include "RomCatalog.h"
#include "RomFetch.h"
#include "SystemProfile.h"
#include "ThreadGuard.h"

#include "imgui.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace pom2 {

namespace {

// Plain CRC-32 (IEEE, reflected) — no zlib in POM2's dependency budget and
// the WOZ path only ever writes the "not computed" sentinel, so there was
// no existing implementation to borrow.
const std::array<std::uint32_t, 256>& crcTable()
{
    static const std::array<std::uint32_t, 256> t = [] {
        std::array<std::uint32_t, 256> a{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            a[i] = c;
        }
        return a;
    }();
    return t;
}

ImVec4 colOk()    { return ImVec4(0.42f, 0.80f, 0.45f, 1.0f); }
ImVec4 colWarn()  { return ImVec4(0.95f, 0.72f, 0.25f, 1.0f); }
ImVec4 colBad()   { return ImVec4(0.90f, 0.36f, 0.34f, 1.0f); }

std::string humanSize(std::uint64_t n)
{
    char buf[32];
    if (n >= 1024 && (n % 1024) == 0)
        std::snprintf(buf, sizeof(buf), "%llu KB",
                      static_cast<unsigned long long>(n / 1024));
    else
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(n));
    return buf;
}

}  // namespace

RomStatus_ImGui::~RomStatus_ImGui()
{
    // Ask first, then join. A bare join here made quitting POM2 block behind
    // whatever curl was doing — up to 90 seconds per remaining catalog entry,
    // with the window already gone and nothing on screen to explain it.
    fetchCancel_.store(true);
    if (fetchThread_.joinable()) fetchThread_.join();
}

void RomStatus_ImGui::startRetroBiosFetch()
{
    if (fetchRunning_.load()) return;
    if (fetchThread_.joinable()) {
        fetchThread_.join();
        fetchJoin_.store(false);
        rescan();
    }
    fetchDone_.store(0);
    fetchTotal_.store(0);
    fetchJoin_.store(false);
    fetchCancel_.store(false);
    {
        std::lock_guard<std::mutex> lk(fetchMutex_);
        fetchLine_    = "Starting RetroBIOS download…";
        fetchSummary_.clear();
    }
    fetchRunning_.store(true);
    const fs::path dest = pom2::writableRomsDir();
    fetchThread_ = pom2::guardedThread("RomFetch", [this, dest] {
        // RAII, not a pair of stores at the end of the body: `fetchRunning_`
        // gates the Download button, and anything that threw on the way out
        // (readAll's SIZE_MAX resize did exactly this) left it true forever —
        // the button stayed disabled for the rest of the session and no
        // message anywhere said why. ThreadGuard catches the exception; this
        // makes sure the flag comes down with it.
        struct DoneGuard {
            RomStatus_ImGui* p;
            ~DoneGuard()
            {
                p->fetchRunning_.store(false);
                p->fetchJoin_.store(true);
            }
        } done{this};
        const auto result = pom2::fetchMissingRoms(
            dest,
            [this](int doneN, int total, const char* label) {
                fetchDone_.store(doneN);
                fetchTotal_.store(total);
                std::lock_guard<std::mutex> lk(fetchMutex_);
                if (label && *label)
                    fetchLine_ = std::string(label);
            },
            [this] { return fetchCancel_.load(); });
        {
            std::lock_guard<std::mutex> lk(fetchMutex_);
            fetchSummary_ = result.summary;
            fetchLine_.clear();
        }
    });
}

void RomStatus_ImGui::pollFetchJoin()
{
    if (!fetchJoin_.load()) return;
    if (fetchThread_.joinable()) fetchThread_.join();
    fetchJoin_.store(false);
    rescan();
}

std::uint32_t RomStatus_ImGui::crc32File(const std::string& path,
                                         std::uint64_t& sizeOut)
{
    sizeOut = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    const auto& tbl = crcTable();
    std::uint32_t c = 0xFFFFFFFFu;
    char buf[64 * 1024];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        const std::streamsize n = f.gcount();
        sizeOut += static_cast<std::uint64_t>(n);
        for (std::streamsize i = 0; i < n; ++i)
            c = tbl[(c ^ static_cast<unsigned char>(buf[i])) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

void RomStatus_ImGui::scanProbe(Probe& p)
{
    p.usedIndex = -1;
    for (std::size_t i = 0; i < p.files.size(); ++i) {
        FileState& fsx = p.files[i];
        fsx.resolved = pom2::findResource(fsx.candidate);
        fsx.found    = !fsx.resolved.empty();
        fsx.size     = 0;
        fsx.crc      = 0;
        fsx.sizeOk   = true;
        fsx.crcOk    = false;
        if (!fsx.found) continue;
        fsx.crc    = crc32File(fsx.resolved, fsx.size);
        fsx.sizeOk = (p.requiredSize == 0) ||
                     (fsx.size == static_cast<std::uint64_t>(p.requiredSize));
        // `crcOk` needs the catalogue's reference value, which only the
        // caller has — rescan() fills it in right after this returns.
        if (p.usedIndex < 0) p.usedIndex = static_cast<int>(i);
    }
    p.fallback = (p.usedIndex > 0);
}

void RomStatus_ImGui::rescan()
{
    machine_.clear();
    charRom_.clear();
    locale_.clear();
    cards_.clear();
    sounds_.clear();
    searchRoots_.clear();
    romDir_.clear();
    missingRequired_ = 0;
    missingOptional_ = 0;

    for (const auto& d : pom2::resourceSearchDirs()) {
        searchRoots_.push_back(d.string());
        std::error_code ec;
        if (romDir_.empty() && fs::is_directory(d / "roms", ec)) {
            // Absolute: with several search roots (CWD, exe dir, install
            // prefix) "roms" alone does not say which one won.
            const fs::path abs = fs::absolute(d / "roms", ec);
            romDir_ = ec ? (d / "roms").string() : abs.lexically_normal().string();
        }
    }

    // ── Machine firmware + character generator, straight from the profile
    //    table so this panel can never drift from the real probe order.
    for (SystemProfile sp : pom2::allProfiles()) {
        const auto& cfg = pom2::profileConfig(sp);

        Probe m;
        m.group = "Machine firmware";
        m.name  = std::string(cfg.displayName);
        m.note  = "The profile cannot start: POM2 falls back to whatever "
                  "generic dump resolves, and warns that it may not match.";
        for (const auto& c : cfg.romProbeOrder) m.files.emplace_back(c);
        scanProbe(m);
        if (m.usedIndex < 0) ++missingRequired_;
        machine_.push_back(std::move(m));

        Probe c;
        c.group = "Character generator";
        c.name  = std::string(cfg.displayName);
        c.note  = "Text mode renders from the built-in fallback glyphs.";
        for (const auto& cc : cfg.charRomProbeOrder) c.files.emplace_back(cc);
        scanProbe(c);
        if (c.usedIndex < 0) ++missingOptional_;
        charRom_.push_back(std::move(c));
    }

    // ── Selectable character generators (View → Character set). The
    //    locale catalogue is a SEPARATE list from the per-profile probe
    //    order above: those are what a profile falls back to, these are
    //    what the user can pick by hand, and a missing one silently greys
    //    its dropdown entry. Listing them here is the only place the whole
    //    set is visible.
    for (const auto& e : pom2::charRomCatalog()) {
        if (!e.path || !*e.path) continue;      // ProfileDefault has no file
        Probe p;
        p.group = "International character sets";
        p.name  = e.displayName;
        p.note  = "The entry is offered but cannot be selected — "
                  "View \xe2\x86\x92 Character set falls back to the "
                  "profile default.";
        p.requiredSize = e.isIIeClass ? 4096u : 2048u;
        p.files.emplace_back(e.path);
        scanProbe(p);
        if (p.usedIndex < 0) ++missingOptional_;
        locale_.push_back(std::move(p));
    }

    // ── Floppy mechanical samples. Not ROMs, but the same class of
    //    user-supplied asset with the same failure mode: absent means a
    //    feature silently does nothing, and nothing on screen says why.
    //    The set is MAME's `floppy_sound` samples (imagedev/floppy.cpp),
    //    named `<ff>_<stem>.wav` under roms/floppy_samples/, one bank per
    //    form factor — see FloppySoundDevice::loadSamples.
    {
        static const char* kStems[] = {
            "seek_2ms", "seek_6ms", "seek_12ms", "seek_20ms",
            "spin_empty", "spin_loaded", "spin_start_empty",
            "spin_start_loaded", "spin_end", "step_1_1",
        };
        struct Bank { const char* prefix; const char* label; };
        static const Bank kBanks[] = {
            { "525", "5.25\" drive" }, { "35", "3.5\" drive" },
        };
        for (const Bank& b : kBanks) {
            for (const char* stem : kStems) {
                Probe p;
                p.group = std::string("Floppy sounds — ") + b.label;
                p.name  = stem;
                p.note  = "That mechanical sound is simply not played; the "
                          "rest of the bank still works.";
                p.files.emplace_back(std::string("roms/floppy_samples/") +
                                     b.prefix + "_" + stem + ".wav");
                scanProbe(p);
                if (p.usedIndex < 0) ++missingOptional_;
                sounds_.push_back(std::move(p));
            }
        }
    }

    // ── Peripheral cards, from the catalogue.
    for (const auto& e : pom2::romCatalog()) {
        Probe p;
        p.group        = e.group;
        p.name         = e.name;
        p.note         = e.whenMissing;
        p.crcLabel     = e.knownCrcLabel ? e.knownCrcLabel : "";
        p.requiredSize = e.size;
        for (const char* c : e.candidates) {
            FileState f;
            f.candidate = c;
            f.crcKnown  = (e.knownCrc != 0);
            p.files.push_back(std::move(f));
        }
        scanProbe(p);
        // Re-evaluate the reference CRC now that the bytes are hashed.
        for (auto& f : p.files)
            if (f.found && f.crcKnown) f.crcOk = (f.crc == e.knownCrc);
        if (p.usedIndex < 0) ++missingOptional_;
        cards_.push_back(std::move(p));
    }

    scanned_ = true;
}

void RomStatus_ImGui::renderTable(const char* id, std::vector<Probe>& rows,
                                  const std::string& highlight)
{
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 4, kFlags)) return;

    // The part column carries the Apple part number, which is the whole
    // point of the row — give it the room and hover for the full string.
    ImGui::TableSetupColumn("Part / profile", ImGuiTableColumnFlags_WidthStretch, 0.44f);
    ImGui::TableSetupColumn("File used",      ImGuiTableColumnFlags_WidthStretch, 0.27f);
    ImGui::TableSetupColumn("Size",           ImGuiTableColumnFlags_WidthStretch, 0.10f);
    ImGui::TableSetupColumn("CRC32",          ImGuiTableColumnFlags_WidthStretch, 0.19f);
    ImGui::TableHeadersRow();

    for (auto& p : rows) {
        ImGui::TableNextRow();
        ImGui::PushID(&p);

        // ── Part / profile, with the status LED up front.
        ImGui::TableNextColumn();
        const bool have = p.usedIndex >= 0;
        const FileState* used = have ? &p.files[static_cast<std::size_t>(p.usedIndex)]
                                     : nullptr;
        const bool bad = have && !used->sizeOk;
        ImGui::TextColored(bad ? colBad() : (have ? (p.fallback ? colWarn() : colOk())
                                                  : colBad()),
                           "%s", bad     ? ICON_FA_TRIANGLE_EXCLAMATION
                                 : have  ? (p.fallback ? ICON_FA_TRIANGLE_EXCLAMATION
                                                       : ICON_FA_CIRCLE_CHECK)
                                         : ICON_FA_CIRCLE_XMARK);
        ImGui::SameLine();
        const bool isActive = !highlight.empty() && p.name == highlight;
        if (isActive) ImGui::PushStyleColor(ImGuiCol_Text,
                                            ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        ImGui::TextUnformatted(p.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.name.c_str());
        if (isActive) {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("(running)");
        }

        // ── File used + the full probe order behind a tooltip.
        ImGui::TableNextColumn();
        if (have) {
            ImGui::TextUnformatted(used->candidate.c_str());
        } else {
            ImGui::TextColored(colBad(), "missing");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Probed in this order:");
            for (std::size_t i = 0; i < p.files.size(); ++i) {
                const FileState& f = p.files[i];
                ImGui::BulletText("%s — %s", f.candidate.c_str(),
                                  f.found ? "found" : "not found");
                if (f.found && !f.resolved.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%s)", f.resolved.c_str());
                }
            }
            if (p.requiredSize)
                ImGui::Text("Required size: %s",
                            humanSize(p.requiredSize).c_str());
            if (!p.crcLabel.empty())
                ImGui::Text("Reference dump: %s", p.crcLabel.c_str());
            if (!have && !p.note.empty()) {
                ImGui::Separator();
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
                ImGui::TextUnformatted(p.note.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTooltip();
        }
        if (p.fallback && have) {
            ImGui::SameLine();
            ImGui::TextColored(colWarn(), "(fallback)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Not the preferred dump for this entry — POM2 is running "
                    "on a substitute.\nFirst choice: %s",
                    p.files.front().candidate.c_str());
        }

        // ── Size.
        ImGui::TableNextColumn();
        if (have) {
            if (used->sizeOk) {
                ImGui::TextUnformatted(humanSize(used->size).c_str());
            } else {
                ImGui::TextColored(colBad(), "%s", humanSize(used->size).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Wrong size — expected %s. This is a "
                                      "different file, not a variant.",
                                      humanSize(p.requiredSize).c_str());
            }
        } else {
            ImGui::TextDisabled("—");
        }

        // ── CRC32: identification, and a verdict only where there is a
        //    reference to compare against.
        ImGui::TableNextColumn();
        if (!have) {
            ImGui::TextDisabled("—");
        } else if (used->crcKnown) {
            ImGui::TextColored(used->crcOk ? colOk() : colWarn(),
                               "%08X %s", used->crc,
                               used->crcOk ? "" : "(unknown dump)");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(used->crcOk
                    ? "Matches the reference dump: %s"
                    : "Does not match POM2's reference dump (%s).\nIt may "
                      "still be a legitimate variant — the size is what POM2 "
                      "actually requires.",
                    p.crcLabel.c_str());
        } else {
            ImGui::TextDisabled("%08X", used->crc);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shown for identification. POM2 has no "
                                  "reference dump to check this one against.");
        }

        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RomStatus_ImGui::render(bool* open, const std::string& activeProfileName)
{
    if (!open || !*open) return;
    if (!scanned_) rescan();
    pollFetchJoin();

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ROM Status", open)) {
        ImGui::End();
        return;
    }

    // ── Where POM2 is looking, and the headline count.
    if (romDir_.empty()) {
        ImGui::TextColored(colBad(), ICON_FA_TRIANGLE_EXCLAMATION
                           " No roms/ directory found in any search root.");
    } else {
        ImGui::TextDisabled("ROMs directory:");
        ImGui::SameLine();
        ImGui::TextUnformatted(romDir_.c_str());
    }
    if (ImGui::IsItemHovered() && !searchRoots_.empty()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Search roots, first hit wins:");
        for (const auto& r : searchRoots_) ImGui::BulletText("%s", r.c_str());
        ImGui::EndTooltip();
    }

    if (missingRequired_ > 0)
        ImGui::TextColored(colBad(), ICON_FA_CIRCLE_XMARK
                           " %d profile%s cannot load its firmware.",
                           missingRequired_, missingRequired_ == 1 ? "" : "s");
    if (missingOptional_ > 0)
        ImGui::TextColored(colWarn(), ICON_FA_TRIANGLE_EXCLAMATION
                           " %d optional ROM%s missing — hover a row to see "
                           "what that costs.",
                           missingOptional_, missingOptional_ == 1 ? " is" : "s are");
    if (missingRequired_ == 0 && missingOptional_ == 0)
        ImGui::TextColored(colOk(), ICON_FA_CIRCLE_CHECK
                           " Every ROM POM2 knows about is present.");

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_ARROW_ROTATE_RIGHT " Rescan")) rescan();
    ImGui::SameLine();
#if defined(__EMSCRIPTEN__)
    ImGui::BeginDisabled();
    ImGui::Button(ICON_FA_DOWNLOAD " Download missing from RetroBIOS");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("ROM download is not available in the browser build.");
#else
    {
        const bool busy = fetchRunning_.load();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(ICON_FA_DOWNLOAD " Download missing from RetroBIOS"))
            startRetroBiosFetch();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(
                "Fetches Apple II firmware POM2 knows how to use from the "
                "RetroBIOS collection (github.com/Abdess/retrobios). Files "
                "already present are left alone. The collection has no //c / "
                "//c+, Liron or TransWarp dump.");
            ImGui::TextDisabled("Saving into: %s",
                                pom2::writableRomsDir().string().c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
#endif
    if (fetchRunning_.load()) {
        const int done  = fetchDone_.load();
        const int total = fetchTotal_.load();
        std::string line;
        {
            std::lock_guard<std::mutex> lk(fetchMutex_);
            line = fetchLine_;
        }
        ImGui::SameLine();
        if (total > 0)
            ImGui::TextDisabled("%d / %d  %s", done, total, line.c_str());
        else
            ImGui::TextDisabled("%s", line.c_str());
        if (total > 0)
            ImGui::ProgressBar(static_cast<float>(done) /
                               static_cast<float>(total),
                               ImVec2(-1.0f, 0.0f), "");
    } else {
        std::lock_guard<std::mutex> lk(fetchMutex_);
        if (!fetchSummary_.empty()) {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", fetchSummary_.c_str());
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("Drop dumps into roms/ or fetch the missing "
                                "ones from RetroBIOS.");
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("##romlist", ImVec2(0, 0));
    if (ImGui::CollapsingHeader("Machine firmware"))
        renderTable("##mach", machine_, activeProfileName);
    if (ImGui::CollapsingHeader("Character generators"))
        renderTable("##chr", charRom_, activeProfileName);
    if (ImGui::CollapsingHeader("International character sets", ImGuiTreeNodeFlags_DefaultOpen))
        renderTable("##loc", locale_, std::string());
    if (ImGui::CollapsingHeader("Floppy mechanical sounds", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string cur;
        std::vector<Probe> bucket;
        auto flush = [&] {
            if (bucket.empty()) return;
            ImGui::SeparatorText(cur.c_str());
            renderTable(cur.c_str(), bucket, std::string());
            bucket.clear();
        };
        for (auto& p : sounds_) {
            if (p.group != cur) { flush(); cur = p.group; }
            bucket.push_back(p);
        }
        flush();
    }
    if (ImGui::CollapsingHeader("Peripheral cards")) {
        // The catalogue is already grouped; draw one table per group so the
        // Disk II PROM quartet doesn't read as four unrelated lines.
        std::string current;
        std::vector<Probe> bucket;
        auto flush = [&] {
            if (bucket.empty()) return;
            ImGui::SeparatorText(current.c_str());
            renderTable(current.c_str(), bucket, activeProfileName);
            bucket.clear();
        };
        for (auto& p : cards_) {
            if (p.group != current) { flush(); current = p.group; }
            bucket.push_back(p);
        }
        flush();
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace pom2
