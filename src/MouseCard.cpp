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

// MouseCard — see header. Verbatim port of MAME `bus/a2bus/mouse.cpp`,
// re-expressed for POM2's slot bus + the M68705P3 / MC6821 emulators
// just landed in Phases 2 + 3. Read this side-by-side with MAME's
// `a2bus_mouse_device::pia_out_a/b`, `mcu_port_a/b/c_*`, and
// `update_axis<>`.

#include "MouseCard.h"
#include "Logger.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

// MCU Port-B pin assignments. The bit->function mapping is verbatim from
// MAME src/devices/bus/a2bus/mouse.cpp `mcu_port_b_r()`: PB0/PB2 carry the
// X/Y *direction*, PB1/PB3 the X/Y *gate* (toggles once per pixel moved),
// PB7 the button. NOTE the X labels: POM2 names them lower-bit-first
// (X0=PB0=direction, X1=PB1=gate); MAME uses the opposite digits there
// (its X1=0x01=direction, X0=0x02=gate). Same bits, same behaviour — only
// the label differs. The Y labels (Y0=direction, Y1=gate) match MAME
// directly. Pinned by tests/mouse_card_axis_parity_test.cpp (real firmware,
// X==Y for an identical ramp).
constexpr uint8_t MCU_PB_X0     = 0x01;     // PB0  X direction (0=left, 1=right)
constexpr uint8_t MCU_PB_X1     = 0x02;     // PB1  X gate (toggles per pixel)
constexpr uint8_t MCU_PB_Y0     = 0x04;     // PB2  Y direction (0=up,   1=down)
constexpr uint8_t MCU_PB_Y1     = 0x08;     // PB3  Y gate (toggles per pixel)
constexpr uint8_t MCU_PB_BUTTON = 0x80;     // PB7  button (active LOW, 0=pressed)

// Diagnostic trace — enabled by `POM2_MOUSE_TRACE=1`. Logs every PIA
// register access (from the 6502) and every slot-IRQ transition with
// the MCU's PC. Capped at kTraceMax events to avoid log flooding.
bool traceEnabled()
{
    static const bool e = []() {
        const char* env = std::getenv("POM2_MOUSE_TRACE");
        return env && env[0] != '\0' && env[0] != '0';
    }();
    return e;
}
constexpr int kTraceMax = 20000;
std::atomic<int> g_traceCount { 0 };

void mtrace(const char* fmt, ...)
{
    if (!traceEnabled()) return;
    if (g_traceCount.fetch_add(1, std::memory_order_relaxed) >= kTraceMax) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pom2::log().info("MouseTrace", buf);
}

// MCU machine cycles per Apple II cycle. The 68705 divides its 2043600 Hz
// crystal by 4 (M68705P3.h; MAME's m6805 core, execute_clock_to_cycles), so
// it retires 510 900 machine cycles a second — half the 1.02 MHz bus. POM2
// ran it at 2:1, four times too fast, and the VBL-mode interrupt came at
// 240 Hz instead of 60 (bug hunt 2026-09-17).
constexpr int MCU_CLOCK_NUMERATOR   = 1;
constexpr int MCU_CLOCK_DENOMINATOR = 2;

}  // namespace

MouseCard::MouseCard(int slot)
    : slot_(slot)
{
    // Wire callbacks: PIA → MouseCard → MCU input pins.
    pia.setPortAWriteCallback([this](uint8_t v) { onPiaPortAOut(v); });
    pia.setPortBWriteCallback([this](uint8_t v) { onPiaPortBOut(v); });

    // MCU output writes propagate the other way: MCU → MouseCard → PIA
    // input pins (or, for PB6, the slot IRQ line).
    mcu.setPortWriteCallback([this](int port) { onMcuPortWrite(port); });

    // Port-read hook: synthesize Port B (button + quadrature) on every
    // MCU-side read.
    mcu.setPortReadCallback([this](int port) { return onMcuPortRead(port); });

    // Initial pin states. MAME mouse.cpp lines 43-44:
    //   "PIA port A connects to 68705 port A in its entirety (bi-directional
    //    with internal pullups)"
    //   "PIA PB4-PB7 connects to 68705 PC0-3 (bi-directional but SHOULD NOT
    //    BE PULLED UP)"
    // Therefore Port A defaults high (pull-ups), Port C defaults LOW. This
    // matches MAME's `tspb_handler().set_constant(0x00)` which makes PIA's
    // Port B input bits 4-7 read as 0 when nothing is driving. Critical for
    // the firmware's BRCLR1 $02,+$0D test at chip $03F3 — with PC1 high the
    // MCU dispatches on uninitialized Port A garbage instead of waiting for
    // the host slot ROM to send a command.
    mcu.setPortInput(0, 0xFF);     // PIA → MCU Port A: pulled up
    mcu.setPortInput(2, 0x00);     // PIA PB[7..4] → MCU PC[3..0]: NOT pulled up

    // CB1 tied high to a 10 kΩ pull-up (MAME mouse.cpp line 250).
    pia.setCB1(true);
}

bool MouseCard::loadRoms(const std::string& slotRomPath,
                         const std::string& mcuRomPath)
{
    // ── Slot EPROM (341-0270-c.4b) ────────────────────────────────────
    {
        std::ifstream f(slotRomPath, std::ios::binary);
        if (!f) {
            pom2::log().warn("Mouse",
                "Cannot open slot ROM: " + slotRomPath);
            return false;
        }
        f.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        if (size != 0x800) {
            pom2::log().warn("Mouse",
                "Slot ROM size mismatch (" + std::to_string(size) +
                " bytes, expected 2048): " + slotRomPath);
            return false;
        }
        f.read(reinterpret_cast<char*>(slotRom.data()), 0x800);
        if (!f) {
            pom2::log().warn("Mouse", "Short read on slot ROM: " + slotRomPath);
            return false;
        }
        slotRomLoaded = true;
        pom2::log().info("Mouse",
            "Loaded slot ROM " + slotRomPath + " (2048 bytes)");
    }

    // ── MCU EPROM (341-0269.2b) ───────────────────────────────────────
    if (!mcu.loadRomFile(mcuRomPath)) {
        slotRomLoaded = false;     // refuse to plug if MCU ROM missing
        return false;
    }

    return true;
}

void MouseCard::setHostMouse(uint8_t rawX, uint8_t rawY, bool button)
{
    hostX.store(rawX, std::memory_order_relaxed);
    hostY.store(rawY, std::memory_order_relaxed);
    hostButton.store(button, std::memory_order_relaxed);
}

// ─── SlotPeripheral interface ───────────────────────────────────────────

uint8_t MouseCard::deviceSelectRead(uint8_t low4)
{
    // MAME `read_c0nx`: forward straight to the PIA, masking to the 2
    // register-select bits. The other 14 bits of the device-select
    // window mirror the PIA registers.
    const uint8_t reg = static_cast<uint8_t>(low4 & 0x03);
    const uint8_t v = pia.read(reg);
    if (traceEnabled()) {
        static const char* kReg[4] = { "PA/DDRA", "CRA", "PB/DDRB", "CRB" };
        mtrace("R  %s = $%02X  [DDRA=%02X DDRB=%02X CRA=%02X CRB=%02X bank=%X]",
               kReg[reg], v,
               pia.getDdrA(), pia.getDdrB(), pia.getCRA(), pia.getCRB(),
               static_cast<unsigned>(romBank >> 8));
    }
    return v;
}

void MouseCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    const uint8_t reg = static_cast<uint8_t>(low4 & 0x03);
    if (traceEnabled()) {
        static const char* kReg[4] = { "PA/DDRA", "CRA", "PB/DDRB", "CRB" };
        mtrace("W  %s = $%02X  [DDRA=%02X DDRB=%02X CRA=%02X CRB=%02X]",
               kReg[reg], v,
               pia.getDdrA(), pia.getDdrB(), pia.getCRA(), pia.getCRB());
    }
    pia.write(reg, v);
}

uint8_t MouseCard::slotRomRead(uint8_t low8)
{
    // MAME `read_cnxx`: `m_rom[offset | m_rom_bank]`. The bank is
    // determined by the most recent PIA Port B write (PB1..PB3).
    return slotRom[static_cast<size_t>(low8) | romBank];
}

void MouseCard::advanceCycles(int cycles)
{
    if (!isReady() || cycles <= 0) return;

    // MCU runs at 2× the Apple II clock — accumulate fractional cycles to
    // keep the ratio exact across calls. `mcu.run()` finishes whatever
    // instruction is in flight when the budget runs out (2..11 machine
    // cycles on the 68705; the firmware's idle loop is all 10-cycle
    // BRCLRs), so it overshoots. Bill the ACTUAL cycles retired against
    // the accumulator — which then goes negative and suppresses the next
    // run until the debt is repaid. This is MAME's own contract: its
    // `m6805_base_device::execute_run` is a do/while that runs
    // `while (m_icount > 0)` and leaves `m_icount` negative, and the
    // scheduler advances the device's local time by
    // `cycles_running - m_icount`, i.e. by what actually ran.
    // advanceCycles is called per 6502 instruction, so budgets are 4-14
    // MCU cycles here; dropping the overshoot instead ran the 68705 at
    // 1.26-1.50× its 2043600 Hz (MAME mouse.cpp:148
    // `M68705P3(config, m_mcu, 2043600);`).
    mcuCycleAccum += cycles * MCU_CLOCK_NUMERATOR;
    const int mcuCycles = mcuCycleAccum / MCU_CLOCK_DENOMINATOR;
    if (mcuCycles > 0) {
        const int ran = mcu.run(mcuCycles);
        mcuCycleAccum -= ran * MCU_CLOCK_DENOMINATOR;
        mcuCyclesRun_ += static_cast<uint64_t>(ran);
        if (traceEnabled()) {
            // Log only on PC change. Suppress the firmware's main idle
            // loop ($03F3 / $0403 / $0405 / $0409 / $0469) which spins
            // waiting for a command strobe — otherwise it floods the
            // trace and we lose the *interesting* transitions.
            static uint16_t lastPc = 0xFFFF;
            const uint16_t pcAfter = mcu.getPC();
            auto inIdle = [](uint16_t pc) {
                return pc == 0x03F3 || pc == 0x0403 || pc == 0x0405 ||
                       pc == 0x0409 || pc == 0x0469;
            };
            if (pcAfter != lastPc && !(inIdle(pcAfter) && inIdle(lastPc))) {
                mtrace("MCU PC -> $%04X  (was $%04X, mcuCyc=%d)",
                       pcAfter, lastPc, mcuCycles);
                lastPc = pcAfter;
            } else if (pcAfter != lastPc) {
                lastPc = pcAfter;     // silent update so we re-log on exit
            }
        }
    }
}

void MouseCard::onReset()
{
    if (traceEnabled()) {
        mtrace("=== MouseCard::onReset called ===");
    }
    mcu.reset();
    pia.reset();
    pia.setCB1(true);
    romBank = 0;
    portAtoMcu = 0xFF;     // PIA Port A pulled up (MAME pull-ups present)
    portCtoMcu = 0x00;     // PIA PB4-7 / MCU PC0-3 NOT pulled up (MAME tspb=0)
    // Seed the per-axis delta trackers from the CURRENT host counters,
    // not zero: MAME initialises m_last/m_count once in device_start, not
    // per reset. Zeroing here made the first post-reset mcuPortBRead see
    // diff = host - 0 (wrap-clamped to ±128) and fabricate up to 128
    // quadrature steps per axis — a visible cursor jump after every
    // Ctrl-Reset / profile switch.
    lastAxis[0] = hostX.load(std::memory_order_relaxed);
    lastAxis[1] = hostY.load(std::memory_order_relaxed);
    countAxis[0] = countAxis[1] = 0;
    portBState = 0xFF;
    assertIrq(false);
    mcuCycleAccum = 0;
    mcuCyclesRun_ = 0;
}

void MouseCard::onUnplug()
{
    // SlotBus::detachFromBus() auto-releases any pending IRQ before
    // unplug, so nothing to do here. Kept as a no-op override for
    // clarity / symmetry with onReset.
}

// ─── PIA → MCU bridge ───────────────────────────────────────────────────

void MouseCard::onPiaPortAOut(uint8_t v)
{
    // PIA Port A output → MCU Port A input. MAME `pia_out_a` ->
    // `set_port_a_in` (deferred via scheduler in MAME, immediate here).
    portAtoMcu = v;
    mcu.setPortInput(0, v);
}

void MouseCard::onPiaPortBOut(uint8_t v)
{
    // PIA Port B output:
    //   PB1..PB3  → EPROM A8..A10 (8-bank slot ROM bank-select)
    //   PB4..PB7  → MCU PC0..PC3 (high nibble shifted down)
    //   PB0       → "sync latch" (not modelled)
    romBank = static_cast<uint16_t>(v & 0x0E) << 7;

    const uint8_t newPc = static_cast<uint8_t>((v >> 4) & 0x0F);
    if (traceEnabled() && newPc != portCtoMcu) {
        mtrace("onPiaPortBOut: v=$%02X -> portCtoMcu $%02X -> $%02X  (PIA out_b=$%02X ddr_b=$%02X)",
               v, portCtoMcu, newPc, pia.getOutB(), pia.getDdrB());
    }
    portCtoMcu = newPc;
    mcu.setPortInput(2, portCtoMcu);
}

// ─── MCU → PIA bridge ───────────────────────────────────────────────────

void MouseCard::onMcuPortWrite(int port)
{
    if (port == 0) {
        // MCU Port A output → PIA Port A input. MAME `mcu_port_a_w` ->
        // `m_pia->set_a_input(...)`.
        const uint8_t pins = mcu.getPortPins(0);
        pia.setPortAInput(pins);
        return;
    }
    if (port == 1) {
        // MCU Port B output: bit 6 = slot IRQ (active low).
        const uint8_t pins = mcu.getPortPins(1);
        const bool assert = !(pins & 0x40);
        if (traceEnabled() && assert != slotIrqAsserted()) {
            mtrace("IRQ %s  pins=$%02X  MCU PC=$%04X  PIA-PA=$%02X PIA-PB=$%02X",
                   assert ? "ASSERT" : "CLEAR ",
                   pins, mcu.getPC(),
                   pia.getPortAOutput(), pia.getPortBOutput());
        }
        assertIrq(assert);
        return;
    }
    if (port == 2) {
        // MCU Port C output → PIA Port B input bits 4..7. MAME
        // `mcu_port_c_w` -> `m_pia->portb_w(data << 4)`.
        const uint8_t pins = mcu.getPortPins(2);
        pia.setPortBInput(static_cast<uint8_t>(pins << 4));
        return;
    }
}

// ─── MCU port-read synthesis (Port B = button + quadrature) ─────────────

uint8_t MouseCard::onMcuPortRead(int port)
{
    if (port == 1) return mcuPortBRead();
    if (port == 0) return portAtoMcu;
    if (port == 2) {
        if (traceEnabled()) {
            static uint8_t lastSeen = 0xAA;
            if (lastSeen != portCtoMcu) {
                lastSeen = portCtoMcu;
                mtrace("MCU PortC <- portCtoMcu=$%02X  (PIA out_b=$%02X ddr_b=$%02X composite=$%02X)",
                       portCtoMcu, pia.getOutB(), pia.getDdrB(),
                       pia.getPortBOutput());
            }
        }
        return portCtoMcu;
    }
    return 0xFF;
}

uint8_t MouseCard::mcuPortBRead()
{
    // Mirrors MAME `mcu_port_b_r()` (mouse.cpp lines 316-340). On every
    // call: refresh button bit; emit at most one quadrature edge per
    // axis if there's pending motion.
    const bool buttonPressed = hostButton.load(std::memory_order_relaxed);
    if (buttonPressed) portBState &= ~MCU_PB_BUTTON;
    else               portBState |=  MCU_PB_BUTTON;

    updateAxis(0, MCU_PB_X0, MCU_PB_X1);     // X axis: dir=X0, clk=X1
    updateAxis(1, MCU_PB_Y0, MCU_PB_Y1);     // Y axis: dir=Y0, clk=Y1

    return portBState;
}

void MouseCard::updateAxis(int axis, uint8_t dirBit, uint8_t clkBit)
{
    // Read the current host position for this axis as an unsigned 8-bit
    // running counter. Compute delta from last seen value with 8-bit
    // wrap correction (matches MAME's diff-wrap handling).
    const uint8_t cur = (axis == 0)
        ? hostX.load(std::memory_order_relaxed)
        : hostY.load(std::memory_order_relaxed);
    int diff = static_cast<int>(cur) - static_cast<int>(lastAxis[axis]);
    if (diff > 0x80)       diff -= 0x100;
    else if (diff < -0x80) diff += 0x100;

    countAxis[axis] += diff;
    lastAxis[axis]   = cur;

    if (countAxis[axis] != 0) {
        portBState ^= clkBit;     // toggle CLK
        if (countAxis[axis] < 0) {
            ++countAxis[axis];
            if (portBState & clkBit) portBState &= ~dirBit;
        } else {
            --countAxis[axis];
            if (portBState & clkBit) portBState |=  dirBit;
        }
    }
}


// ── Snapshot / rewind ─────────────────────────────────────────────────────
//
// The MCU + PIA carry their own state surfaces (M68705P3 /
// MC6821::append|loadSnapshotState); this wraps them with the card's
// bridge state. Host mouse position (hostX/hostY/hostButton) is NOT
// serialized: it is where the user's physical pointer is right now, not
// emulated state, and forcing it backwards on a rewind would fight the UI.

namespace {
constexpr uint8_t kMouseSnapMagic[4] = { 'M', 'S', 'E', '1' };
}

void MouseCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kMouseSnapMagic, kMouseSnapMagic + 4);
    out.push_back(static_cast<uint8_t>(romBank));
    out.push_back(static_cast<uint8_t>(romBank >> 8));
    out.push_back(portAtoMcu);
    out.push_back(portCtoMcu);
    out.push_back(portBState);
    for (int i = 0; i < 2; ++i) {
        out.push_back(static_cast<uint8_t>(lastAxis[i]));
        out.push_back(static_cast<uint8_t>(lastAxis[i] >> 8));
        out.push_back(static_cast<uint8_t>(countAxis[i]));
        out.push_back(static_cast<uint8_t>(countAxis[i] >> 8));
    }
    const uint32_t acc = static_cast<uint32_t>(mcuCycleAccum);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(acc >> (8 * i)));
    mcu.appendSnapshotState(out);
    pia.appendSnapshotState(out);
}

void MouseCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    constexpr size_t kFixed = 4 + 2 + 3 + 8 + 4;
    if (data == nullptr ||
        len < kFixed + M68705P3::kSnapshotBytes + MC6821::kSnapshotBytes ||
        std::memcmp(data, kMouseSnapMagic, 4) != 0)
        return;   // foreign blob — a different card sat in this slot
    size_t p = 4;
    romBank    = static_cast<uint16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    romBank    = static_cast<uint16_t>(romBank & 0x0700);   // 8 banks of 256 B
    portAtoMcu = data[p++];
    portCtoMcu = data[p++];
    portBState = data[p++];
    for (int i = 0; i < 2; ++i) {
        lastAxis[i]  = static_cast<int16_t>(data[p] | (data[p + 1] << 8)); p += 2;
        countAxis[i] = static_cast<int16_t>(data[p] | (data[p + 1] << 8)); p += 2;
    }
    uint32_t acc = 0;
    for (int i = 0; i < 4; ++i) acc |= static_cast<uint32_t>(data[p++]) << (8 * i);
    mcuCycleAccum = static_cast<int>(acc);
    // Untrusted blob: this feeds `mcu.run()` directly, so a crafted value
    // near INT_MAX would hand the MCU a two-billion-cycle slice. A live
    // accumulator only ever holds one bus slice's worth of credit minus at
    // most one 68705 instruction of debt.
    if (mcuCycleAccum < -64 || mcuCycleAccum > 1 << 16) mcuCycleAccum = 0;
    p += mcu.loadSnapshotState(data + p, len - p);
    p += pia.loadSnapshotState(data + p, len - p);
    // Re-drive the slot IRQ from MCU Port B bit 6, the card's ONLY IRQ
    // source (MAME mouse.cpp:46 "68705 PB6 is IRQ for the slot", and
    // mouse.cpp:275-284 `mcu_port_b_w`: `if (!BIT(data, 6)) raise_slot_irq();
    // else lower_slot_irq();`). The PIA's IRQ-A/B outputs are wired but
    // dead — MAME's `pia_irqa_w` / `pia_irqb_w` (mouse.cpp:235-240) are
    // empty stubs — so deriving the line from them published a constant
    // false and swallowed any interrupt that was pending at snapshot time.
    // The pin state itself rides in the MCU blob restored just above; the
    // wire-OR lives in SlotPeripheral, so it has to be re-published here.
    // Nothing else re-arms it: `onMcuPortWrite` only fires when the
    // firmware WRITES port B, which it next does after the host services
    // the request that was already outstanding.
    assertIrq((mcu.getPortPins(1) & 0x40) == 0);
}
