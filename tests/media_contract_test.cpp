// G5-1 — one contract across the parallel media paths.
//
// DiskImage, Disk35Image, Block512Backing, the SmartPort units that wrap
// the last two, and the Disk II / HDV cards that wrap the first and third.
// Every path answers the same questions; where a path diverges on purpose,
// the test names the divergence.
//
// Divergences (stated, not accidental):
//   * Block512Backing::isWriteProtected does NOT fold write-back off
//     (HDV-class cards are in-session-writable). SmartPortHdvUnit does,
//     because inside a SmartPort card the rule is the card's (R1).
//   * DiskImage::writeNibbleAt gates on physical WP only, so write-back
//     off still dirties RAM. Disk35Image::writeBlock and the SmartPort
//     units refuse. Block512Backing::writeBlock follows its own
//     isWriteProtected (so it accepts the write with write-back off).
//     The DiskIICard and ProDOSHardDiskCard rows poke the LEAF through the
//     card (driveImage() / backing()), so "not gated" there is a property
//     of that test-only path: the guest's own write reaches DiskImage via
//     writeFlux, and DiskIICard gates that on writeBackEnabled
//     (DiskIICard.cpp commitInFlightWrite + the LSS write path).
//   * A .dsk save re-encodes from sectors, so persist is "the file
//     changed by exactly the one data byte the poke flipped", not nibble
//     identity (.nib is exact).
//   * Eject saves on the WRAPPERS only (SmartPort units, Disk II / HDV
//     cards); the raw leaves' eject() drops the buffer and the caller owns
//     the save. Stated per row by `ejectSaves`.

#include "Block512Backing.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "MediaNotch.h"
#include "MediaWritePolicy.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
void clearWriteDefault()
{
    _putenv_s("POM2_MEDIA_WRITE_DEFAULT", "");
}
#else
void clearWriteDefault()
{
    unsetenv("POM2_MEDIA_WRITE_DEFAULT");
}
#endif

/// One directory per process under the host temp dir, so two runs cannot
/// collide and a read-only leftover from an aborted run (the notch phase
/// chmods the image) is never inherited: the notch is cleared BEFORE the
/// remove, which is the order `fs::remove` needs on Windows.
const fs::path& scratchDir()
{
    static const fs::path dir = [] {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch().count();
        fs::path d = fs::temp_directory_path() /
                     ("pom2_media_contract_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(d, ec);
        assert(!ec);
        return d;
    }();
    return dir;
}

fs::path scratchPath(const char* name)
{
    const fs::path p = scratchDir() / name;
    std::string err;
    (void)pom2::setMediaNotch(p.string(), false, err);
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

void writeFile(const fs::path& p, const std::vector<char>& buf)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    f.close();
    assert(f.good() && "scratch image could not be written");
}

fs::path scratch(const char* name, std::size_t bytes, uint8_t fill = 0)
{
    const fs::path p = scratchPath(name);
    writeFile(p, std::vector<char>(bytes, static_cast<char>(fill)));
    return p;
}

std::vector<uint8_t> slurp(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> out{std::istreambuf_iterator<char>(f),
                             std::istreambuf_iterator<char>()};
    assert(!out.empty() && "scratch image could not be read back");
    return out;
}

constexpr std::size_t kNibBytes =
    static_cast<std::size_t>(DiskImage::kTracks) * DiskImage::kNibblesPerTrack;
constexpr std::size_t kPoBytes  = 819200;
constexpr std::size_t kHdvBytes = 512 * 64;

struct Leaf {
    const char* name = nullptr;
    /// Product report: isWriteProtected folds write-back off.
    bool foldsWriteBack = true;
    /// Guest write is refused when write-back is off.
    bool writeGatedOnWriteBack = true;
    /// A nibble poke round-trips through saveDirty. False for .dsk: save
    /// re-encodes from sectors, so the contract is "the file changed".
    bool persistExact = true;
    /// eject() commits a dirty medium (write-back on). True for the
    /// wrappers, false for the raw leaves whose eject() drops the buffer.
    bool ejectSaves = false;
    bool (*load)(void* self, const std::string& path) = nullptr;
    void (*eject)(void* self) = nullptr;
    bool (*isWP)(const void* self) = nullptr;
    void (*setWB)(void* self, bool on) = nullptr;
    bool (*isWB)(const void* self) = nullptr;
    /// Dirty the medium with `v` (writing a different value first, so the
    /// poke is never a no-op on a medium that already holds `v`).
    bool (*writeMarker)(void* self, uint8_t v) = nullptr;
    bool (*readMarker)(const void* self, uint8_t v) = nullptr;
    bool (*saveDirty)(void* self) = nullptr;
    bool (*hasUnsaved)(const void* self) = nullptr;
    void (*setHostWP)(void* self, bool on) = nullptr;
    fs::path (*makeA)() = nullptr;
    fs::path (*makeB)() = nullptr;
};

// ── DiskImage (.nib so a nibble write round-trips) ───────────────────────

struct DiskLeaf {
    DiskImage img;
};

bool diskLoad(void* s, const std::string& path)
{
    return static_cast<DiskLeaf*>(s)->img.loadFile(path);
}
void diskEject(void* s) { static_cast<DiskLeaf*>(s)->img.eject(); }
bool diskIsWP(const void* s)
{
    return static_cast<const DiskLeaf*>(s)->img.isWriteProtected();
}
void diskSetWB(void* s, bool on)
{
    static_cast<DiskLeaf*>(s)->img.setWriteBackEnabled(on);
}
bool diskIsWB(const void* s)
{
    return static_cast<const DiskLeaf*>(s)->img.isWriteBackEnabled();
}
bool nibbleMarkerWrite(DiskImage& img, uint8_t v)
{
    if (img.nibbleAt(0, 42) == v) img.writeNibbleAt(0, 42, v ^ 0x01);
    img.writeNibbleAt(0, 42, v);
    return img.hasUnsavedChanges();
}
bool diskWrite(void* s, uint8_t v)
{
    return nibbleMarkerWrite(static_cast<DiskLeaf*>(s)->img, v);
}
bool diskRead(const void* s, uint8_t v)
{
    return static_cast<const DiskLeaf*>(s)->img.nibbleAt(0, 42) == v;
}
bool diskSave(void* s) { return static_cast<DiskLeaf*>(s)->img.saveDirty(); }
bool diskDirty(const void* s)
{
    return static_cast<const DiskLeaf*>(s)->img.hasUnsavedChanges();
}
void diskHostWP(void* s, bool on)
{
    static_cast<DiskLeaf*>(s)->img.setHostWriteProtected(on);
}
fs::path diskMakeA() { return scratch("pom2_contract_a.nib", kNibBytes, 0xA5); }
fs::path diskMakeB() { return scratch("pom2_contract_b.nib", kNibBytes, 0x5A); }

// ── Disk35Image ──────────────────────────────────────────────────────────

struct D35Leaf { pom2::Disk35Image img; };

bool d35Load(void* s, const std::string& path)
{
    return static_cast<D35Leaf*>(s)->img.loadFile(path);
}
void d35Eject(void* s) { static_cast<D35Leaf*>(s)->img.eject(); }
bool d35IsWP(const void* s)
{
    return static_cast<const D35Leaf*>(s)->img.isWriteProtected();
}
void d35SetWB(void* s, bool on)
{
    static_cast<D35Leaf*>(s)->img.setWriteBackEnabled(on);
}
bool d35IsWB(const void* s)
{
    return static_cast<const D35Leaf*>(s)->img.isWriteBackEnabled();
}
bool d35Write(void* s, uint8_t v)
{
    auto& img = static_cast<D35Leaf*>(s)->img;
    uint8_t blk[pom2::Disk35Image::kBlockBytes];
    std::memset(blk, 0x11, sizeof(blk));
    (void)img.writeBlock(2, blk);
    std::memset(blk, v, sizeof(blk));
    return img.writeBlock(2, blk);
}
bool d35Read(const void* s, uint8_t v)
{
    uint8_t blk[pom2::Disk35Image::kBlockBytes];
    if (!static_cast<const D35Leaf*>(s)->img.readBlock(2, blk)) return false;
    return blk[0] == v && blk[511] == v;
}
bool d35Save(void* s) { return static_cast<D35Leaf*>(s)->img.saveDirty(); }
bool d35Dirty(const void* s)
{
    return static_cast<const D35Leaf*>(s)->img.hasUnsavedChanges();
}
void d35HostWP(void* s, bool on)
{
    static_cast<D35Leaf*>(s)->img.setHostWriteProtected(on);
}
fs::path d35MakeA() { return scratch("pom2_contract_a.po", kPoBytes); }
fs::path d35MakeB() { return scratch("pom2_contract_b.po", kPoBytes, 0x11); }

// ── Block512Backing ──────────────────────────────────────────────────────

struct BlkLeaf { pom2::Block512Backing img; };

bool blkLoad(void* s, const std::string& path)
{
    return static_cast<BlkLeaf*>(s)->img.loadImage(path);
}
void blkEject(void* s) { static_cast<BlkLeaf*>(s)->img.eject(); }
bool blkIsWP(const void* s)
{
    return static_cast<const BlkLeaf*>(s)->img.isWriteProtected();
}
void blkSetWB(void* s, bool on)
{
    static_cast<BlkLeaf*>(s)->img.setWriteBackEnabled(on);
}
bool blkIsWB(const void* s)
{
    return static_cast<const BlkLeaf*>(s)->img.isWriteBackEnabled();
}
bool blkWrite(void* s, uint8_t v)
{
    auto& img = static_cast<BlkLeaf*>(s)->img;
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    std::memset(blk, 0x11, sizeof(blk));
    (void)img.writeBlock(1, blk);
    std::memset(blk, v, sizeof(blk));
    return img.writeBlock(1, blk);
}
bool blkRead(const void* s, uint8_t v)
{
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    if (!static_cast<const BlkLeaf*>(s)->img.readBlock(1, blk)) return false;
    return blk[0] == v && blk[511] == v;
}
bool blkSave(void* s) { return static_cast<BlkLeaf*>(s)->img.saveDirty(); }
bool blkDirty(const void* s)
{
    return static_cast<const BlkLeaf*>(s)->img.hasUnsavedChanges();
}
void blkHostWP(void* s, bool on)
{
    static_cast<BlkLeaf*>(s)->img.setHostWriteProtected(on);
}
fs::path blkMakeA() { return scratch("pom2_contract_a.hdv", kHdvBytes); }
fs::path blkMakeB() { return scratch("pom2_contract_b.hdv", kHdvBytes, 0x22); }

// ── SmartPort35Unit ──────────────────────────────────────────────────────

struct Sp35Leaf { pom2::SmartPort35Unit unit; };

bool sp35Load(void* s, const std::string& path)
{
    return static_cast<Sp35Leaf*>(s)->unit.loadImage(path);
}
void sp35Eject(void* s) { (void)static_cast<Sp35Leaf*>(s)->unit.eject(); }
bool sp35IsWP(const void* s)
{
    return static_cast<const Sp35Leaf*>(s)->unit.isWriteProtected();
}
void sp35SetWB(void* s, bool on)
{
    static_cast<Sp35Leaf*>(s)->unit.setWriteBackEnabled(on);
}
bool sp35IsWB(const void* s)
{
    return static_cast<const Sp35Leaf*>(s)->unit.isWriteBackEnabled();
}
bool sp35Write(void* s, uint8_t v)
{
    auto& img = static_cast<Sp35Leaf*>(s)->unit;
    uint8_t blk[pom2::Disk35Image::kBlockBytes];
    std::memset(blk, 0x11, sizeof(blk));
    (void)img.writeBlock(2, blk);
    std::memset(blk, v, sizeof(blk));
    return img.writeBlock(2, blk);
}
bool sp35Read(const void* s, uint8_t v)
{
    uint8_t blk[pom2::Disk35Image::kBlockBytes];
    if (!static_cast<const Sp35Leaf*>(s)->unit.readBlock(2, blk)) return false;
    return blk[0] == v && blk[511] == v;
}
bool sp35Save(void* s) { return static_cast<Sp35Leaf*>(s)->unit.saveDirty(); }
bool sp35Dirty(const void* s)
{
    return static_cast<const Sp35Leaf*>(s)->unit.hasUnsavedChanges();
}
void sp35HostWP(void* s, bool on)
{
    static_cast<Sp35Leaf*>(s)->unit.setHostWriteProtected(on);
}
fs::path sp35MakeA() { return scratch("pom2_contract_sp35_a.po", kPoBytes); }
fs::path sp35MakeB() { return scratch("pom2_contract_sp35_b.po", kPoBytes, 0x33); }

// ── SmartPortHdvUnit ─────────────────────────────────────────────────────

struct SpHdvLeaf { pom2::SmartPortHdvUnit unit; };

bool spHdvLoad(void* s, const std::string& path)
{
    return static_cast<SpHdvLeaf*>(s)->unit.loadImage(path);
}
void spHdvEject(void* s) { (void)static_cast<SpHdvLeaf*>(s)->unit.eject(); }
bool spHdvIsWP(const void* s)
{
    return static_cast<const SpHdvLeaf*>(s)->unit.isWriteProtected();
}
void spHdvSetWB(void* s, bool on)
{
    static_cast<SpHdvLeaf*>(s)->unit.setWriteBackEnabled(on);
}
bool spHdvIsWB(const void* s)
{
    return static_cast<const SpHdvLeaf*>(s)->unit.isWriteBackEnabled();
}
bool spHdvWrite(void* s, uint8_t v)
{
    auto& img = static_cast<SpHdvLeaf*>(s)->unit;
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    std::memset(blk, 0x11, sizeof(blk));
    (void)img.writeBlock(1, blk);
    std::memset(blk, v, sizeof(blk));
    return img.writeBlock(1, blk);
}
bool spHdvRead(const void* s, uint8_t v)
{
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    if (!static_cast<const SpHdvLeaf*>(s)->unit.readBlock(1, blk)) return false;
    return blk[0] == v && blk[511] == v;
}
bool spHdvSave(void* s) { return static_cast<SpHdvLeaf*>(s)->unit.saveDirty(); }
bool spHdvDirty(const void* s)
{
    return static_cast<const SpHdvLeaf*>(s)->unit.hasUnsavedChanges();
}
void spHdvHostWP(void* s, bool on)
{
    static_cast<SpHdvLeaf*>(s)->unit.setHostWriteProtected(on);
}
fs::path spHdvMakeA() { return scratch("pom2_contract_sphdv_a.hdv", kHdvBytes); }
fs::path spHdvMakeB() { return scratch("pom2_contract_sphdv_b.hdv", kHdvBytes, 0x44); }

// ── DiskImage .dsk (re-encodes; persist is "the file changed") ───────────

fs::path scratchDsk(const char* name)
{
    const fs::path p = scratchPath(name);
    std::vector<char> buf(DiskImage::kBytesPerImage);
    for (std::size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<char>((i * 13u + 7u) & 0xFF);
    writeFile(p, buf);
    return p;
}

/// The .dsk contract: a save re-encodes from sectors, and the poke below
/// flips bit 0 of one high-6 nibble, i.e. bit 2 of ONE data byte. So the
/// file after a save differs from the file before in exactly one byte, by
/// exactly 4 — a scrambled track would also "differ", and must not pass.
void assertDskChangedByOneByte(const std::vector<uint8_t>& before,
                               const std::vector<uint8_t>& after)
{
    assert(before.size() == after.size());
    std::size_t diffs = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before[i] == after[i]) continue;
        ++diffs;
        const int delta = static_cast<int>(before[i]) - static_cast<int>(after[i]);
        assert((delta == 4 || delta == -4) && "the .dsk re-encode moved a byte by more than the poke");
    }
    assert(diffs == 1 && "the .dsk re-encode changed more than the poked byte");
}

// DOS 3.3 6-and-2 GCR (DiskImage.cpp kGcrTable). A gap poke round-trips
// to the same .dsk bytes; the last high6 nibble + checksum of a data
// field is a payload change that still decodes.
constexpr uint8_t kDskGcr[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

uint8_t dskGcrInv(uint8_t disk)
{
    for (uint8_t v = 0; v < 64; ++v)
        if (kDskGcr[v] == disk) return v;
    return 0xFF;
}

bool dskWrite(void* s, uint8_t v)
{
    (void)v;   // the poke is a flip, so every call changes the medium
    auto& img = static_cast<DiskLeaf*>(s)->img;
    constexpr int kTrack = 5;
    int prologue = -1;
    for (int i = 0; i + 3 + 86 + 256 < DiskImage::kNibblesPerTrack; ++i) {
        if (img.nibbleAt(kTrack, i) == 0xD5 &&
            img.nibbleAt(kTrack, i + 1) == 0xAA &&
            img.nibbleAt(kTrack, i + 2) == 0xAD) {
            prologue = i;
            break;
        }
    }
    if (prologue < 0) return false;
    const int lastHigh = prologue + 3 + 86 + 255;
    const int chkIdx   = prologue + 3 + 86 + 256;
    const uint8_t v6  = dskGcrInv(img.nibbleAt(kTrack, lastHigh));
    const uint8_t chk = dskGcrInv(img.nibbleAt(kTrack, chkIdx));
    if (v6 == 0xFF || chk == 0xFF) return false;
    // chk == high6[255]; prev_before = chk ^ v6. Flip high6 bit 0 and
    // keep the running XOR (new_v6 = v6 ^ 1, new_chk = chk ^ 1).
    img.writeNibbleAt(kTrack, lastHigh, kDskGcr[(v6 ^ 1u) & 0x3F]);
    img.writeNibbleAt(kTrack, chkIdx,   kDskGcr[(chk ^ 1u) & 0x3F]);
    return img.hasUnsavedChanges();
}
bool dskRead(const void* s, uint8_t v)
{
    (void)s; (void)v;
    // persistExact is false: runOne compares the FILE instead. Answering
    // "found" here would let a flipped flag pass a phase that never ran.
    return false;
}
fs::path dskMakeA() { return scratchDsk("pom2_contract_a.dsk"); }
fs::path dskMakeB() { return scratchDsk("pom2_contract_b.dsk"); }

// ── DiskIICard (wraps DiskImage) ─────────────────────────────────────────

struct DiiLeaf { DiskIICard card{6}; };

bool diiLoad(void* s, const std::string& path)
{
    return static_cast<DiiLeaf*>(s)->card.insertDisk(0, path);
}
void diiEject(void* s) { (void)static_cast<DiiLeaf*>(s)->card.ejectDisk(0); }
bool diiIsWP(const void* s)
{
    return static_cast<const DiiLeaf*>(s)->card.driveImage(0).isWriteProtected();
}
void diiSetWB(void* s, bool on)
{
    static_cast<DiiLeaf*>(s)->card.setWriteBackEnabled(on);
}
bool diiIsWB(const void* s)
{
    // The image the card mounted, not the card's own flag: the flag cannot
    // be dropped by a remount, the image's copy of it can.
    return static_cast<const DiiLeaf*>(s)->card.driveImage(0).isWriteBackEnabled();
}
bool diiWrite(void* s, uint8_t v)
{
    return nibbleMarkerWrite(static_cast<DiiLeaf*>(s)->card.driveImage(0), v);
}
bool diiRead(const void* s, uint8_t v)
{
    return static_cast<const DiiLeaf*>(s)->card.driveImage(0).nibbleAt(0, 42) == v;
}
bool diiSave(void* s)
{
    return static_cast<DiiLeaf*>(s)->card.flushPendingWrites();
}
bool diiDirty(const void* s)
{
    return static_cast<const DiiLeaf*>(s)->card.hasUnsavedChanges(0);
}
void diiHostWP(void* s, bool on)
{
    static_cast<DiiLeaf*>(s)->card.setDriveHostWriteProtected(0, on);
}
fs::path diiMakeA() { return scratch("pom2_contract_dii_a.nib", kNibBytes, 0xA5); }
fs::path diiMakeB() { return scratch("pom2_contract_dii_b.nib", kNibBytes, 0x5A); }

// ── ProDOSHardDiskCard (wraps Block512Backing; does not fold write-back) ─

struct HdvCardLeaf { ProDOSHardDiskCard card{5}; };

bool hdvLoad(void* s, const std::string& path)
{
    return static_cast<HdvCardLeaf*>(s)->card.loadImage(path);
}
void hdvEject(void* s) { (void)static_cast<HdvCardLeaf*>(s)->card.ejectImage(); }
bool hdvIsWP(const void* s)
{
    return static_cast<const HdvCardLeaf*>(s)->card.isWriteProtected();
}
void hdvSetWB(void* s, bool on)
{
    static_cast<HdvCardLeaf*>(s)->card.setWriteBackEnabled(on);
}
bool hdvIsWB(const void* s)
{
    return static_cast<const HdvCardLeaf*>(s)->card.isWriteBackEnabled();
}
bool hdvWrite(void* s, uint8_t v)
{
    auto& img = static_cast<HdvCardLeaf*>(s)->card.backing();
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    std::memset(blk, 0x11, sizeof(blk));
    (void)img.writeBlock(1, blk);
    std::memset(blk, v, sizeof(blk));
    return img.writeBlock(1, blk);
}
bool hdvRead(const void* s, uint8_t v)
{
    uint8_t blk[pom2::Block512Backing::kBlockBytes];
    if (!static_cast<const HdvCardLeaf*>(s)->card.backing().readBlock(1, blk)) return false;
    return blk[0] == v && blk[511] == v;
}
bool hdvSave(void* s) { return static_cast<HdvCardLeaf*>(s)->card.saveDirty(); }
bool hdvDirty(const void* s)
{
    return static_cast<const HdvCardLeaf*>(s)->card.hasUnsavedChanges();
}
void hdvHostWP(void* s, bool on)
{
    static_cast<HdvCardLeaf*>(s)->card.setHostWriteProtected(on);
}
fs::path hdvMakeA() { return scratch("pom2_contract_hdvcard_a.hdv", kHdvBytes); }
fs::path hdvMakeB() { return scratch("pom2_contract_hdvcard_b.hdv", kHdvBytes, 0x22); }

void runOne(const Leaf& L, void* self)
{
    std::error_code ec;
    std::string err;
    constexpr uint8_t kMark      = 0xC9;   // persist phase
    constexpr uint8_t kMarkNotch = 0xC7;   // notch-while-dirty phase
    constexpr uint8_t kMarkEject = 0xC5;   // eject-saves phase
    constexpr uint8_t kMarkDrop  = 0xC3;   // write-back-off eject phase

    const fs::path a = L.makeA();
    const fs::path b = L.makeB();

    // ── Default: writable, not protected ────────────────────────────────
    assert(L.load(self, a.string()));
    assert(!L.isWP(self) && "a plain image came up write-protected");
    assert(L.isWB(self) && "the product default is writable");

    // ── A clean flush is a no-op, on the flag AND on the file ───────────
    {
        const auto before = slurp(a);
        assert(L.saveDirty(self));
        assert(!L.hasUnsaved(self));
        assert(slurp(a) == before && "a clean flush rewrote the file");
    }

    // ── Write-back off: what the guest sees, what a write does ──────────
    L.setWB(self, false);
    assert(L.foldsWriteBack == L.isWP(self));
    if (L.writeGatedOnWriteBack) {
        assert(!L.writeMarker(self, kMark));
        assert(!L.hasUnsaved(self));
    } else {
        // DiskImage: nibble gate is physical WP. Block512: isWriteProtected
        // does not fold write-back, so the write lands in RAM.
        assert(L.writeMarker(self, kMark));
        assert(L.hasUnsaved(self));
        L.eject(self);
        assert(L.load(self, a.string()));
        L.setWB(self, false);
    }

    // ── A remount preserves the opt-out ─────────────────────────────────
    L.setWB(self, false);
    assert(L.load(self, b.string()));
    assert(!L.isWB(self) && "a remount dropped the write-back opt-out");
    assert(L.foldsWriteBack == L.isWP(self));

    // ── Write + save + reload ───────────────────────────────────────────
    L.setWB(self, true);
    assert(L.load(self, a.string()));
    assert(L.writeMarker(self, kMark));
    assert(L.hasUnsaved(self));
    {
        const auto preSave = slurp(a);
        assert(L.saveDirty(self));
        assert(!L.hasUnsaved(self));
        L.eject(self);
        assert(L.load(self, a.string()));
        if (L.persistExact)
            assert(L.readMarker(self, kMark) && "saveDirty + reload lost the write");
        else
            assertDskChangedByOneByte(preSave, slurp(a));
    }

    // ── A notch flipped while dirty still commits — to the FILE ─────────
    // A distinct marker, so the file cannot already hold it: the dirty
    // flag clearing with nothing written would otherwise pass.
    assert(L.writeMarker(self, kMarkNotch));
    assert(L.hasUnsaved(self));
    {
        const auto preSave = slurp(a);
        assert(pom2::setMediaNotch(a.string(), true, err) && err.empty());
        L.setHostWP(self, true);
        assert(L.saveDirty(self) && "a notch flipped while dirty dropped the save");
        assert(!L.hasUnsaved(self));
        L.eject(self);
        assert(pom2::mediaFileIsReadOnly(a.string()) &&
               "the commit under the notch lost the notch");
        assert(pom2::setMediaNotch(a.string(), false, err));
        assert(L.load(self, a.string()));
        if (L.persistExact)
            assert(L.readMarker(self, kMarkNotch) && "the save under the notch wrote nothing");
        else
            assertDskChangedByOneByte(preSave, slurp(a));
    }

    // ── Eject with write-back on: the wrappers save, the leaves do not ──
    L.setWB(self, true);
    L.setHostWP(self, false);
    assert(L.writeMarker(self, kMarkEject));
    assert(L.hasUnsaved(self));
    {
        const auto preEject = slurp(a);
        L.eject(self);
        const auto postEject = slurp(a);
        assert(L.load(self, a.string()));
        if (L.persistExact) {
            assert(L.readMarker(self, kMarkEject) == L.ejectSaves &&
                   "eject's save-on-eject policy is not what the row states");
        } else if (L.ejectSaves) {
            assertDskChangedByOneByte(preEject, postEject);
        } else {
            assert(preEject == postEject && "a raw leaf's eject wrote the file");
        }
    }

    // ── Eject with write-back off leaves the file alone ─────────────────
    // Recreate a clean file so the dropped write is observably absent.
    L.eject(self);
    fs::remove(a, ec);
    const fs::path drop = L.makeA();
    L.setWB(self, true);
    L.setHostWP(self, false);
    assert(L.load(self, drop.string()));
    const auto original = slurp(drop);
    assert(L.writeMarker(self, kMarkDrop));
    assert(L.hasUnsaved(self));
    L.setWB(self, false);
    assert(L.saveDirty(self));
    L.eject(self);
    assert(slurp(drop) == original &&
           "eject with write-back off rewrote the file");

    fs::remove(drop, ec);
    fs::remove(b, ec);
    std::printf("  ok: %s\n", L.name);
}

}  // namespace

int main()
{
    // ctest sets POM2_MEDIA_WRITE_DEFAULT=protected; this test is the
    // product default. Must run before the first medium is constructed
    // (the policy caches on first use).
    clearWriteDefault();

    const Leaf disk{
        "DiskImage.nib",
        /*foldsWriteBack=*/true,
        /*writeGatedOnWriteBack=*/false,
        /*persistExact=*/true,
        /*ejectSaves=*/false,
        diskLoad, diskEject, diskIsWP, diskSetWB, diskIsWB,
        diskWrite, diskRead, diskSave, diskDirty, diskHostWP,
        diskMakeA, diskMakeB,
    };
    const Leaf dsk{
        "DiskImage.dsk",
        true, false, /*persistExact=*/false, /*ejectSaves=*/false,
        diskLoad, diskEject, diskIsWP, diskSetWB, diskIsWB,
        dskWrite, dskRead, diskSave, diskDirty, diskHostWP,
        dskMakeA, dskMakeB,
    };
    const Leaf d35{
        "Disk35Image",
        true, true, true, /*ejectSaves=*/false,
        d35Load, d35Eject, d35IsWP, d35SetWB, d35IsWB,
        d35Write, d35Read, d35Save, d35Dirty, d35HostWP,
        d35MakeA, d35MakeB,
    };
    const Leaf blk{
        "Block512Backing",
        /*foldsWriteBack=*/false,
        /*writeGatedOnWriteBack=*/false,
        /*persistExact=*/true,
        /*ejectSaves=*/false,
        blkLoad, blkEject, blkIsWP, blkSetWB, blkIsWB,
        blkWrite, blkRead, blkSave, blkDirty, blkHostWP,
        blkMakeA, blkMakeB,
    };
    const Leaf sp35{
        "SmartPort35Unit",
        true, true, true, /*ejectSaves=*/true,
        sp35Load, sp35Eject, sp35IsWP, sp35SetWB, sp35IsWB,
        sp35Write, sp35Read, sp35Save, sp35Dirty, sp35HostWP,
        sp35MakeA, sp35MakeB,
    };
    const Leaf spHdv{
        "SmartPortHdvUnit",
        true, true, true, /*ejectSaves=*/true,
        spHdvLoad, spHdvEject, spHdvIsWP, spHdvSetWB, spHdvIsWB,
        spHdvWrite, spHdvRead, spHdvSave, spHdvDirty, spHdvHostWP,
        spHdvMakeA, spHdvMakeB,
    };
    const Leaf dii{
        "DiskIICard",
        true, false, true, /*ejectSaves=*/true,
        diiLoad, diiEject, diiIsWP, diiSetWB, diiIsWB,
        diiWrite, diiRead, diiSave, diiDirty, diiHostWP,
        diiMakeA, diiMakeB,
    };
    const Leaf hdv{
        "ProDOSHardDiskCard",
        false, false, true, /*ejectSaves=*/true,
        hdvLoad, hdvEject, hdvIsWP, hdvSetWB, hdvIsWB,
        hdvWrite, hdvRead, hdvSave, hdvDirty, hdvHostWP,
        hdvMakeA, hdvMakeB,
    };

    // Heap, one at a time: a DiskImage is ~242 KB of inline tracks and the
    // Disk II card carries two.
    runOne(disk,  std::make_unique<DiskLeaf>().get());
    runOne(dsk,   std::make_unique<DiskLeaf>().get());
    runOne(d35,   std::make_unique<D35Leaf>().get());
    runOne(blk,   std::make_unique<BlkLeaf>().get());
    runOne(sp35,  std::make_unique<Sp35Leaf>().get());
    runOne(spHdv, std::make_unique<SpHdvLeaf>().get());
    runOne(dii,   std::make_unique<DiiLeaf>().get());
    runOne(hdv,   std::make_unique<HdvCardLeaf>().get());

    std::error_code ec;
    fs::remove_all(scratchDir(), ec);
    std::puts("media_contract: every path honours the contract");
    return 0;
}
