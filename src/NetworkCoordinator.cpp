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

#include "NetworkCoordinator.h"

#include "ChildProcess.h"
#include "EmulationController.h"
#include "FujiNetCard.h"
#include "Memory.h"
#include "SerialPort.h"
#include "SlotBus.h"
#include "FujiNetTransport.h"

namespace pom2 {
namespace {

/// Resolve the FujiNet card from the live bus. Single-instance on purpose:
/// the card holds a listening socket or an open serial device, and a second
/// one would just fail to bind the same endpoint.
FujiNetCard* findFujiNet(SlotBus& bus)
{
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<FujiNetCard*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

} // namespace

FujiNet_ImGui::Snapshot NetworkCoordinator::captureFujiNetPanel(
    EmulationController& controller) const
{
    FujiNet_ImGui::Snapshot snap;
    {
        auto state = controller.lockState();
        auto* card = findFujiNet(state.memory().slotBus());
        if (card) {
            snap.plugged = true;
            // Two surfaces, deliberately bound separately: `transport` is
            // how the host drives the link (mode, worker, counters), `link`
            // is the SmartPort protocol the card speaks over it. Connection
            // and the device list belong to the protocol side.
            const auto& transport = card->transportLink();
            const auto& link      = card->link();
            snap.slot      = card->getSlot();
            snap.transport =
                transport.mode() == FujiNetTransport::Mode::Serial
                    ? FujiNet_ImGui::Transport::Serial
                : transport.mode() == FujiNetTransport::Mode::Tcp
                    ? FujiNet_ImGui::Transport::Tcp
                    : FujiNet_ImGui::Transport::Off;
            snap.running    = transport.isRunning();
            snap.connected  = link.isConnected();
            snap.state      = transport.describe();
            snap.lastError  = transport.lastError();
            snap.tcpPort    = transport.tcpPort();
            snap.serialPath = transport.serialPath();
            snap.serialBaud = transport.serialBaud();
            snap.timeoutMs  = transport.timeoutMs();
            for (const auto& d : link.devices()) {
                FujiNet_ImGui::DeviceRow row;
                row.unit    = d.unit;
                row.name    = d.name;
                row.type    = d.type;
                row.subtype = d.subtype;
                row.blocks  = d.blocks;
                snap.devices.push_back(row);
            }
            const auto st   = transport.stats();
            snap.calls      = st.calls;
            snap.timeouts   = st.timeouts;
            snap.stale      = st.stale;
            snap.bytesIn    = st.bytesIn;
            snap.bytesOut   = st.bytesOut;
            snap.localCalls    = card->localCount();
            snap.printerTap    = card->hasPrinterUnit();
            snap.printerBytes  = card->bytesWritten();
            snap.helperRunning  = card->helper().isRunning();
            snap.helperExitCode = card->helper().lastExitCode();
        }
    }

    // Host-side, and deliberately outside the lock: none of it is machine
    // state, and the status line is this coordinator's own.
    if (snap.plugged && snap.lastError.empty()) snap.lastError = status_;
    snap.serialDevices  = serialDevices_;
    snap.helperPath     = helperPath_;
    snap.helperResolved = helperResolved_;
    return snap;
}

void NetworkCoordinator::applyFujiNetPanel(
    EmulationController& controller, const FujiNet_ImGui::Result& r)
{
    // Host-side first: scanning /dev must not happen under the machine lock.
    if (r.requestRescan) rescanSerialDevices();
    if (r.helperPathChanged) setHelperPath(r.helperPathTo);

    // The card is resolved ONCE, under the lock, and the two host objects it
    // owns — the link and the helper process — are then driven with the lock
    // released. The code this replaces resolved the card afresh for each of
    // six separate critical sections (a slot rebuild between any two of them
    // left the rest writing to a freed link) and applied the timeout change
    // with no lock at all; one acquisition fixed that, and then held the lock
    // across two teardowns that can take seconds. Why carrying the pointers
    // out is safe is spelled out below.
    const bool touchesCard =
        r.transportChanged || r.portChanged || r.serialChanged ||
        r.timeoutChanged || r.requestStart || r.requestStop ||
        r.requestDropPeer || r.requestHelperStart || r.requestHelperStop ||
        r.requestHelperRestart;
    if (touchesCard) {
        std::string startError;
        bool startFailed = false;
        std::string helperMessage;

        // Stopping the helper is not the only SLOW thing in this function:
        // ChildProcess::stop() signals the helper's process group and then
        // polls for the whole 2 s grace — deliberately, so a FujiNet flushing
        // an SD-card image gets to finish. Under the machine lock that is two
        // seconds with the CPU worker blocked on its next chunk and the UI
        // thread blocked trying to paint: exactly the freeze CLAUDE.md forbids,
        // cancel button included. So the stop happens BETWEEN the two critical
        // sections below, and only the sub-millisecond spawn stays under the
        // lock.
        //
        // Carrying the helper — and the link — across that gap is safe. Both
        // are host state whose lifetime is the CARD's, the ChildProcess is
        // never touched by the CPU thread at all, and SlotBus topology is
        // UI-thread-confined: the only thread that can unplug the card is the
        // one standing right here, between these two statements.
        //
        // The link needs one thing more, and it has it: `SpOverSlipLink`
        // guards its transport with its own `callMtx_`, so a SmartPort call
        // in flight on the CPU thread and a `stop()` here cannot overlap. That
        // is exactly why callMtx_ exists (see SpOverSlipLink::stop) — the
        // emulator's state mutex USED to serialise the two by accident, and
        // relying on that accident is what kept this work under the lock.
        ChildProcess*      helper = nullptr;
        FujiNetTransport*  link   = nullptr;

        {
            // Resolving the card is a bus read and nothing else — microseconds
            // — so it is all that stays under the lock.
            auto state = controller.lockState();
            auto* card = findFujiNet(state.memory().slotBus());
            if (card) {
                link = &card->transportLink();
                // Every helper request stops whatever is running first —
                // `ChildProcess::start()` does it internally anyway, with the
                // same 2 s grace — so pick the process up here and stop it
                // once the lock is gone.
                if ((r.requestHelperStart || r.requestHelperStop ||
                     r.requestHelperRestart) && card->helper().isRunning())
                    helper = &card->helper();
            }
        }

        // ── Off the lock from here ───────────────────────────────────────
        // Link first, helper second: the same order ~FujiNetCard uses, so the
        // peer goes away cleanly before the process serving it is killed.
        if (link) {
            if (r.transportChanged) {
                switch (r.transportTo) {
                case FujiNet_ImGui::Transport::Tcp:
                    link->setTcpMode(link->tcpPort());
                    break;
                case FujiNet_ImGui::Transport::Serial:
                    link->setSerialMode(link->serialPath(), link->serialBaud());
                    break;
                case FujiNet_ImGui::Transport::Off:
                    link->setOff();
                    break;
                }
            }
            if (r.portChanged)    link->setTcpMode(r.portTo);
            if (r.serialChanged)  link->setSerialMode(r.serialPathTo,
                                                      r.serialBaudTo);
            if (r.timeoutChanged) link->setTimeoutMs(r.timeoutTo);

            if (r.requestStart) {
                std::string err;
                if (!link->start(err)) { startError = err; startFailed = true; }
            }
            // stop() JOINS the link's worker, and that worker can be inside a
            // device enumeration — up to `kMaxUnits` round trips, each bounded
            // by the panel's own timeout (5 s at the maximum setting). Under
            // the machine lock that is the freeze CLAUDE.md forbids: the CPU
            // worker blocked on its next chunk, the UI thread blocked trying
            // to paint, and the panel's own Stop button unreachable because
            // drawing it needs the same mutex. Every mode change above is a
            // stop too (setTcpMode/setSerialMode/setOff all call it), which is
            // why the whole block moved and not just the stop button.
            if (r.requestStop)     link->stop();
            if (r.requestDropPeer) {
                // Stop then start: the button means "let the peer
                // reconnect", not "turn the bridge off", so the listener
                // comes straight back up.
                link->stop();
                std::string err;
                link->start(err);
            }
        }

        // The grace poll, and on POSIX the SIGKILL sweep and the reap that
        // follow it.
        if (helper) helper->stop();

        if (r.requestHelperStart || r.requestHelperRestart) {
            // Re-resolved rather than reusing the pointer above: the spawn is
            // sub-millisecond, so it belongs back under the lock, where the
            // card is proven live and nothing has to escape a critical
            // section at all.
            auto state = controller.lockState();
            auto* card = findFujiNet(state.memory().slotBus());
            if (card) {
                std::string err;
                if (!card->startHelper(helperPath_, err))
                    helperMessage = err;
                else if (r.requestHelperRestart)
                    helperMessage = "FujiNet program restarted.";
            }
        }

        if (startFailed)             status_ = startError;
        else if (r.requestStart)     status_.clear();
        if (!helperMessage.empty())  status_ = helperMessage;
        else if (r.requestHelperStart) status_.clear();
    }

    if (r.requestOpenWebUi) status_ = "FujiNet web UI: http://127.0.0.1/";
}

void NetworkCoordinator::rescanSerialDevices()
{
    serialDevices_.clear();
    for (const auto& d : SerialPort::enumerate())
        serialDevices_.emplace_back(d.path, d.description);
}

void NetworkCoordinator::setHelperPath(std::string path)
{
    helperPath_ = std::move(path);
    // An empty configured path means "find the stock `fujinet` on PATH".
    helperResolved_ = ChildProcess::findOnPath(
        helperPath_.empty() ? std::string("fujinet") : helperPath_);
}

} // namespace pom2
