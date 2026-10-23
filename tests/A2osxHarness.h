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

// A2osxHarness — a //e enhanced or a //c that boots A2osX
// (disks_3.5/A2osX-0.95-STABLE.800.po, on a scratch COPY: A2osX writes to its
// own disk) from a slot-5 SmartPort card, plugs POM2 catalog cards, and types
// at the A2osX shell. Shared by the a2osx_boot ctest and a2osx_boot_probe.
//
// No window, no thread: the CPU is run inline, so a run is deterministic and
// takes well under a second of host time.

#ifndef POM2_TESTS_A2OSX_HARNESS_H
#define POM2_TESTS_A2OSX_HARNESS_H

#include "ClockCard.h"
#include "DiskIICard.h"
#include "IIcExternalSmartPort.h"
#include "IIcMouse.h"
#include "IWMDevice.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "ResourcePaths.h"
#include "SlirpNetworkBackend.h"
#include "SlotCardFactory.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortCard.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace pom2test {

/// A backend that forwards to another and, when `log` is on, prints one line
/// per frame: direction, length, EtherType, and the first bytes past it.
class LoggingNetworkBackend final : public pom2::NetworkBackend {
public:
    explicit LoggingNetworkBackend(std::unique_ptr<pom2::NetworkBackend> inner)
        : inner_(std::move(inner)) {}
    bool isValid() const override { return inner_->isValid(); }
    std::string_view name() const override { return inner_->name(); }
    void transmit(const uint8_t* f, int len) override { dump("TX", f, len); inner_->transmit(f, len); }
    int receive(uint8_t* buf, int cap) override
    {
        const int n = inner_->receive(buf, cap);
        if (n > 0) dump("RX", buf, n);
        return n;
    }
    void poll() override { inner_->poll(); }
    bool resolveMac(uint32_t ip, pom2::MacAddress& out) override { return inner_->resolveMac(ip, out); }

private:
    static void dump(const char* dir, const uint8_t* f, int len)
    {
        std::printf("[net] %s %4d type %02X%02X:", dir, len, len > 13 ? f[12] : 0, len > 13 ? f[13] : 0);
        for (int i = 14; i < len && i < 14 + 24; ++i) std::printf(" %02X", f[i]);
        std::printf("\n");
    }
    std::unique_ptr<pom2::NetworkBackend> inner_;
};

class A2osxMachine {
public:
    static constexpr double kHz = 1022727.0;

    /// ROM and disk resolved from the tree; `ready()` false when either is
    /// missing (a test then skips).
    /// `image` replaces the tracked 800K disk (any ProDOS image; one larger
    /// than 800K goes in a hard-disk unit).
    explicit A2osxMachine(bool iic, const std::string& scratchName = "pom2_a2osx.po",
                          const std::string& image = {})
        : iic_(iic), port_(&mem_.slotBus())
    {
        const std::string rom = pom2::findResource(iic ? "roms/apple2c-32Kv0.rom" : "roms/apple2e.rom");
        const std::string disk = image.empty()
            ? pom2::findResource("disks_3.5/A2osX-0.95-STABLE.800.po") : image;
        if (rom.empty() || disk.empty()) return;
        copy_ = std::filesystem::temp_directory_path() / scratchName;
        std::error_code ec;
        std::filesystem::copy_file(disk, copy_, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return;

        mem_.setCpu(&cpu_);
        mem_.clearRam();
        mem_.resetSoftSwitches();
        mem_.setIIEMode(true);
        if (iic) {
            mem_.setIWM(&iwm_);
            mem_.setIWMAuthoritative(true);
            mem_.setExternalSmartPort(&port_);
        }
        if (!mem_.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/iic)) return;
        auto d2 = std::make_unique<DiskIICard>(6);
        if (iic) d2->setIWM(&iwm_);
        mem_.slotBus().plug(6, std::move(d2));
        auto sp = std::make_unique<pom2::SmartPortCard>(5);
        std::error_code szEc;
        if (std::filesystem::file_size(copy_, szEc) > 819200)
            sp->setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
        else
            sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
        sp->setUnit(1, std::make_unique<pom2::SmartPort35Unit>());
        std::string err;
        if (!sp->mountBay(0, copy_.string(), err)) return;
        // It is a scratch copy, so it writes even under ctest's
        // POM2_MEDIA_WRITE_DEFAULT=protected: A2osX logs to its disk, and a
        // test edits etc/init and boots again.
        sp->setBayWriteBack(0, true);
        mem_.slotBus().plug(5, std::move(sp));
        ready_ = true;
    }

    ~A2osxMachine()
    {
        // The SmartPort card commits its dirty disk when it is destroyed:
        // let it, THEN delete the copy, or the commit recreates the file.
        (void)mem_.slotBus().unplug(5);
        std::error_code ec;
        if (!copy_.empty()) std::filesystem::remove(copy_, ec);
    }

    bool ready() const { return ready_; }
    /// Print every Ethernet frame the cards plugged AFTER this call exchange.
    void setNetLog(bool on) { netLog_ = on; }
    /// The card in `slot`, as its concrete type (nullptr otherwise).
    template <class T> T* card(int slot)
    {
        return dynamic_cast<T*>(mem_.slotBus().peripheral(slot));
    }
    Memory& memory() { return mem_; }
    M6502&  cpu()    { return cpu_; }

    /// Plug a POM2 card by its catalog key. The keys the window composes
    /// itself (SSC, Uthernet I/II, Mockingboard) are built here the same way.
    /// "chatmauve-eve" is Le Chat Mauve set to its //e Eve variant.
    /// "iicmouse" and "mb4c" are the //c's own: the IOU mouse, and the
    /// internal-connector Mockingboard 4c (POM2 slot 3, firmware page $C4).
    bool plug(int slot, const std::string& key)
    {
        std::unique_ptr<SlotPeripheral> card;
        // The Ethernet cards get the virtual network the window gives them:
        // libslirp, with POM2's safe defaults (DHCP at 10.0.2.2, no host
        // loopback). Without it a card exists but nothing answers.
        if (key == "uthernet2") {
            auto u = std::make_unique<pom2::UthernetIICard>(slot);
            if (auto net = network()) u->setBackend(std::move(net));
            card = std::move(u);
        } else if (key == "uthernet") {
            auto u = std::make_unique<pom2::UthernetCard>(slot);
            if (auto net = network()) u->setBackend(std::move(net));
            card = std::move(u);
        }
        else if (key == "iicmouse")  card = std::make_unique<IIcMouse>(slot);
        else if (key == "clock")     card = std::make_unique<ClockCard>(slot);
        else if (key == "chatmauve-eve")
            card = std::make_unique<LeChatMauveCard>(slot, LeChatMauveCard::Variant::Eve);
        else if (key == "mockingboard" || key == "mb4c") {
            auto mb = std::make_unique<MockingboardCard>(slot);
            mb->setCpu(&cpu_);
            card = std::move(mb);
        } else if (key == "ssc") {
            auto ssc = std::make_unique<SuperSerialCard>(slot);
            if (!iic_)
                (void)factory_.loadSuperSerialFirmware(*ssc, pom2::SystemProfile::AppleIIe);
            card = std::move(ssc);
        } else {
            pom2::SlotCardFactory::Request rq;
            rq.key = key; rq.slot = slot; rq.cpuIsCmos = true;
            auto r = factory_.create(rq);
            if (!r) return false;
            card = std::move(r.card);
        }
        // The window hands Le Chat Mauve the Memory its Eve shadows into
        // (CPREG deposited in aux on text/HGR writes).
        if (auto* lcm = dynamic_cast<LeChatMauveCard*>(card.get())) {
            lcm->setMemory(&mem_);
        }
        card->setCpuClock(kHz);
        card->setStandardClock(kHz);
        mem_.slotBus().plug(slot, std::move(card));
        return true;
    }

    /// Cold start (RAM cleared) into slot 5 and run until the A2osX shell prompt shows, or
    /// `seconds` of emulated time. True when the prompt appeared.
    /// `launch`, when set, is typed at the S-C assembler's `:` prompt — the
    /// BUILD media of A2osX's master stops there, and starts A2osX on
    /// `-A2OSX.SYSTEM`.
    bool boot(int seconds = 40, const std::string& launch = {})
    {
        // A power cycle: RAM is lost (a second boot must not find the old
        // shell prompt on the text page), the disk keeps what was written.
        mem_.clearRam();
        mem_.resetSoftSwitches();
        cpu_.setCpuMode(M6502::CpuMode::CMOS);
        cpu_.hardReset();
        mem_.slotBus().reset();
        if (iic_) mem_.setIicSmartPortArmed(true);
        cpu_.setProgramCounter(0xC500);
        const long budget = static_cast<long>(seconds * kHz);
        bool launched = launch.empty();
        for (long total = 0; total < budget; ) {
            total += cpu_.run(4096);
            if (!launched && (total % (1 << 18)) < 4096
                && screen().find("FOR THE 6502") != std::string::npos) {
                // The banner is up; give the `:` prompt half a second more.
                for (long t = 0; t < static_cast<long>(kHz / 2); ) t += cpu_.run(4096);
                mem_.pasteText(launch + "\r");
                launched = true;
            }
            if ((total % (1 << 18)) < 4096 && atPrompt()) {
                bootSeconds_ = total / kHz;
                return true;
            }
        }
        return false;
    }
    double bootSeconds() const { return bootSeconds_; }

    /// Type `command` + Return at the shell, run `seconds`, and return what
    /// the screen shows after it.
    std::string type(const std::string& command, int seconds = 8)
    {
        mem_.pasteText(command + "\r");
        for (long t = 0; t < static_cast<long>(seconds * kHz); ) t += cpu_.run(4096);
        return screen();
    }

    /// The text page as the beam shows it — 80 columns interleave aux (even)
    /// and main (odd) — one line per row, trailing spaces trimmed.
    std::string screen()
    {
        static const int rowBase[24] = {
            0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
            0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
            0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0 };
        const bool eighty = mem_.getDisplayState().eightyCol;
        const uint8_t* aux = mem_.auxData();
        std::string out;
        for (int r = 0; r < 24; ++r) {
            std::string line;
            for (int c = 0; c < (eighty ? 80 : 40); ++c) {
                const uint16_t a = static_cast<uint16_t>(rowBase[r] + (eighty ? c / 2 : c));
                uint8_t ch = static_cast<uint8_t>(
                    ((eighty && (c & 1) == 0) ? aux[a] : mem_.peekMainRam(a)) & 0x7F);
                if (ch < 0x20) ch = static_cast<uint8_t>(ch + 0x40);   // inverse / MouseText
                line.push_back(static_cast<char>(ch));
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out += line + "\n";
        }
        return out;
    }

    bool atPrompt()
    {
        const std::string t = screen();
        return t.find("root/$") != std::string::npos;
    }

private:
    std::unique_ptr<pom2::NetworkBackend> network()
    {
        auto net = pom2::makeSlirpBackend("pom2test");
        if (net && netLog_) net = std::make_unique<LoggingNetworkBackend>(std::move(net));
        return net;
    }

    bool netLog_ = false;
    bool iic_;
    bool ready_ = false;
    double bootSeconds_ = 0.0;
    Memory mem_;
    M6502  cpu_{&mem_};
    pom2::IWMDevice iwm_;
    pom2::IIcExternalSmartPort port_;
    pom2::SlotCardFactory factory_;
    std::filesystem::path copy_;
};

}  // namespace pom2test

#endif  // POM2_TESTS_A2OSX_HARNESS_H
