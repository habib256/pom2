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

// InstructionTrace — see the header.

#include "InstructionTrace.h"

#include "Disassembler6502.h"
#include "M6502.h"
#include "Memory.h"

#include <array>
#include <cstring>

namespace pom2 {

namespace {

std::string hex(unsigned v, int digits)
{
    char b[8];
    std::snprintf(b, sizeof b, "%0*X", digits, v);
    return b;
}

std::string padded(std::string s, std::size_t width)
{
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

} // namespace

InstructionTrace::~InstructionTrace() { close(); }

bool InstructionTrace::open(const std::filesystem::path& path, std::string format,
                            const M6502& cpu, const Memory& memory, std::string& error)
{
    if (!openFile(path, std::move(format), error)) return false;
    attach(cpu, memory);
    return true;
}

bool InstructionTrace::openFile(const std::filesystem::path& path, std::string format,
                                std::string& error)
{
    close();
    cpu_ = nullptr;
    memory_ = nullptr;
#if defined(_WIN32)
    out_ = _wfopen(path.c_str(), L"wb");
#else
    out_ = std::fopen(path.c_str(), "wb");
#endif
    if (!out_) { error = "cannot create " + path.u8string(); return false; }
    // One write(2) per MB, not per line: `after` runs on the CPU worker
    // under the machine lock, once per instruction.
    std::setvbuf(out_, nullptr, _IOFBF, 1 << 20);
    stopped_.store(false, std::memory_order_relaxed);
    writeFailed_.store(false, std::memory_order_relaxed);
    format_ = format.empty() ? std::string(kDefaultFormat) : std::move(format);
    havePending_ = false;
    lines_.store(0, std::memory_order_relaxed);
    return true;
}

std::string InstructionTrace::stopReason() const
{
    if (writeFailed_.load(std::memory_order_relaxed)) return "the file could not be written (disk full?)";
    if (stopped_.load(std::memory_order_relaxed)) return "it reached its line cap";
    return {};
}

void InstructionTrace::write(const std::string& text)
{
    if (stopped_.load(std::memory_order_relaxed)) return;
    if (std::fwrite(text.data(), 1, text.size(), out_) != text.size() || std::ferror(out_)) {
        writeFailed_.store(true, std::memory_order_relaxed);
        stopped_.store(true, std::memory_order_relaxed);
        return;
    }
    const uint64_t n = lines_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (maxLines_ && n >= maxLines_) stopped_.store(true, std::memory_order_relaxed);
}

bool InstructionTrace::close()
{
    bool ok = true;
    if (out_) ok = std::fclose(out_) == 0;
    out_ = nullptr;
    havePending_ = false;
    if (!ok) writeFailed_.store(true, std::memory_order_relaxed);
    return ok && !writeFailed_.load(std::memory_order_relaxed);
}

std::string InstructionTrace::formatLine(const std::string& format, const Fields& f)
{
    static constexpr struct { const char* name; int id; } kTokens[] = {
        // Longest first: %PC must not swallow the P of %P, nor %SP the S.
        { "TOTAL", 0 }, { "BYTES", 1 }, { "FLAGS", 2 }, { "LABEL", 3 }, { "BANK", 4 },
        { "DIS", 5 }, { "CYC", 6 }, { "PC", 7 }, { "SP", 8 }, { "A", 9 }, { "X", 10 },
        { "Y", 11 }, { "P", 12 },
    };
    std::string out;
    out.reserve(format.size() + 48);
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') { out.push_back(format[i]); continue; }
        if (i + 1 < format.size() && format[i + 1] == '%') { out.push_back('%'); ++i; continue; }
        int id = -1;
        std::size_t len = 0;
        for (const auto& t : kTokens) {
            const std::size_t n = std::strlen(t.name);
            if (format.compare(i + 1, n, t.name) == 0) { id = t.id; len = n; break; }
        }
        if (id < 0) { out.push_back('%'); continue; }   // not a token: kept as written
        i += len;
        switch (id) {
            case 0: out += std::to_string(f.total); break;
            case 1: {
                std::string b;
                for (int k = 0; k < 3; ++k) {
                    if (k) b.push_back(' ');
                    b += k < f.length ? hex(f.bytes[k], 2) : std::string("  ");
                }
                out += b;
                break;
            }
            case 2: {
                static const char kNames[] = "NV-BDIZC";
                for (int bit = 7; bit >= 0; --bit) {
                    const char c = kNames[7 - bit];
                    const bool set = (f.p >> bit) & 1;
                    out.push_back(c == '-' ? '-' : set ? c : static_cast<char>(c + ('a' - 'A')));
                }
                break;
            }
            case 3: out += f.label; break;
            case 4: out += f.bank.text(); break;
            case 5: out += padded(f.disassembly, 16); break;
            case 6: out += std::to_string(f.cycles); break;
            case 7: out += hex(f.pc, 4); break;
            case 8: out += hex(f.sp, 2); break;
            case 9: out += hex(f.a, 2); break;
            case 10: out += hex(f.x, 2); break;
            case 11: out += hex(f.y, 2); break;
            case 12: out += hex(f.p, 2); break;
        }
    }
    return out;
}

void InstructionTrace::before(uint16_t pc, DebugBank bank)
{
    if (!out_ || !cpu_ || !memory_ || stopped()) return;
    Fields& f = pending_;
    f.pc = pc;
    f.bank = bank;
    // The disassembler reads a 64 KiB image; only the instruction's three
    // bytes matter, read through the CPU's own view (the bank it executes).
    static thread_local std::array<uint8_t, 0x10000> window{};
    for (int k = 0; k < 3; ++k) {
        const uint16_t at = static_cast<uint16_t>(pc + k);
        window[at] = memory_->peekCpuView(at);
        f.bytes[k] = window[at];
    }
    f.disassembly = disassemble6502(window.data(), pc, f.length, cpu_->instructionSet());
    if (f.length < 1 || f.length > 3) f.length = 1;
    f.label = symbols_.label(pc, bank);
    f.a = cpu_->getAccumulator();
    f.x = cpu_->getXRegister();
    f.y = cpu_->getYRegister();
    f.p = cpu_->getStatusRegister();
    f.sp = cpu_->getStackPointer();
    f.total = memory_->getCycleCounter();
    havePending_ = true;
}

void InstructionTrace::after(const M6502&, int cycles)
{
    if (!out_ || !havePending_) return;
    pending_.cycles = cycles;
    write(formatLine(format_, pending_) + '\n');
    havePending_ = false;
}

void InstructionTrace::interrupted(const M6502& cpu, int cycles)
{
    if (!out_ || stopped()) return;
    // The instruction announced by before() did not run: the CPU took the
    // interrupt instead, and the next line is the handler's first.
    char b[64];
    std::snprintf(b, sizeof b, "---- interrupt -> $%04X (%d cycles)\n",
                  cpu.getProgramCounter(), cycles);
    write(b);
    havePending_ = false;
}

} // namespace pom2
