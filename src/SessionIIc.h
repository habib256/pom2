// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"
#include "Disk35Image.h"
#include <array>
#include <memory>
class Memory;
class SlotPeripheral;
namespace pom2 {
bool describeIIcSessionDevice(SlotPeripheral* card, SessionArchive::Slot& out, std::string& error);
bool captureIIcSessionDevice(SlotPeripheral* card, std::vector<uint8_t>& out, size_t& mediaBytes, std::string& error);
bool prepareIIcSessionDevice(SlotPeripheral* card, const SessionArchive::Slot& record, int slot,
                             std::unique_ptr<SlotPeripheral>& out, size_t& mediaBytes, std::string& error, bool allowWritable, bool allowBlocks);
void applyIIcSessionDevice(SlotPeripheral* card, SlotPeripheral* prepared);
struct SessionSonyMedia {
    std::array<std::unique_ptr<Disk35Image>, 3> images;
    std::array<std::vector<uint8_t>, 3> writes;
    auto& operator[](size_t index) { return images[index]; }
};
bool captureSessionSonyMedia(Memory& memory, SessionArchive::Slot& record, size_t& mediaBytes, std::string& error);
/// `replaceUnsaved`: an SDK restore, whose host flushed what it keeps (see
/// SessionIIc.cpp); a session file never replaces unsaved writes.
bool prepareSessionSonyMedia(Memory& memory, const SessionArchive::Slot& record, SessionSonyMedia& out,
                             size_t& mediaBytes, std::string& error, bool allowWritable,
                             bool replaceUnsaved = false);
void applySessionSonyMedia(Memory& memory, SessionSonyMedia& prepared);
/// The mounted //c+ 3.5" medium that holds `path`, or nullptr: a restore
/// onto that same copy keeps its stamp and lineage (Disk35Image::bindSnapshotBacking).
const Disk35Image* liveSonyImage(Memory& memory, const std::string& path);
}
