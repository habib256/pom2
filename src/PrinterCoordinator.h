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

// PrinterCoordinator owns the host printer cable policy. SlotBus owns every
// interface card; this class resolves them under the machine lock, copies the
// values a frame needs and drains exactly one source according to the physical
// priority. No SlotBus-owned pointer escapes a call.

#ifndef POM2_PRINTER_COORDINATOR_H
#define POM2_PRINTER_COORDINATOR_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

class Settings;

class PrinterCoordinator
{
public:
    enum class SourceKind : std::uint8_t {
        None,
        PrinterCard,
        Grappler,
        FujiNet,
        SuperSerial,
    };

    struct PrinterPanelSnapshot {
        bool plugged = false;
        int slot = -1;
        std::size_t bytesWritten = 0;
        bool spoolTruncated = false;
        std::string spoolText;
    };

    struct HostSnapshot {
        SourceKind source = SourceKind::None;
        int sourceSlot = -1;
        std::vector<std::string> ignoredSources;

        bool grapplerPlugged = false;
        int grapplerSlot = -1;
        bool grapplerRomLoaded = false;
        bool grapplerBusy = false;
        int grapplerPrinterType = -1;
        bool grapplerMsbSoftwareControl = false;

        bool printerCardPlugged() const noexcept
        {
            return source == SourceKind::PrinterCard || printerCardSlot >= 0;
        }

        int printerCardSlot = -1;
    };

    struct FeedBatch {
        SourceKind source = SourceKind::None;
        int sourceSlot = -1;
        std::vector<std::uint8_t> bytes;

        bool haveSource() const noexcept { return source != SourceKind::None; }
    };

    struct BusyUpdate {
        bool grapplerPlugged = false;
        bool changed = false;
    };

    PrinterPanelSnapshot capturePrinterPanel(
        EmulationController& controller) const;
    bool clearPrinterPanelSpool(EmulationController& controller, int slot);

    HostSnapshot captureHost(EmulationController& controller) const;
    FeedBatch drainImageWriter(EmulationController& controller);

    bool setGrapplerPrinterType(EmulationController& controller, int value);
    BusyUpdate setGrapplerBusy(EmulationController& controller, bool busy);
    void persistGrappler(Settings& settings,
                         EmulationController& controller) const;

    /// Slot rebuilds can reuse an allocator address for a replacement card.
    /// Explicitly invalidate the handover identity before SlotBus::clear().
    void resetFeedCursor() noexcept;

private:
    struct SourceIdentity {
        SourceKind kind = SourceKind::None;
        std::uintptr_t address = 0;

        bool operator==(const SourceIdentity& rhs) const noexcept
        {
            return kind == rhs.kind && address == rhs.address;
        }
        bool operator!=(const SourceIdentity& rhs) const noexcept
        {
            return !(*this == rhs);
        }
    };

    void prepareDrain(SourceIdentity identity, std::size_t total);
    /// Declare the batch irreversible so the rewind ring drops its history
    /// (see the .cpp for why a cursor reset would be the wrong fix).
    static void notePrinted(const FeedBatch& batch);

    SourceIdentity source_;
    std::size_t consumed_ = 0;
};

} // namespace pom2

#endif // POM2_PRINTER_COORDINATOR_H
