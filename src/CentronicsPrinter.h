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

// CentronicsPrinter — the printer at the far end of a parallel card's cable,
// as the card's status inputs see it, plus the paper it prints on (a spool
// the host ImageWriter drains).
//
// Lines, as raw connector levels (true = high):
//   SELECT   high while the printer is on line
//   PE       high while it is out of paper
//   BUSY     high while it cannot take a byte (buffer full, off line, out of
//            paper, or no printer at all)
//   /FAULT   LOW on an error (off line, out of paper)
// With no printer on the cable every input floats high (MAME ctronics.cpp:
// 56-73 pulls them up), /ACK included — so it never pulses.
//
// `strobe(byte)` is the card's /STROBE: a printer that can take the byte
// prints it and acknowledges at once (the caller then sets its ACK latch).
// One that cannot keeps it, unacknowledged, and prints it — acknowledging
// then — when it is able again; `onAck` tells the card. A real printer would
// ignore a strobe it cannot take and leave the firmware waiting for an ACK
// that never comes; POM2 takes the recoverable reading, the same choice as
// GrapplerCard's.
//
// Threading: the line setters run under the machine lock (PrinterPortControl,
// the panels); `strobe` runs on the CPU thread, which holds the same lock.
// The spool has its own mutex because the host ImageWriter drains it from the
// UI thread.

#ifndef POM2_CENTRONICS_PRINTER_H
#define POM2_CENTRONICS_PRINTER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace pom2 {

class CentronicsPrinter
{
public:
    static constexpr std::size_t kMaxSpoolBytes = 4u * 1024u * 1024u;

    void setOnline(bool on)            { online_ = on;    release(); }
    void setPaperOut(bool out)         { paperOut_ = out; release(); }
    void setConnected(bool connected)  { connected_ = connected; release(); }
    void setBusy(bool busy)            { busy_ = busy;    release(); }
    bool online() const     { return online_; }
    bool paperOut() const   { return paperOut_; }
    bool connected() const  { return connected_; }
    bool busy() const       { return busy_; }

    bool selectLine() const { return !connected_ || online_; }
    bool peLine() const     { return !connected_ || paperOut_; }
    bool busyLine() const   { return !connected_ || busy_ || paperOut_ || !online_; }
    /// /FAULT, active low: low while connected and off line or out of paper.
    bool faultLine() const  { return !connected_ || (online_ && !paperOut_); }

    /// Called when a held byte finally prints (the /ACK pulse it earned).
    void setAckHandler(std::function<void()> fn) { onAck_ = std::move(fn); }

    /// /STROBE with `byte` on the data lines. True = printed and acknowledged
    /// now; false = held for later (or lost, with no printer: a byte strobed
    /// into an empty cable is held too, and prints if one is plugged in).
    bool strobe(uint8_t byte);
    /// A reset abandons a held byte.
    void dropPending() { pending_ = false; }
    /// The held byte, for the owning card's snapshot. It travels with the
    /// ACK latch that waits on it: a rewind that restored the latch without
    /// it left the guest waiting on an ACK no byte would give, or printed a
    /// byte from the abandoned future (bug hunt 2026-09-29).
    bool    hasPending() const  { return pending_; }
    uint8_t pendingByte() const { return pendingByte_; }
    void restorePending(bool held, uint8_t byte)
    {
        pending_ = held;
        pendingByte_ = byte;
        release();           // a printer that accepts now takes it at once
    }

    /// Same contract as PrinterCard::drainSpoolFrom: absolute offsets, and a
    /// `from` past the end hands back everything (the consumer resyncs).
    std::size_t drainSpoolFrom(std::size_t from, std::vector<uint8_t>& out) const;
    std::size_t bytesWritten() const;

private:
    bool accepts() const { return connected_ && online_ && !paperOut_ && !busy_; }
    void print(uint8_t byte);
    void release();

    bool online_ = true;
    bool paperOut_ = false;
    bool connected_ = true;
    bool busy_ = false;
    bool pending_ = false;
    uint8_t pendingByte_ = 0;
    std::function<void()> onAck_;

    mutable std::mutex spoolMtx_;
    std::deque<uint8_t> spool_;
    std::size_t spoolBase_ = 0;
};

}  // namespace pom2

#endif  // POM2_CENTRONICS_PRINTER_H
