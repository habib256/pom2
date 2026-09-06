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

// MouseCardAppleWin — see header. Direct port of AppleWin's
// `source/MouseInterface.cpp` (CMouseInterface). The constants, command
// dispatch, bit-handshake on Port B, and OnMouseEvent gating are
// transcribed line-for-line so future AppleWin updates can be diffed
// against POM2 without re-deriving the protocol.

#include "MouseCardAppleWin.h"
#include "Logger.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace {

// AppleWin MouseInterface.cpp — command opcodes (high nibble of first
// byte written to the MCU command port).
constexpr uint8_t MOUSE_SET   = 0x00;
constexpr uint8_t MOUSE_READ  = 0x10;
constexpr uint8_t MOUSE_SERV  = 0x20;
constexpr uint8_t MOUSE_CLEAR = 0x30;
constexpr uint8_t MOUSE_POS   = 0x40;
constexpr uint8_t MOUSE_INIT  = 0x50;
constexpr uint8_t MOUSE_CLAMP = 0x60;
constexpr uint8_t MOUSE_HOME  = 0x70;
constexpr uint8_t MOUSE_TIME  = 0x90;

constexpr uint8_t BIT4 = 0x10;
constexpr uint8_t BIT5 = 0x20;
constexpr uint8_t BIT6 = 0x40;
constexpr uint8_t BIT7 = 0x80;

// Status bits returned to the firmware in MOUSE_READ / MOUSE_SERV.
constexpr uint8_t STAT_PREV_BUTTON1             = 1 << 0;
constexpr uint8_t STAT_INT_MOVEMENT             = 1 << 1;
constexpr uint8_t STAT_INT_BUTTON               = 1 << 2;
constexpr uint8_t STAT_INT_VBL                  = 1 << 3;
constexpr uint8_t STAT_CURR_BUTTON1             = 1 << 4;
constexpr uint8_t STAT_MOVEMENT_SINCE_READMOUSE = 1 << 5;
constexpr uint8_t STAT_PREV_BUTTON0             = 1 << 6;
constexpr uint8_t STAT_CURR_BUTTON0             = 1 << 7;
constexpr uint8_t STAT_INT_ALL =
    STAT_INT_VBL | STAT_INT_BUTTON | STAT_INT_MOVEMENT;

// MODE bits (MOUSE_SET argument, latched into byMode).
constexpr uint8_t MODE_MOUSE_ON     = 1 << 0;
[[maybe_unused]] constexpr uint8_t MODE_INT_MOVEMENT = 1 << 1;
[[maybe_unused]] constexpr uint8_t MODE_INT_BUTTON   = 1 << 2;
constexpr uint8_t MODE_INT_VBL      = 1 << 3;
constexpr uint8_t MODE_INT_ALL      = STAT_INT_ALL;

// VBL period now lives in the member `vblCycles_` (default 17045 =
// 1.022727 MHz / 60 Hz NTSC; PAL profiles plumb 20313 ≈ 50 Hz through
// setVblCycles at plug time) — a hard-wired NTSC constant desynced
// MODE_INT_VBL from the 50 Hz frame on the PAL profiles.

}  // namespace

MouseCardAppleWin::MouseCardAppleWin(int slot)
    : slot_(slot)
{
    pia.setPortAWriteCallback([this](uint8_t v) { onPiaPortAOut(v); });
    pia.setPortBWriteCallback([this](uint8_t v) { onPiaPortBOut(v); });

    // AppleWin Reset(): m_by6821B = 0x40 → BIT6 (read-strobe ack) set.
    pia.setPortAInput(0);
    pia.setPortBInput(by6821B);
    pia.setCB1(true);
}

bool MouseCardAppleWin::loadRom(const std::string& slotRomPath)
{
    std::ifstream f(slotRomPath, std::ios::binary);
    if (!f) {
        pom2::log().warn("MouseAW", "Cannot open slot ROM: " + slotRomPath);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 0x800) {
        pom2::log().warn("MouseAW",
            "Slot ROM size mismatch (" + std::to_string(size) +
            " bytes, expected 2048): " + slotRomPath);
        return false;
    }
    f.read(reinterpret_cast<char*>(slotRom.data()), 0x800);
    if (!f) {
        pom2::log().warn("MouseAW", "Short read on slot ROM: " + slotRomPath);
        return false;
    }
    slotRomLoaded = true;
    pom2::log().info("MouseAW",
        "Loaded slot ROM " + slotRomPath + " (2048 bytes)");
    return true;
}

// setHostMouse is header-inline — see MouseCardAppleWin.h (the AI control
// server drives it without linking this TU).

MouseCardAppleWin::DebugSnapshot MouseCardAppleWin::debugSnapshot() const
{
    DebugSnapshot s{};
    s.iX        = iX;        s.iY        = iY;
    s.nX        = nX;        s.nY        = nY;
    s.iMinX     = iMinX;     s.iMaxX     = iMaxX;
    s.iMinY     = iMinY;     s.iMaxY     = iMaxY;
    s.bBtn0     = bButtons[0];
    s.bBtn1     = bButtons[1];
    s.bPrevBtn0 = bBtn0;
    s.bPrevBtn1 = bBtn1;
    s.byMode    = byMode;
    s.byState   = byState;
    s.by6821A   = by6821A;
    s.by6821B   = by6821B;
    s.buffPos   = nBuffPos;
    s.dataLen   = nDataLen;
    s.lastCmd   = byBuff[0];
    return s;
}

// ─── SlotPeripheral ───────────────────────────────────────────────────────

uint8_t MouseCardAppleWin::deviceSelectRead(uint8_t low4)
{
    return pia.read(static_cast<uint8_t>(low4 & 0x03));
}

void MouseCardAppleWin::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    pia.write(static_cast<uint8_t>(low4 & 0x03), v);
}

uint8_t MouseCardAppleWin::slotRomRead(uint8_t low8)
{
    // AppleWin SetSlotRom: bank = (m_by6821B << 7) & 0x0700. POM2 reads
    // the bank on demand instead of memcpy'ing into peripheral ROM.
    const uint16_t bank = static_cast<uint16_t>((by6821B << 7) & 0x0700);
    return slotRom[static_cast<size_t>(low8) | bank];
}

void MouseCardAppleWin::advanceCycles(int cycles)
{
    if (!isReady() || cycles <= 0) return;

    // Drain any host motion / button accumulated since the last tick. This
    // lets the firmware see input as soon as it polls, without waiting for
    // the next VBL boundary.
    //
    // advanceCycles runs once per INSTRUCTION on the CPU thread and this card
    // is the //e default in slot 4, so the drain has to be free when nothing
    // moved: gate it on a generation counter the UI thread bumps inside
    // setHostMouse. One relaxed load per instruction instead of three loads
    // plus the wrap-corrected delta arithmetic, with identical semantics —
    // a change is still visible to the very next instruction, and the
    // `!hostPrimed` term keeps the first tick priming the delta trackers
    // exactly where it did before (from the shadow as it stands, not from
    // the first move).
    const uint32_t gen = hostGen.load(std::memory_order_relaxed);
    if (gen != lastHostGen_ || !hostPrimed) {
        lastHostGen_ = gen;
        pollHostInput();
    }

    vblCycleAccum += cycles;
    while (vblCycleAccum >= vblCycles_) {
        vblCycleAccum -= vblCycles_;
        onMouseEvent(/*vbl=*/true);
    }
}

void MouseCardAppleWin::onReset()
{
    // AppleWin Reset() — verbatim defaults.
    by6821A = 0;
    by6821B = 0x40;
    pia.reset();
    pia.setCB1(true);
    pia.setPortBInput(by6821B);

    byMode = 0;
    nX = nY = 0;
    iX = iY = 0;
    iMinX = 0; iMaxX = 1023;
    iMinY = 0; iMaxY = 1023;
    bButtons[0] = bButtons[1] = false;

    clearState();
    for (auto& b : byBuff) b = 0;

    lastHostX = lastHostY = 0;
    lastHostButton = false;
    hostPrimed = false;
    vblCycleAccum = 0;

    assertIrq(false);
}

// ─── PIA → MouseCard bridge (verbatim from AppleWin On6821_A/B) ───────────

void MouseCardAppleWin::onPiaPortAOut(uint8_t v)
{
    // On6821_A: just stash. The byte is consumed by OnCommand / OnWrite
    // when the firmware drops BIT5 of Port B (write-strobe).
    by6821A = v;
}

void MouseCardAppleWin::onPiaPortBOut(uint8_t v)
{
    // AppleWin On6821_B (verbatim): handshake on BIT5 (write-strobe) and
    // BIT4 (read-strobe); only react when at least one bit in 0x3E
    // changed (the firmware-driven nibble).
    const uint8_t byDiff = (by6821B ^ v) & 0x3E;
    if (!byDiff) return;

    by6821B = static_cast<uint8_t>((by6821B & ~0x3E) | (v & 0x3E));

    if (byDiff & BIT5) {
        if (v & BIT5) {
            // 0→1 on BIT5 — just signal write-ack. Pair with the 1→0
            // edge below that actually consumes the byte.
            by6821B |= BIT7;
        } else {
            // 1→0 — consume Port A as the next command/data byte.
            byBuff[nBuffPos++] = by6821A;
            if (nBuffPos == 1) onCommand();
            if (nBuffPos == nDataLen || nBuffPos > 7) {
                onWrite();
                nBuffPos = 0;
            }
            by6821B &= ~BIT7;
            pia.setPortBInput(by6821B);
        }
    }
    if (byDiff & BIT4) {
        if (v & BIT4) {
            by6821B &= ~BIT6;
        } else {
            // Read-strobe: deliver the next buffered reply byte to Port A.
            if (nBuffPos) ++nBuffPos;
            if (nBuffPos == nDataLen || nBuffPos > 7) {
                nBuffPos = 0;
            } else {
                pia.setPortAInput(byBuff[nBuffPos]);
            }
            by6821B |= BIT6;
        }
    }
    pia.setPortBInput(by6821B);
}

// ─── HLE'd MCU command dispatch (AppleWin OnCommand / OnWrite) ────────────

void MouseCardAppleWin::onCommand()
{
    switch (byBuff[0] & 0xF0) {
    case MOUSE_SET:
        nDataLen = 1;
        byMode = static_cast<uint8_t>(byBuff[0] & 0x0F);
        break;
    case MOUSE_READ:
        nDataLen = 6;
        byState &= STAT_MOVEMENT_SINCE_READMOUSE;
        nX = iX;
        nY = iY;
        if (bBtn0) byState |= STAT_PREV_BUTTON0;
        if (bBtn1) byState |= STAT_PREV_BUTTON1;
        bBtn0 = bButtons[0];
        bBtn1 = bButtons[1];
        if (bBtn0) byState |= STAT_CURR_BUTTON0;
        if (bBtn1) byState |= STAT_CURR_BUTTON1;
        byBuff[1] = static_cast<uint8_t>(nX & 0xFF);
        byBuff[2] = static_cast<uint8_t>((nX >> 8) & 0xFF);
        byBuff[3] = static_cast<uint8_t>(nY & 0xFF);
        byBuff[4] = static_cast<uint8_t>((nY >> 8) & 0xFF);
        byBuff[5] = byState;
        byState &= ~STAT_MOVEMENT_SINCE_READMOUSE;
        break;
    case MOUSE_SERV:
        nDataLen = 2;
        byBuff[1] = static_cast<uint8_t>(byState & ~STAT_MOVEMENT_SINCE_READMOUSE);
        assertIrq(false);     // AppleWin: CpuIrqDeassert(IS_MOUSE)
        break;
    case MOUSE_CLEAR:
        clearState();
        nDataLen = 1;
        break;
    case MOUSE_POS:
        nDataLen = 5;
        break;
    case MOUSE_INIT:
        nDataLen = 3;
        byBuff[1] = 0xFF;
        break;
    case MOUSE_CLAMP:
        nDataLen = 5;
        break;
    case MOUSE_HOME:
        nDataLen = 1;
        // The ONE deliberate deviation from AppleWin in this file. AppleWin
        // has `case MOUSE_HOME: m_nDataLen = 1; SetPositionAbs( 0, 0 );`,
        // but Apple's own spec for the firmware entry this command backs
        // (HOMEMOUSE, $Cn08 — Apple II Mouse Technical Note / the AppleMouse
        // II User's Manual) is "sets the mouse position to the upper-left
        // corner of the clamping window", i.e. (MinX, MinY). The two agree
        // only while the clamp window is still the power-on 0..1023: a
        // program that clamps to, say, X 100..500 and then homes expects the
        // cursor at 100, and got 0 — outside its own window, where the next
        // relative move would snap it back with a visible jump.
        setPositionAbs(iMinX, iMinY);
        clampX();
        clampY();
        break;
    case MOUSE_TIME:
        switch (byBuff[0] & 0x0C) {
        case 0x00: nDataLen = 1; break;
        case 0x04: nDataLen = 3; break;
        case 0x08: nDataLen = 2; break;
        case 0x0C: nDataLen = 4; break;
        }
        break;
    case 0xA0:
        nDataLen = 2;
        break;
    case 0xB0:
    case 0xC0:
        nDataLen = 1;
        break;
    default:
        nDataLen = 1;
        break;
    }
    pia.setPortAInput(byBuff[1]);
}

void MouseCardAppleWin::onWrite()
{
    int nMin, nMax;
    switch (byBuff[0] & 0xF0) {
    case MOUSE_CLAMP:
        nMin = (byBuff[3] << 8) | byBuff[1];
        nMax = (byBuff[4] << 8) | byBuff[2];
        if (byBuff[0] & 1) setClampY(nMin, nMax);
        else               setClampX(nMin, nMax);
        break;
    case MOUSE_POS:
        nX = (byBuff[2] << 8) | byBuff[1];
        nY = (byBuff[4] << 8) | byBuff[3];
        setPositionAbs(nX, nY);
        break;
    case MOUSE_INIT:
        nX = 0;
        nY = 0;
        setClampX(0, 1023);
        setClampY(0, 1023);
        setPositionAbs(0, 0);
        break;
    }
}

void MouseCardAppleWin::onMouseEvent(bool vbl)
{
    uint8_t st = 0;

    if ((byMode & MODE_INT_VBL) && vbl)
        st |= STAT_INT_VBL;

    if (byMode & MODE_MOUSE_ON) {
        if (nX != iX || nY != iY) {
            st     |= STAT_INT_MOVEMENT | STAT_MOVEMENT_SINCE_READMOUSE;
            byState |= STAT_MOVEMENT_SINCE_READMOUSE;
        }
        if (bBtn0 != bButtons[0] || bBtn1 != bButtons[1])
            st |= STAT_INT_BUTTON;
        st &= static_cast<uint8_t>((byMode & MODE_INT_ALL) |
                                   STAT_MOVEMENT_SINCE_READMOUSE);
    } else {
        st &= STAT_INT_VBL;
    }

    if (st & STAT_INT_ALL) {
        byState |= st;
        assertIrq(true);     // AppleWin: CpuIrqAssert(IS_MOUSE)
    }
}

void MouseCardAppleWin::clearState()
{
    nBuffPos = 0;
    nDataLen = 1;
    byState  = 0;
    nX = nY = 0;
    bBtn0 = bBtn1 = false;
    setPositionAbs(0, 0);
}

int MouseCardAppleWin::clampX()
{
    if (iX > iMaxX) { iX = iMaxX; return  1; }
    if (iX < iMinX) { iX = iMinX; return -1; }
    return 0;
}

int MouseCardAppleWin::clampY()
{
    if (iY > iMaxY) { iY = iMaxY; return  1; }
    if (iY < iMinY) { iY = iMinY; return -1; }
    return 0;
}

void MouseCardAppleWin::setClampX(int lo, int hi)
{
    // AppleWin SetClampX: swapped-range trick — when lo > hi, treat as a
    // wrapped window with effective max = (lo+hi)&0xFFFF.
    if (static_cast<unsigned>(lo) > 0xFFFF ||
        static_cast<unsigned>(hi) > 0xFFFF) return;
    if (lo > hi) { int nh = (lo + hi) & 0xFFFF; lo = 0; hi = nh; }
    iMinX = lo;
    iMaxX = hi;
    clampX();
}

void MouseCardAppleWin::setClampY(int lo, int hi)
{
    if (static_cast<unsigned>(lo) > 0xFFFF ||
        static_cast<unsigned>(hi) > 0xFFFF) return;
    if (lo > hi) { int nh = (lo + hi) & 0xFFFF; lo = 0; hi = nh; }
    iMinY = lo;
    iMaxY = hi;
    clampY();
}

void MouseCardAppleWin::setPositionAbs(int x, int y)
{
    iX = x;
    iY = y;
}

void MouseCardAppleWin::setPositionRel(int dx, int dy)
{
    iX += dx; clampX();
    iY += dy; clampY();
    onMouseEvent(/*vbl=*/false);
}

void MouseCardAppleWin::setButton(int idx, bool down)
{
    if (idx < 0 || idx > 1) return;
    bButtons[idx] = down;
    onMouseEvent(/*vbl=*/false);
}

void MouseCardAppleWin::pollHostInput()
{
    const uint8_t hx = hostX.load(std::memory_order_relaxed);
    const uint8_t hy = hostY.load(std::memory_order_relaxed);
    const bool    hb = hostButton.load(std::memory_order_relaxed);

    if (!hostPrimed) {
        lastHostX = hx;
        lastHostY = hy;
        lastHostButton = hb;
        hostPrimed = true;
        return;
    }

    // 8-bit signed delta with wrap correction — matches the running
    // counter convention used by MAME-faithful MouseCard's setHostMouse.
    int dx = static_cast<int>(hx) - static_cast<int>(lastHostX);
    int dy = static_cast<int>(hy) - static_cast<int>(lastHostY);
    if (dx >  0x80) dx -= 0x100;
    else if (dx < -0x80) dx += 0x100;
    if (dy >  0x80) dy -= 0x100;
    else if (dy < -0x80) dy += 0x100;
    lastHostX = hx;
    lastHostY = hy;

    if (dx || dy) setPositionRel(dx, dy);

    if (hb != lastHostButton) {
        lastHostButton = hb;
        setButton(0, hb);
    }
}

// ── Snapshot / rewind ─────────────────────────────────────────────────────
//
// The whole HLE'd MCU is plain C++ members, so this blob IS the MCU state:
// mode/status bytes, the resolved position + clamp window, button shadows,
// the command-byte cursor, the Port A/B latch shadows and the VBL pacer,
// wrapping the real MC6821's own blob. The host-input shadows
// (hostX/hostY/hostButton and their lastHost* delta trackers) are NOT
// serialized — same rule as MouseCard: that is where the user's physical
// pointer is right now, not emulated state. `hostPrimed` is forced false on
// restore so the next pollHostInput re-seeds the delta trackers from the
// live pointer with a zero delta, instead of replaying the distance the
// pointer travelled while the snapshot sat on disk as one cursor jump.

namespace {
constexpr uint8_t kMouseAwSnapMagic[4] = { 'M', 'A', 'W', '1' };
constexpr size_t  kMouseAwFixedBytes   = 4 + 2 + 8 + 8 + 2 + 16 + 16 + 4 + 8 + 1;
}

void MouseCardAppleWin::appendSnapshotState(std::vector<uint8_t>& out) const
{
    auto put32 = [&](int32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i)));
    };
    out.insert(out.end(), kMouseAwSnapMagic, kMouseAwSnapMagic + 4);
    out.push_back(by6821A);
    out.push_back(by6821B);
    out.insert(out.end(), std::begin(byBuff), std::end(byBuff));
    put32(nBuffPos);
    put32(nDataLen);
    out.push_back(byState);
    out.push_back(byMode);
    put32(iX);    put32(iY);
    put32(nX);    put32(nY);
    put32(iMinX); put32(iMaxX);
    put32(iMinY); put32(iMaxY);
    out.push_back(bButtons[0] ? 1 : 0);
    out.push_back(bButtons[1] ? 1 : 0);
    out.push_back(bBtn0 ? 1 : 0);
    out.push_back(bBtn1 ? 1 : 0);
    put32(vblCycles_);
    put32(vblCycleAccum);
    // The IRQ level cannot be re-derived from byState: MOUSE_SERV drops the
    // line (AppleWin's CpuIrqDeassert(IS_MOUSE)) WITHOUT clearing byState's
    // STAT_INT_* bits — only the next MOUSE_READ does that. Carry the level
    // the card actually published to the wire-OR.
    out.push_back(slotIrqAsserted() ? 1 : 0);
    pia.appendSnapshotState(out);
}

void MouseCardAppleWin::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (data == nullptr ||
        len < kMouseAwFixedBytes + MC6821::kSnapshotBytes ||
        std::memcmp(data, kMouseAwSnapMagic, 4) != 0)
        return;   // foreign blob — a different card sat in this slot
    size_t p = 4;
    auto get32 = [&]() -> int32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data[p++]) << (8 * i);
        return static_cast<int32_t>(v);
    };
    by6821A = data[p++];
    by6821B = data[p++];
    std::memcpy(byBuff, data + p, sizeof(byBuff)); p += sizeof(byBuff);
    nBuffPos = get32();
    nDataLen = get32();
    byState  = data[p++];
    byMode   = data[p++];
    iX    = get32(); iY    = get32();
    nX    = get32(); nY    = get32();
    iMinX = get32(); iMaxX = get32();
    iMinY = get32(); iMaxY = get32();
    bButtons[0] = data[p++] != 0;
    bButtons[1] = data[p++] != 0;
    bBtn0       = data[p++] != 0;
    bBtn1       = data[p++] != 0;
    vblCycles_    = get32();
    vblCycleAccum = get32();
    const bool irq = data[p++] != 0;
    p += pia.loadSnapshotState(data + p, len - p);

    // Untrusted blob: onPiaPortBOut indexes byBuff with nBuffPos and
    // advanceCycles loops `while (vblCycleAccum >= vblCycles_)`, so clamp
    // every cursor and pacer to the range this card can actually produce.
    if (nBuffPos < 0 || nBuffPos > 7) nBuffPos = 0;
    if (nDataLen < 1 || nDataLen > 8) nDataLen = 1;
    if (vblCycles_ <= 0) vblCycles_ = 17045;
    if (vblCycleAccum < 0 || vblCycleAccum >= vblCycles_) vblCycleAccum = 0;
    // AppleWin's SetClampX/Y only ever store 0..0xFFFF; anything else came
    // from a corrupt blob and would let clampX/clampY strand iX outside the
    // firmware's window.
    auto clampBound = [](int v) { return (v < 0 || v > 0xFFFF) ? 0 : v; };
    iMinX = clampBound(iMinX); iMaxX = clampBound(iMaxX);
    iMinY = clampBound(iMinY); iMaxY = clampBound(iMaxY);
    if (iMinX > iMaxX) { iMinX = 0; iMaxX = 1023; }
    if (iMinY > iMaxY) { iMinY = 0; iMaxY = 1023; }
    clampX();
    clampY();

    // Re-seed the host delta trackers from the live pointer on the next poll.
    hostPrimed = false;
    // Re-publish the slot IRQ: the wire-OR bookkeeping lives in
    // SlotPeripheral, not in any serialized member.
    assertIrq(irq);
}
