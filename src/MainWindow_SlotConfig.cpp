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

// MainWindow_SlotConfig — turning the saved slot configuration into plugged
// cards, and the two card swaps that are not a full re-plug.
//
// `plugSlotsFromSettings` is the composition root for peripherals: it reads
// `slot_N_card`, honours the profile's built-in slots (which override the
// settings and grey out their row in Slot Config), and hands each card the
// runtime seam it cannot build for itself — a SuperSerial transport, the
// W5100's host socket factory and name resolver, a FujiNet link. That
// injection is the layer guard's doing: a DEVICES card reaching into RUNTIME
// to construct its own transport is exactly what the manifests forbid.
//
// It takes a `const pom2::StateAccess&` rather than taking the lock itself,
// because it is called from both locked and unlocked callers and `stateMutex`
// is NON-RECURSIVE. The handle is the caller's proof of ownership.

#include "MainWindow.h"

#include "AudioCoordinator.h"
#include "AudioDevice.h"
#include "CffaCard.h"
#include "DevicePanelCoordinator.h"
#include "NetworkCoordinator.h"
#include "PrinterSoundDevice.h"
#include "SerialPort.h"
#include "SpTransport.h"
#include "SpOverSlipLink.h"
#include "StorageCoordinator.h"
#include "ClockCard.h"
#include "DiskController_ImGui.h"
#include "Disk35Controller_ImGui.h"
#include "DiskIICard.h"
#include "HdvController_ImGui.h"
#include "SmartPort_ImGui.h"
#include "FujiNet_ImGui.h"
#include "FloppyEmu_ImGui.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "EmulationController.h"
#include "FujiNetCard.h"
#include "FujiNetCardFactory.h"
#include "FourPlayCard.h"
#include "TranswarpCard.h"
#include "GrapplerCard.h"
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseCoordinator.h"
#include "NetworkBackend.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlirpNetworkBackend.h"
#include "SlotCardCatalog.h"
#include "SlotCardFactory.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotRebuildCoordinator.h"
#include "SmartPort35Unit.h"
#include "LironCard.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SuperSerialTransport.h"
#include "SystemProfile.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "W5100HostSockets.h"
#include "W5100NameResolver.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

bool MainWindow::setChatMauveInvertBit7(bool v)
{
    // Resolves the card under the lock, writes, unlocks, then persists.
    return devicePanelCoordinator_->setChatMauveInvertBit7(v);
}

void MainWindow::plugSlotsFromSettings(const pom2::StateAccess& st)
{
    namespace fs = std::filesystem;

    // The fresh-install default map (grappler / mouseaw / — / mockingboard /
    // smartport35 / diskii / chatmauve, slot 3 deliberately empty because the
    // //e's 80 columns are internal $C300 ROM + the AUX connector, not a slot
    // card) now lives in SlotConfigurationCoordinator, next to the settings
    // lookup that falls back to it. CLAUDE.md documents the map itself.

    // The effective plan: settings defaults, the legacy `clock_card_enable`
    // opt-out, profile-forced built-in slots, the //c-class no-physical-slots
    // rule and the single-instance policy, resolved in one place.
    //
    // What it is NOT is a record of what ended up plugged. The plan holds what
    // the user asked for; a missing ROM or a session-only auto-provisioned
    // card does not rewrite it, so a CFFA whose firmware is absent today is
    // still a CFFA in the config tomorrow. The live SlotBus is the authority
    // on what is actually there, and the panel reads it separately.
    const auto& plan = slotConfigCoordinator_->resolve(*settings, activeProfile);
    for (int s = 1; s <= 7; ++s) slotCards[s] = plan[s];

    // ── Per-card construction helpers. Each one populates the matching
    //    raw `*Card` member pointer (non-owning) for the rest of MainWindow
    //    to find, and plugs the card into the SlotBus. ────────────────

    // Read once for every factory Request below: the CFFA firmware comes in
    // an NMOS and a 65C02 variant and the factory picks by this.
    const bool cpuIsCmosForSlots =
        st.cpu().getCpuMode() == M6502::CpuMode::CMOS;

    auto plugDiskII = [&](int s) {
        // Construction + every ROM lookup belongs to the factory: four
        // optional PROMs (boot, P6 LSS, and the 13-sector pair) each had their
        // own hand-rolled {roms/, ../roms/, ../../roms/} candidate loop here,
        // which is what `pom2::findResource` already does.
        auto made = slotCardFactory_->create(
            { "diskii", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        diskRomPath   = made.resourcePath;
        diskRomStatus = made.status;
        auto* card = static_cast<DiskIICard*>(made.card.get());

        // Runtime wiring stays here — it is composition, not construction.
        // Sub-instruction cycle accuracy on MMIO: cycle-precise copy
        // protections read the LSS state at the exact sub-cycle of the data
        // fetch, not at instruction start (DiskIICard::setCpu).
        card->setCpu(&st.cpu());
        card->setFloppySound(&controller->floppySound525());
        // //c+ on-board IWM — only the slot-6 card pushes its drive pointer to
        // the IWM. A second Disk II (slot 4, say) stays off that path so it
        // cannot clobber the //c+ flux mirror.
        if (s == 6) card->setIWM(&controller->iwm());

        // Media and write-back are NOT restored here. This builds empty
        // hardware; StorageCoordinator::restoreMediaFromSettings() runs once
        // at the end of this function against the finished topology, because
        // "is this the primary card" is a property of the whole bus.
        diskPanels.push_back(std::make_unique<pom2::DiskController_ImGui>());
        if (!diskPanel) diskPanel = diskPanels.front().get();
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugHdv = [&](int s) {
        // Empty hardware only; the image and write-back opt-in arrive in the
        // restore pass at the end of this function. `hdvPath` is seeded from
        // settings here purely so the panel has something to show before that
        // pass runs — an empty `hdv_path` means "nothing mounted", not "scan
        // hdv/ and pick one", which is what it used to mean and what silently
        // re-mounted a hard disk the user had just ejected.
        const std::string saved = settings->getString("hdv_path", "");
        std::error_code ec;
        if (!saved.empty() && fs::is_regular_file(saved, ec)) hdvPath = saved;

        auto made = slotCardFactory_->create(
            { "hdv", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugCffa = [&](int s) {
        // The factory picks the firmware variant by CPU — the CFFA 2.0 ROM
        // ships in an NMOS and a 65C02 build — and falls back to the other if
        // only one is present. A missing or unloadable ROM clears the slot:
        // a CFFA with no firmware is not a card, it is a hole at $Cn00.
        auto made = slotCardFactory_->create(
            { "cffa", s, cpuIsCmosForSlots, activeProfile });
        if (!made) {
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            // The slot stays empty, but the PLAN keeps the user's request.
            // Clearing it here meant a CFFA whose ROM was missing today was
            // silently deleted from the config and gone tomorrow; the panel
            // now reports plan-vs-live divergence instead.
            return;
        }
        // Empty hardware only — the per-slot image and write-back arrive in
        // the restore pass at the end of this function.
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugChatMauve = [&](int s) {
        // Which Chat Mauve: a //c-class machine has the rear DB-15 adapter,
        // a slotted machine defaults to the Féline; `chatmauve_variant`
        // (feline | iic | eve | video7) overrides either.
        using Variant = LeChatMauveCard::Variant;
        // A //c-class machine has no aux slot: its DB-15 takes the
        // Adaptateur IIc and nothing else — there the model is hardware,
        // not a setting. Slotted profiles read `chatmauve_variant` (the
        // Slot Config window's "model" combo, or the Chat Mauve panel).
        Variant variant = Variant::Feline;
        if (pom2::profileConfig(activeProfile).noPhysicalSlots) {
            variant = Variant::IIcAdapter;
        } else if (settings) {
            Variant parsed;
            if (LeChatMauveCard::parseVariant(
                    settings->getString("chatmauve_variant", ""), parsed))
                variant = parsed;
        }
        auto card = std::make_unique<LeChatMauveCard>(s, variant);
        // Local, not a retained alias: the display genuinely needs the card
        // for its RGB decode path and is re-pointed on every rebuild.
        LeChatMauveCard* plugged = card.get();
        if (settings)
            plugged->setInvertBit7(
                settings->getBool("chatmauve_invert_bit7", false));
        // The Eve writes aux memory behind the CPU (CPREG auto-write): the
        // card programs Memory's aux shadow from its switches.
        plugged->setMemory(&st.memory());
        st.memory().slotBus().plug(s, std::move(card));
        display->setChatMauveCard(plugged);
    };

    auto plugSsc = [&](int s) {
        auto card = std::make_unique<SuperSerialCard>(s);
        SuperSerialCard* raw = card.get();
        // Use pasteText (not queueKey) — pasteText respects the paste
        // queue, so a stream of bytes from telnet doesn't clobber earlier
        // characters that BASIC hasn't picked up yet.
        raw->setKeyboardSink(
            [&mem = st.memory()](uint8_t b) {
                const char buf[1] = { static_cast<char>(b) };
                mem.pasteText(buf, 1);
            });
        // IRQ routing is auto-wired by SlotBus's installed router (see
        // Memory::setCpu) — no per-card setup needed.
        st.memory().slotBus().plug(s, std::move(card));
        // No alias list: serialCards() reads the bus, slot-ascending, so the
        // lowest-slot card is the primary exactly as before. The card is
        // already plugged above, so it is visible to that read.
        // Per-slot persistence; fall back to legacy global keys (the
        // primary SSC was the only one before //c dual-port support).
        const std::string sk = "_slot" + std::to_string(s);
        const bool legacyPrimary = (raw == primarySerialCard());
        raw->setRawMode(settings->getBool(
            "ssc_raw_mode" + sk,
            legacyPrimary ? settings->getBool("ssc_raw_mode", false) : false));
        // Printer tap: slot 1 is the printer-port convention (the //c
        // hard-wires it), so the tap defaults ON there — a //c user gets
        // PR#1 landing on the ImageWriter with zero configuration.
        raw->setPrinterTap(settings->getBool("ssc_printer_tap" + sk, s == 1));
        // Give the card its host transport at plug time, listening or not.
        // The card cannot build one itself — that would be a device reaching
        // into runtime — and `startListening` refuses outright without one,
        // so injecting it only when the saved state was already "listening"
        // left the Super Serial panel's "Start listener" button permanently
        // dead (it reported "bind failed (port busy?)" for a socket that was
        // never created). Constructing the transport opens nothing; the
        // socket and its worker thread appear on start().
        raw->setTransport(pom2::makeSuperSerialTcpTransport(*raw, s));
        const bool listenDefault = legacyPrimary
            ? settings->getBool("ssc_listening", false) : false;
        if (settings->getBool("ssc_listening" + sk, listenDefault)) {
            const int portDefault = legacyPrimary
                ? settings->getInt("ssc_port", SuperSerialCard::kDefaultPort)
                : SuperSerialCard::kDefaultPort;
            const int p = settings->getInt("ssc_port" + sk, portDefault);
            raw->startListening(static_cast<uint16_t>(p));
        }
    };

    auto plugClock = [&](int s) {
        auto card = std::make_unique<ClockCard>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugSoftCard = [&](int s) {
        // Microsoft SoftCard (Z80 DMA bus master). No ROM to probe — the
        // hardware has none. The card needs the real bus (soft-switch
        // side effects, LC paging) and the 6502 so its $CnXX toggle can
        // halt the in-flight run() chunk at an instruction boundary.
        auto card = std::make_unique<SoftCardZ80>();
        card->setMemory(&st.memory());
        card->setCpu(&st.cpu());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugPrinter = [&](int s) {
        auto card = std::make_unique<PrinterCard>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    // Both Ethernet cards share one host-transport decision, so the
    // backend factory lives here rather than in either plug lambda.
    // Settings key `ethernet_backend`: "slirp" (default) | "loopback" |
    // "none". Loopback is a self-test mode — everything the guest
    // transmits comes straight back — and is also the honest fallback
    // when a user explicitly wants the cards inert.
    auto makeEthernetBackend = [&](const char* who)
        -> std::unique_ptr<pom2::NetworkBackend> {
        const std::string choice =
            settings->getString("ethernet_backend", "slirp");
        if (choice == "loopback")
            return std::make_unique<pom2::LoopbackNetworkBackend>();
        if (choice == "none")
            return std::make_unique<pom2::NullNetworkBackend>();

        if (!pom2::slirpAvailable()) {
            pom2::log().warn(who,
                "libslirp not compiled in — no host Ethernet transport. "
                "Uthernet II TCP/UDP still works; install libslirp-dev and "
                "rebuild for raw-frame modes and the Uthernet I.");
            return std::make_unique<pom2::NullNetworkBackend>();
        }
        auto slirp = pom2::makeSlirpBackend("pom2");
        if (!slirp) {
            pom2::log().warn(who, "libslirp failed to start — falling back "
                                  "to no host transport");
            return std::make_unique<pom2::NullNetworkBackend>();
        }
        return slirp;
    };

    auto plugUthernet = [&](int s) {
        // a2RetroSystems Uthernet I — CS8900A NIC, raw Ethernet only.
        // Without a working transport the card still plugs and probes
        // (drivers detect it via the PacketPage ProductID), it just never
        // sees a frame — which is a better failure mode than hiding it.
        auto card = std::make_unique<pom2::UthernetCard>(s);
        card->setBackend(makeEthernetBackend("Uthernet"));
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugUthernetII = [&](int s) {
        // a2RetroSystems Uthernet II — W5100 hardware TCP/IP. Its TCP and
        // UDP sockets are host sockets, so this card is fully functional
        // with no backend at all; the backend only serves MACRAW/IPRAW.
        auto card = std::make_unique<pom2::UthernetIICard>(s);
        // Inject the host socket factory: the W5100 cannot build one itself,
        // and without it its TCP/UDP modes stay CLOSED.
        card->chip().setSocketFactory(pom2::makeHostW5100SocketFactory());
        card->chip().setNameResolver(
            std::make_unique<pom2::W5100NameResolver>());
        card->setBackend(makeEthernetBackend("UthernetII"));
        // Virtual DNS is an emulator extension (not on real silicon) that
        // lets a guest connect by hostname without carrying a resolver.
        // On by default, matching AppleWin, and detectable by software as
        // PTIMER == 0.
        card->chip().setVirtualDnsEnabled(
            settings->getBool("uthernet2_virtual_dns", true));
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugFujiNet = [&](int s) {
        // FujiNet relay. The card itself is inert until the link finds a
        // peer, and finding one is asynchronous, so plugging always succeeds
        // — a machine with this card and no FujiNet running behaves like a
        // machine with an empty drive, not a broken one.
        auto card = pom2::makeFujiNetCard(s);
        card->setMemory(&st.memory());
        card->setCpu(&st.cpu());

        const std::string sk = "_slot" + std::to_string(s);
        auto& link = card->transportLink();
        link.setTimeoutMs(settings->getInt("fujinet_timeout_ms" + sk,
                                           pom2::FujiNetTransport::kDefaultTimeoutMs));

        // Built-in N:, on by default. The FujiNet desktop build's own network
        // device answers the guest's open with success and then never opens a
        // socket, so relaying faithfully to it means the guest can never
        // fetch anything; POM2 serving N: itself is the difference between a
        // machine that browses and one that does not. Everything else still
        // goes to the peer. Set `fujinet_builtin_network<slot> = false` for a
        // real FujiNet board over USB, whose N: works and does far more than
        // plain HTTP.
        card->setBuiltInNetwork(
            settings->getBool("fujinet_builtin_network" + sk, true));

        const std::string transport =
            settings->getString("fujinet_transport" + sk, "tcp");
        if (transport == "serial") {
            link.setSerialMode(
                settings->getString("fujinet_serial_path" + sk, ""),
                settings->getInt("fujinet_serial_baud" + sk,
                                 pom2::SerialPort::kDefaultBaud));
        } else {
            link.setTcpMode(static_cast<uint16_t>(
                settings->getInt("fujinet_port" + sk,
                                 pom2::SpTcpTransport::kDefaultPort)));
        }

        // The link is STARTED after this function returns, not here: start()
        // opens a TCP listener or a serial tty — syscalls that can block —
        // and this whole function runs inside the caller's `stateMutex`
        // scope, the lock the CPU worker takes every 4096 cycles and the UI
        // thread takes to paint. It ran on every profile switch and every
        // Slot Config Apply. See `startDeferredFujiNetLinks`, which the three
        // callers run once the lock is released and while the worker is still
        // stopped.
        if (settings->getBool("fujinet_enabled" + sk, true))
            pendingFujiNetSlots_.push_back(s);

        st.memory().slotBus().plug(s, std::move(card));

        // setHelperPath resolves against PATH as well: a configured name the
        // host cannot find is what the panel must show as unresolved.
        networkCoordinator_->setHelperPath(
            settings->getString("fujinet_helper_path" + sk, ""));
    };

    auto plugPhasor = [&](int s) {
        // Applied Engineering Phasor. Same MMIO surface as a Mockingboard
        // plus a mode soft-switch at $C0(8+s)X that flips between MB-
        // compat (1 AY per VIA) and Phasor-native (2 AYs per VIA × 2 VIAs
        // = 4 chips, 12 voices). Audio synth is a v1 placeholder — the
        // card detects + responds to MMIO correctly but emits silence
        // until the 4-AY mix lands (TODO 🟡 [Phasor] audio synth).
        auto card = std::make_unique<PhasorCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&st.cpu());
        card->setVolume(settings->getFloat("phasor_volume", 0.5f));
        card->setMuted (settings->getBool ("phasor_muted",  false));
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlus = [&](int s) {
        // Street Electronics Echo+ — standalone SSI263 speech synth.
        // No PROM, no ROM dependency, audio is silent in v1 (chip
        // model complete but phoneme PCM blob deferred to a separate
        // commit pending license review of AppleWin's data).
        auto card = std::make_unique<EchoPlusCard>(s);
        card->setSampleRate(controller->audio().getActualSampleRate());
        card->setCpu(&st.cpu());
        card->setVolume(settings->getFloat("echoplus_volume", 0.7f));
        card->setMuted (settings->getBool ("echoplus_muted",  false));
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugEchoPlusTms = [&](int s) {
        // Street Electronics Echo+ AS ACTUALLY SHIPPED — 2×AY-3-8913 +
        // TMS5220. Scaffold only: register decode is present so software
        // detects the card, but the LPC core + AY synth are deferred.
        // Audio is silent. See EchoPlusTMS5220Card.h for the chipset
        // sourcing notes.
        auto card = std::make_unique<EchoPlusTMS5220Card>(s);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugTranswarp = [&](int s) {
        // TransWarp accelerator. Its ROM shadow needs Memory (a 4 KB swap
        // in the $F000 mirror) and is ROM-gated; the acceleration itself
        // works without the dump, so a missing ROM is a warning, not a
        // refusal to plug.
        auto card = std::make_unique<pom2::TranswarpCard>(s);
        card->setMemory(&st.memory());
        card->setDsw1(static_cast<uint8_t>(settings->getInt(
            "transwarp_dsw1", pom2::TranswarpCard::kDsw1Default)));
        card->setDsw2(static_cast<uint8_t>(settings->getInt(
            "transwarp_dsw2", pom2::TranswarpCard::kDsw2Default)));
        const std::string why = card->loadRomFromDisk();
        if (!why.empty()) pom2::log().warn("TransWarp", why);
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugFourPlay = [&](int s) {
        // 4play — four digital joysticks. No ROM, no state; the host pads
        // are pushed in from pollJoystickAndPushToMemory().
        st.memory().slotBus().plug(s, std::make_unique<pom2::FourPlayCard>(s));
    };

    auto plugWorkstation = [&](int s) {
        // Apple II Workstation Card. Hard ROM gate in the factory: no
        // firmware, no card. It runs a second 65C02 at the Apple II's own
        // rate, so plugging it roughly doubles the emulation work — that is
        // what a coprocessor board costs, not a defect.
        auto made = slotCardFactory_->create(
            { "workstation", s, cpuIsCmosForSlots, activeProfile });
        if (!made.warning.empty())
            pom2::log().warn(made.warningCategory.c_str(), made.warning);
        if (!made) return;
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugGrappler = [&](int s) {
        // Orange Micro Grappler+ — ROM-gated parallel printer. Loads
        // roms/grappler_plus.bin if present; falls back to a synthetic
        // stub ROM (PR#n trampoline only) so the card always plugs.
        auto made = slotCardFactory_->create(
            { "grappler", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        if (!made.warning.empty())
            pom2::log().warn(made.warningCategory.c_str(), made.warning);
        auto* card = static_cast<GrapplerCard*>(made.card.get());
        // S1 printer-type DIP. Default = Apple Dot Matrix / ImageWriter:
        // POM2's printer IS an ImageWriter II, and MAME's Epson default
        // makes the firmware emit Epson escape codes that this printer
        // renders as garbage (same as flipping the switches wrong on a
        // real desk).
        card->setPrinterType(static_cast<GrapplerCard::PrinterType>(
            settings->getInt("grappler_printer_type",
                static_cast<int>(GrapplerCard::PrinterType::AppleDotMatrix))));
        card->setMsbSoftwareControl(
            settings->getBool("grappler_msb_software", true));
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugMockingboard = [&](int s, MockingboardCard::Variant variant) {
        // Mockingboard A/C — 6522×2 + AY-3-8910×2. No ROM dependency, no
        // image to mount: software detects it by writing to the VIA at
        // $C(s)00 and observing the read-back. We always-plug when
        // requested. The inner AudioSource is registered with the audio
        // device so synthesised samples mix with the speaker output, and
        // the CPU IRQ line is wired so VIA T1 can drive the music
        // driver's tick.
        //
        // Variant::SoundII additionally adds an SSI263 speech synth at
        // $C(s)40-$C(s)44 with A/!R wired to VIA1.CA1 → IFR.CA1 →
        // (gated by IER.CA1) slot IRQ. Drivers configure PCR.0=0 for
        // negative-edge detection on the inverted A/!R wiring.
        auto card = std::make_unique<MockingboardCard>(s, variant);
        card->setSampleRate(controller->audio().getActualSampleRate());
        // The emulated CPU clock (for the audio thread's emuCycles replay
        // cursor) is applied to EVERY plugged card at the end of this
        // function — see the fan-out there.
        // CPU pointer feeds the lazy-sync timer back-channel
        // (getCycleCountNow); IRQ routing is auto-wired via SlotBus.
        card->setCpu(&st.cpu());
        // Default volume is conservative — the card's three-channel mix
        // can dwarf the speaker at peak; the user can crank via the
        // Mockingboard panel (TODO).
        card->setVolume(settings->getFloat("mockingboard_volume", 0.5f));
        card->setMuted(settings->getBool ("mockingboard_muted",  false));
        registerAudioSource(card->audioSource());
        st.memory().slotBus().plug(s, std::move(card));
    };

    auto plugSmartPort35 = [&](int s) {
        // Liron-class card. Each unit's type + image is restored from
        // settings (smartport_slotN_unitK_*) so per-card mixes (e.g.
        // unit 0 = 3.5", unit 1 = HDV) survive across launches. When
        // no setting exists, both units start empty — the user picks
        // a type via the SmartPort Configuration panel.
        // The factory owns the Liron ROM rule: the real identity
        // (roms/liron.rom, BMOW dump) goes on slot-having machines only —
        // NEVER on //c-class, whose on-board $C500 stub must keep the
        // synthetic $Cn07=$01 ProDOS-block identity, because a
        // SmartPort-class byte there re-triggers the boot-scan confusion
        // (project_iic_smartport_boot). That is why Request carries the
        // profile.
        auto made = slotCardFactory_->create(
            { "smartport35", s, cpuIsCmosForSlots, activeProfile });
        if (!made) return;
        auto* card = static_cast<pom2::SmartPortCard*>(made.card.get());
        // Mechanical sound: route to the dedicated 3.5" sound bank. Block-level
        // transfers only — the card synthesises step / motor / click events
        // from READBLOCK / WRITEBLOCK directly. Wiring, so it stays here.
        card->setFloppySound(&controller->floppySound35());

        // Units are NOT built here. The card is plugged empty and
        // StorageCoordinator::restoreMediaFromSettings() creates each unit
        // from its persisted kind, applies the write-back opt-in and resolves
        // the image path against the same cwd anchors, once the whole topology
        // exists. Keyspace unchanged:
        //   smartport_slotN_unitK_type      ("" / "35" / "hdv")
        //   smartport_slotN_unitK_path      (image path, optional)
        //   smartport_slotN_unitK_writeback (bool)
        // No alias to seed: primarySmartPortCard() reads the bus.
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugLiron = [&](int s) {
        // The same controller as silicon: real EPROM, real IWM, its drives
        // answered as intelligent devices on the SmartPort bus. The factory
        // gates it on roms/liron.rom and leaves the slot empty otherwise.
        auto made = slotCardFactory_->create(
            { "liron", s, cpuIsCmosForSlots, activeProfile });
        if (!made) {
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            return;
        }
        auto* card = static_cast<pom2::LironCard*>(made.card.get());
        card->setFloppySound(&controller->floppySound35());
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    auto plugMouse = [&](int s) {
        // MAME-faithful 68705P3 + 6821 PIA. Both Apple ROMs are required —
        // without them the card has no firmware and refuses to plug, which is
        // why the slot is left empty rather than half-built.
        auto made = slotCardFactory_->create(
            { "mouse", s, cpuIsCmosForSlots, activeProfile });
        mouseRomStatus = made.status;
        if (!made) {
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            return;
        }
        // IRQ routing is auto-wired by SlotBus (Memory::setCpu): the MCU's
        // PB6 reaches the CPU through SlotPeripheral::assertIrq, which fans
        // out via M6502::setIrqLine(slot, …).
        st.memory().slotBus().plug(s, std::move(made.card));
    };

    // Dispatch: walk slots 1..7 and plug whichever card the settings ask
    // for. Anything we don't recognise is logged and skipped.
    for (int s = 1; s <= 7; ++s) {
        const std::string& kind = slotCards[s];
        if      (kind.empty())          continue;
        else if (kind == "diskii")      plugDiskII(s);
        else if (kind == "hdv")         plugHdv(s);
        else if (kind == "cffa")        plugCffa(s);
        else if (kind == "ssc")         plugSsc(s);
        else if (kind == "printer")     plugPrinter(s);
        else if (kind == "clock")       plugClock(s);
        else if (kind == "softcard")    plugSoftCard(s);
        else if (kind == "chatmauve")   plugChatMauve(s);
        else if (kind == "mouse")       plugMouse(s);
        else if (kind == "mouseaw") {
            // The factory owns the fallback: if the AppleWin HLE slot EPROM
            // is missing or will not load, it builds the MC68705 "mouse" card
            // instead and reports `fallback` with the reason, rather than
            // leaving the slot empty. MODE_INT_VBL pacing follows the
            // machine's VIDEO frame — scanlinesPerFrame × cyclesPerScanline
            // (17030 NTSC / 20280 PAL), not the worker's cyclesPerFrame
            // budget — which is why the Request carries the profile.
            auto made = slotCardFactory_->create(
                { "mouseaw", s, cpuIsCmosForSlots, activeProfile });
            mouseRomStatus = made.status;
            if (!made.warning.empty())
                pom2::log().warn(made.warningCategory.c_str(), made.warning);
            if (!made) continue;
            // A fallback is a LIVE fact, not a configuration change: the
            // user still asked for "mouseaw", and the ROM may be there next
            // launch. The panel reads the live snapshot to show what is
            // actually plugged.
            st.memory().slotBus().plug(s, std::move(made.card));
        }
        else if (kind == "mockingboard")   plugMockingboard(s, MockingboardCard::Variant::AC);
        else if (kind == "mockingboard_c") plugMockingboard(s, MockingboardCard::Variant::SoundII);
        else if (kind == "phasor")      plugPhasor(s);
        else if (kind == "echoplus")    plugEchoPlus(s);
        else if (kind == "echoplus_tms") plugEchoPlusTms(s);
        else if (kind == "grappler")    plugGrappler(s);
        else if (kind == "workstation") plugWorkstation(s);
        else if (kind == "4play")       plugFourPlay(s);
        else if (kind == "transwarp")   plugTranswarp(s);
        else if (kind == "uthernet")    plugUthernet(s);
        else if (kind == "uthernet2")   plugUthernetII(s);
        else if (kind == "smartport35") plugSmartPort35(s);
        else if (kind == "liron")       plugLiron(s);
        else if (kind == "fujinet")     plugFujiNet(s);
        else {
            pom2::log().warn("Slots",
                "Slot " + std::to_string(s) + " has unknown card type '" +
                kind + "' — leaving empty");
        }
    }

    // The printer's mechanical sound is not owned by any card, but it IS
    // swept away with the card-owned ones: both rebuild paths call
    // unregisterAllAudioSources() before getting here. Re-registering it on
    // every rebuild is what keeps it alive across a profile switch and a Slot
    // Config "Apply" — registerAudioSource() is idempotent, so the
    // constructor's first pass through here simply arms it.
    registerAudioSource(printerSound.get());

    // Re-apply a `--fujinet` card requested on the command line.
    //
    // It is deliberately not in the settings file (a one-shot CLI card must
    // not leak into the user's saved slot config), so the re-seed at the top
    // of this function has just erased it. Doing it here means applyProfile's
    // step 7 reproduces the card, which matters twice: `--preset` no longer
    // destroys it moments after the CLI logged success, and because step 7
    // runs BEFORE step 11's cold boot, the autostart scan still finds a
    // FujiNet on its first pass.
    if (cliFujiNetSlot_ > 0 &&
        !pom2::profileConfig(activeProfile).noPhysicalSlots) {
        const int s = cliFujiNetSlot_;
        if (st.memory().slotBus().peripheral(s) != nullptr) {
            pom2::log().warn("CLI", "--fujinet: slot " + std::to_string(s) +
                                        " is taken after the rebuild — card "
                                        "not restored");
        } else {
            std::string err;
            // startNow = false: same reason as the settings-driven card
            // above — the transport is opened once this lock is released.
            if (!plugFujiNetUnlocked(st, s, cliFujiNetSerial_,
                                     cliFujiNetSerialPath_, cliFujiNetPort_,
                                     err, /*startNow=*/false))
                pom2::log().warn("CLI", "--fujinet: " + err);
            else
                pendingFujiNetSlots_.push_back(s);
        }
    }

    // ── Phase 2: media, once the whole topology exists ────────────────────
    // Everything above builds EMPTY hardware. Only now can "is this the
    // primary Disk II / HDV" be answered, which is what the legacy `disk_path`
    // and `disk_writeback` keys fall back on — asking it while the bus was
    // half-built is what made a moved primary HDV and the second Disk II drive
    // restore against the wrong card.
    //
    // Runs under the caller's lock (this function takes a StateAccess to prove
    // it), so it does its file reads there, exactly as the per-card restores
    // it replaces did. That is the documented profile-switch exception in
    // MainWindow_Slots.cpp: the CPU worker is stopped across a rebuild anyway.
    const auto restored =
        storageCoordinator_->restoreMediaFromSettings(st.memory().slotBus(),
                                                      *settings);
    for (const std::string& warning : restored.warnings)
        pom2::log().warn("Storage", warning);

    // ── Emulated CPU clock, to every card that takes one ─────────────────
    // `SlotPeripheral::setCpuClock` is a virtual with a no-op default, and
    // four cards override it (Mockingboard, Phasor, ClockCard,
    // WorkstationCard). Only the Mockingboard's plug function set it, so a
    // Phasor / clock card / Workstation card added through Slot Config Apply
    // — which re-plugs cards WITHOUT re-running the profile's video-standard
    // step — kept the NTSC default on a PAL machine: the AY queues starve
    // against a 0.7 % wrong clock, and the ThunderClock's bit-bang half
    // period is measured in CPU cycles.
    {
        const double cpuHz = static_cast<double>(
            pom2VideoTiming(controller->getVideoStandard()).cpuClockHz);
        for (int s = 1; s < SlotBus::kSlotCount; ++s) {
            if (auto* card = st.memory().slotBus().peripheral(s))
                card->setCpuClock(cpuHz);
        }
    }

    // The HDV panel's status line is derived, not remembered.
    if (auto* hdv = primaryHdvCard()) {
        hdvStatus = hdv->isImageLoaded()
                        ? std::string("loaded: ") + hdv->getImagePath()
                        : "no image mounted";
        hdvPath = hdv->isImageLoaded() ? hdv->getImagePath() : std::string();
    } else {
        hdvStatus = "no image mounted";
    }
}

bool MainWindow::swapSlotCardVariant(const char* fromKey, const char* toKey)
{
    // In place, in the slot the card already occupies: the two keys are the
    // same peripheral at two abstraction levels, so moving it would be a
    // second, unasked-for change (and would break any software that has the
    // slot number baked in, which for a mouse or a printer is most of it).
    int slot = -1;
    for (int s = 1; s <= 7; ++s)
        if (slotCards[s] == fromKey) { slot = s; break; }
    if (slot < 0) return false;

    const std::string key      = "slot_" + std::to_string(slot) + "_card";
    const std::string previous = settings->getString(key, "");

    // `slotCards[]` is the RESOLVED plan, so on a profile-forced slot it holds
    // the PROFILE's card, not the user's saved choice — writing the swap back
    // there clobbers an unrelated //e-era card key (the //c on-board Mouse in
    // slot 4 over a saved `mockingboard`), and the swap would not even take
    // live effect because the rebuild re-forces the built-in. Same guard as
    // the ~MainWindow persist loop and Slot Config's Apply.
    if (!pom2::slotKeyIsUserChoice(pom2::profileConfig(activeProfile), slot,
                                   toKey, previous)) {
        tapeStatusMessage = std::string("Slot ") + std::to_string(slot) +
                            " is a built-in of this profile — " + fromKey +
                            " cannot be swapped for " + toKey + " here.";
        tapeStatusUntil   = lastFrameTime + 6.0;
        pom2::log().warn("Abstraction", tapeStatusMessage);
        return false;
    }

    settings->setString(key, toKey);
    if (!settings->save()) {
        settings->setString(key, previous);
        tapeStatusMessage = "Could not save the slot change.";
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    if (!restartEmulationFromSettings()) {
        // Rebuild refused — the live machine was deliberately left intact, so
        // the persisted key has to go back too or the refused change would
        // apply silently on the next launch. Same contract as Slot Config's
        // Apply. Say so: the panel's radio snaps back to the old side next
        // frame, and an unexplained snap-back reads as a dead control.
        settings->setString(key, previous);
        settings->save();
        tapeStatusMessage = std::string("Could not rebuild the machine with ") +
                            toKey + " — kept " + fromKey + ".";
        tapeStatusUntil   = lastFrameTime + 6.0;
        return false;
    }
    pom2::log().info("Abstraction",
                     "slot " + std::to_string(slot) + ": " + fromKey +
                     " -> " + toKey);
    return true;
}
