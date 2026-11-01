// POM2 — GPL-3.0-or-later
// rom_fetch_archive_down — "Download missing ROMs" against POM2's archive
// on archive.org, with no network: RomFetchSources::download plays the
// servers, in three shapes —
//
//   403   the archive refuses (2026-10-31: both items dark; the zip URL
//         answers 403 with a 4.8 KB HTML page — curl -f reports it as exit
//         56 over HTTP/2), the error curl's wrapper reports;
//   html  the archive answers 200 with a web page, saved as pom2_firm.zip;
//   up    the archive serves pom2_firm.zip, built here from the tree's
//         roms/ in the archive's layout (every catalogue entry at
//         "roms/<probed name>"), when the tree holds every dump.
//
// Down, RetroBIOS answers 404 to everything, so nothing installs. Checked:
// every downloadable entry is attempted on its own (the fallback runs per
// entry), the summary names the archive as unavailable — not only the first
// entry's error — no page is kept as a zip to be reused by the next run, and
// an archive-only entry (the CCS 7710 PROMs, the DAN ][ EPROM) does not ask
// the archive again as its own "fallback". Up, every entry installs from the
// archive, the archive-only ones included, and RetroBIOS is never asked.

#include "RomFetch.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fail = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++g_fail; }
}

std::vector<std::uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

std::uint32_t crc32Of(const std::vector<std::uint8_t>& b)
{
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::uint8_t x : b) {
        c ^= x;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/// A stored-only PKZIP archive in pom2_firm.zip's layout (rom_import builds
/// the same shape; RomFetch's reader takes stored and deflated members).
std::vector<std::uint8_t> buildZip(
    const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& entries)
{
    std::vector<std::uint8_t> out, cdir;
    auto p16 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        v.push_back(static_cast<std::uint8_t>(x));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    };
    auto p32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) {
        p16(v, x & 0xFFFF); p16(v, x >> 16);
    };
    for (const auto& [name, plain] : entries) {
        const std::uint32_t off = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32Of(plain);
        const auto sz = static_cast<std::uint32_t>(plain.size());
        p32(out, 0x04034B50u); p16(out, 20); p16(out, 0); p16(out, 0);
        p16(out, 0); p16(out, 0); p32(out, crc); p32(out, sz); p32(out, sz);
        p16(out, static_cast<std::uint32_t>(name.size())); p16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), plain.begin(), plain.end());

        p32(cdir, 0x02014B50u); p16(cdir, 20); p16(cdir, 20); p16(cdir, 0);
        p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p32(cdir, crc);
        p32(cdir, sz); p32(cdir, sz);
        p16(cdir, static_cast<std::uint32_t>(name.size()));
        p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p16(cdir, 0); p32(cdir, 0);
        p32(cdir, off);
        cdir.insert(cdir.end(), name.begin(), name.end());
    }
    const std::uint32_t cdOff = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), cdir.begin(), cdir.end());
    p32(out, 0x06054B50u); p16(out, 0); p16(out, 0);
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p16(out, static_cast<std::uint32_t>(entries.size()));
    p32(out, static_cast<std::uint32_t>(cdir.size())); p32(out, cdOff); p16(out, 0);
    return out;
}

bool anyFile(const fs::path& dir)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file()) return true;
    return false;
}

}  // namespace

int main()
{
    // The archive's zip, from the tree the test runs in (ctest: the source
    // dir), before the sandbox moves away from it. Missing dumps leave the
    // "up" shape out, with a line saying so; CI's tree holds them all.
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> members;
    std::vector<std::string> absent;
    for (const auto& e : pom2::romFetchCatalog()) {
        const fs::path p = fs::path(e.destRel);
        if (fs::exists(p)) members.emplace_back(pom2::romArchiveMember(e), readFile(p));
        else absent.push_back(e.destRel);
    }
    const std::vector<std::uint8_t> archiveZip = buildZip(members);

    const fs::path root = fs::temp_directory_path() / "pom2_rom_fetch_archive_down";
    std::error_code ec;
    fs::remove_all(root, ec);
    for (const char* d : { "home", "cwd" }) fs::create_directories(root / d, ec);
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", (root / "home").string().c_str());
    _putenv_s("APPDATA", (root / "home").string().c_str());
#else
    setenv("HOME", (root / "home").string().c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "home").string().c_str(), 1);
#endif
    fs::current_path(root / "cwd", ec);

    std::size_t downloadable = 0;
    for (const auto& e : pom2::romFetchCatalog()) downloadable += pom2::romDownloadable(e) ? 1 : 0;
    if (pom2::romsToFetch().size() != downloadable) {
        std::fprintf(stderr, "a ROM resolved in the sandbox — not empty, skipped\n");
        return 77;
    }

    std::vector<std::string> asked;
    bool html = false, up = false;
    pom2::RomFetchSources src;
    src.download = [&](const std::string& url, const fs::path& dest, std::string& err) {
        asked.push_back(url);
        if (url.find("archive.org") != std::string::npos) {
            if (up) {
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(archiveZip.data()),
                        static_cast<std::streamsize>(archiveZip.size()));
                return true;
            }
            if (html) {
                std::ofstream(dest) << "<!DOCTYPE html><html><body>Item not available</body></html>";
                return true;                       // curl -f: a 200 is a success
            }
            err = "the server refused the download (an HTTP error such as 403 or 404) "
                  "or broke it off (curl exit 56): " + url;
            return false;
        }
        err = "HTTP 404 (curl exit 22): " + url;
        return false;
    };

    for (bool page : { false, true }) {
        html = page;
        asked.clear();
        const char* mode = page ? "html" : "403";
        const fs::path dest = root / (std::string("roms-") + mode);
        const auto r = pom2::fetchMissingRoms(dest, {}, {}, src);
        std::printf("[%s] %s\n", mode, r.summary.c_str());
        expect(r.saved == 0 && r.failed == static_cast<int>(downloadable),
               std::string(mode) + ": every downloadable entry tried (" +
                   std::to_string(r.failed) + "/" + std::to_string(downloadable) + " failed)");
        expect(r.summary.find("archive.org is unavailable") != std::string::npos,
               std::string(mode) + ": the summary names the archive as unavailable");
        expect(page || r.summary.find("HTTP error") != std::string::npos,
               "403: the reason is an HTTP error, not \"curl exited 56\"");
        expect(page || r.summary.find("central directory") == std::string::npos, "403: no zip parse");
        expect(!page || r.summary.find("central directory") != std::string::npos,
               "html: the page was read as the non-zip it is");
        expect(!anyFile(dest), std::string(mode) + ": nothing left in roms/ (no page kept as a zip)");
        int archive = 0, retro = 0;
        for (const auto& u : asked) (u.find("archive.org") != std::string::npos ? archive : retro) += 1;
        expect(archive == 1, std::string(mode) + ": the archive asked once per run (" +
                                 std::to_string(archive) + ")");
        expect(retro >= 1, std::string(mode) + ": the RetroBIOS fallback ran");
    }

    // The archive up: every entry from it, the archive-only ones included,
    // RetroBIOS never asked. (A member absent from the zip would go to
    // RetroBIOS — hence the whole catalogue or nothing.)
    if (absent.empty()) {
        html = false; up = true;
        asked.clear();
        const fs::path dest = root / "roms-up";
        const auto r = pom2::fetchMissingRoms(dest, {}, {}, src);
        std::printf("[up] %s\n", r.summary.c_str());
        expect(r.failed == 0 && r.saved == static_cast<int>(downloadable),
               "up: every entry installed from the archive (" + std::to_string(r.saved) + "/" +
                   std::to_string(downloadable) + ", " + std::to_string(r.failed) + " failed)");
        expect(r.fromArchive == r.saved && r.fromRetroBios == 0,
               "up: all counted as from the archive, none from RetroBIOS");
        expect(r.summary.find("unavailable") == std::string::npos, "up: the summary does not call the archive down");
        int archive = 0, retro = 0;
        for (const auto& u : asked) (u.find("archive.org") != std::string::npos ? archive : retro) += 1;
        expect(archive == 1, "up: the archive asked once (" + std::to_string(archive) + ")");
        expect(retro == 0, "up: RetroBIOS never asked (" + std::to_string(retro) + ")");
        for (const auto& e : pom2::romFetchCatalog()) {
            if (!pom2::romArchiveOnly(e)) continue;
            const fs::path f = dest / fs::path(e.destRel).filename();
            expect(fs::exists(f) && fs::file_size(f) == e.expectedSize,
                   std::string("up: archive-only dump installed: ") + e.destRel);
        }
    } else {
        std::printf("rom_fetch_archive_down: 'up' shape left out, %zu dump(s) absent from the tree (first: %s)\n",
                    absent.size(), absent.front().c_str());
    }

    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(root, ec);
    if (g_fail) { std::fprintf(stderr, "rom_fetch_archive_down: %d failure(s)\n", g_fail); return 1; }
    std::puts("rom_fetch_archive_down: OK");
    return 0;
}
