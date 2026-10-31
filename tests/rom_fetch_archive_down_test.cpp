// POM2 — GPL-3.0-or-later
// rom_fetch_archive_down — "Download missing ROMs" when POM2's archive on
// archive.org is gone (2026-10-31: both items dark; the zip URL answers 403
// with a 4.8 KB HTML page — curl -f reports it as exit 56 over HTTP/2). No
// network: RomFetchSources::download plays the servers, in two shapes —
//
//   403   the archive refuses (the error curl's wrapper reports);
//   html  the archive answers 200 with a web page, saved as pom2_roms.zip.
//
// RetroBIOS answers 404 to everything, so nothing installs. Checked: every
// downloadable entry is attempted on its own (the fallback runs per entry),
// the summary names the archive as unavailable — not only the first entry's
// error — and no page is kept as a zip to be reused by the next run.

#include "RomFetch.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_fail = 0;
void expect(bool ok, const std::string& what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++g_fail; }
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
    bool html = false;
    pom2::RomFetchSources src;
    src.download = [&](const std::string& url, const fs::path& dest, std::string& err) {
        asked.push_back(url);
        if (url.find("archive.org") != std::string::npos) {
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

    fs::current_path(fs::temp_directory_path(), ec);
    fs::remove_all(root, ec);
    if (g_fail) { std::fprintf(stderr, "rom_fetch_archive_down: %d failure(s)\n", g_fail); return 1; }
    std::puts("rom_fetch_archive_down: OK");
    return 0;
}
