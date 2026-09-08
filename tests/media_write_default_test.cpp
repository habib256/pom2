// Media are writable by default — the write-protect is the user's opt-out.
//
// Policy since 2026-09-08: a peripheral writes unless the user protects it,
// and that protection is visible and editable in every media panel (Disk II,
// 3.5", HDV, SmartPort units, the Slot Config media rows). Before, every
// medium came up write-protected and the guest's saves silently stayed in
// memory until the user found the "Write-back" opt-in — a DOS SAVE answered
// I/O ERROR, a ProDOS save "succeeded" and vanished on quit. This pins the
// default at every storage leaf and card the panels reach.

#include "Block512Backing.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path scratchImage(const char* leaf, std::size_t bytes, uint8_t fill)
{
    const fs::path p = fs::temp_directory_path() / leaf;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(bytes, static_cast<char>(fill));
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    return p;
}

}  // namespace

int main()
{
    // The suite runs under POM2_MEDIA_WRITE_DEFAULT=protected so that tests
    // booting tracked images never write them back; this test is about the
    // PRODUCT default, so it clears the variable before the first medium is
    // built (the policy caches its answer on first use).
#ifdef _WIN32
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
#else
    unsetenv("POM2_MEDIA_WRITE_DEFAULT");
#endif
    std::error_code ec;

    // Disk II card and its image: a fresh card writes, and the drive does
    // NOT report write-protect on a plain .dsk.
    {
        DiskIICard card(6);
        assert(card.isWriteBackEnabled() && "a fresh Disk II must be writable");
        const fs::path dsk = scratchImage("pom2_media_default.dsk", 143360, 0x00);
        DiskImage img;
        assert(img.loadFile(dsk.string()));
        assert(!img.isWriteProtected() && "a plain .dsk came up write-protected");
        img.setWriteBackEnabled(false);
        assert(img.isWriteProtected() && "the opt-out must still protect");
        fs::remove(dsk, ec);
    }

    // 3.5" image: writable unless the file or the container says otherwise.
    {
        const fs::path po = scratchImage("pom2_media_default_800k.po", 819200, 0x00);
        pom2::Disk35Image img;
        assert(img.loadFile(po.string()));
        assert(!img.isWriteProtected() && "a plain 800K .po came up write-protected");
        img.setWriteBackEnabled(false);
        assert(img.isWriteProtected());
        fs::remove(po, ec);
    }

    // Block devices: the backing store and the HDV card.
    {
        pom2::Block512Backing backing;
        assert(backing.isWriteBackEnabled() && "a fresh block backing must write");
        ProDOSHardDiskCard hdv(5);
        assert(hdv.isWriteBackEnabled() && "a fresh HDV card must write");
        const fs::path hdvImg = scratchImage("pom2_media_default.hdv", 8 * 512, 0x00);
        assert(hdv.loadImage(hdvImg.string()));
        assert(hdv.isWriteBackEnabled() && "mounting must not turn write-back off");
        assert(!hdv.isWriteProtected());
        fs::remove(hdvImg, ec);
    }

    std::printf("media_write_default OK: Disk II, .dsk, 3.5\" .po, block backing "
                "and HDV are writable by default; the opt-out still protects\n");
    return 0;
}
