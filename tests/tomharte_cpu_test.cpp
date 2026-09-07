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

// Tom Harte "SingleStepTests/65x02" ProcessorTests harness.
//
// Source of truth: https://github.com/SingleStepTests/65x02 — randomly
// generated, single-instruction test vectors that pin the full processor +
// memory state *before* and *after* one opcode, plus the per-cycle bus
// activity. Each opcode has its own `NN.json` file (NN = opcode hex) holding
// 10 000 vectors. We validate POM2's M6502 against:
//
//   * the NMOS 6502 set  → `6502/v1/*.json`
//   * the 65C02 CMOS set → `wdc65c02/v1/*.json`  (WDC W65C02S superset — the
//     only published variant that has BOTH the Rockwell bit ops
//     (SMBn/RMBn/BBRn/BBSn) AND WAI/STP, which is exactly POM2's CMOS table.)
//
// POM2's M6502 is **instruction-stepped**, not cycle-stepped: one `run(1)`
// executes a whole opcode and returns its cycle total, and `Memory::memRead/
// memWrite` are non-virtual (no per-access bus hook). So we cannot replay the
// exact per-cycle bus *order*. What we CAN — and do — validate cycle-exactly:
//
//   1. final A / X / Y / SP / PC                         (register file)
//   2. final P, comparing the 6 architectural flags      (N V D I Z C)
//      only; the phantom B(bit4) + unused(bit5) bits are masked because they
//      are not real register latches. Their *pushed* values (BRK sets B=1,
//      IRQ/NMI B=0) are still validated — they land in stack RAM and are
//      caught by the memory check below.
//   3. final RAM at every address the vector lists        (memory effects)
//   4. cycle count == length of the vector's `cycles[]`   (← the timing gate;
//      this is the class of bug that the hand-picked cpu_cycle_count_test was
//      built to catch, now generalised to every opcode × 10 000 states)
//
// POM2 models undocumented NMOS opcodes as length/cycle-correct NOP
// placeholders (see cpu_cycle_count_test.cpp), NOT their real silicon
// behaviour, so those vectors are expected to diverge. The default CTest run
// points at a curated set of *documented* opcodes (downloaded at configure
// time); pass `--skip` / `--only` to scope a hand-run, and use
// tests/fetch_tomharte.sh to pull the full 256-opcode set for exhaustive
// local validation.
//
// Usage:
//   tomharte_cpu_test <nmos|cmos> <dir-of-json> [--max N] [--only hh,hh]
//                     [--skip hh,hh] [--verbose] [--examples K]
// Exit 0 = every non-skipped vector matched; 1 = at least one mismatch;
// 2 = usage / I/O error; 77 = corpus absent, nothing was verified.
//
// 77 is ctest's SKIP_RETURN_CODE (wired in tests/CMakeLists.txt), NOT 0.
// An absent corpus must never read as a pass: this gate is the only
// cycle-level CPU oracle POM2 has, and a green line saying "Passed 0.00s"
// for a suite that ran zero vectors is the exact failure shape TODO.md
// names as the lesson of the 2026-08-28 pass — a mechanism that reports
// success while doing nothing.

#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

// ─────────────────────────── parsed vector model ───────────────────────────

struct RamCell { uint16_t addr; uint8_t val; };

struct CpuState {
    uint16_t pc = 0;
    uint8_t  s = 0, a = 0, x = 0, y = 0, p = 0;
    std::vector<RamCell> ram;
};

struct Vector {
    std::string name;       // e.g. "fe 12 34" — opcode + operand bytes
    CpuState    initial;
    CpuState    fin;
    int         cycleCount = -1;  // == final length of the JSON `cycles[]`
};

// ───────────────────── minimal, schema-aware JSON scanner ───────────────────
// Hand-rolled (POM2 vendors no JSON lib — AiControlServer.cpp parses by hand
// too). The upstream files are machine-generated and perfectly regular, so a
// single forward pass over the buffer is enough and stays fast on the ~5 MB,
// 10 000-object arrays. Values are all non-negative integers, plain strings
// (no escapes), nested arrays, and flat objects.

inline void skipWs(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

inline bool eat(const char*& p, char c) {
    skipWs(p);
    if (*p == c) { ++p; return true; }
    return false;
}

inline uint32_t parseUint(const char*& p) {
    skipWs(p);
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + uint32_t(*p - '0'); ++p; }
    return v;
}

inline void skipString(const char*& p) {
    skipWs(p);
    if (*p != '"') return;
    ++p;
    while (*p && *p != '"') ++p;
    if (*p == '"') ++p;
}

// Generic value-skip for any unexpected key (keeps the parser robust to JSON
// shape changes without crashing).
void skipValue(const char*& p) {
    skipWs(p);
    if (*p == '"') { skipString(p); return; }
    if (*p == '[' || *p == '{') {
        const char open = *p, close = (open == '[') ? ']' : '}';
        int depth = 0;
        while (*p) {
            if (*p == '"') { skipString(p); continue; }
            if (*p == open) ++depth;
            else if (*p == close) { if (--depth == 0) { ++p; return; } }
            ++p;
        }
        return;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') ++p;  // number / literal
}

// `[[addr,val], [addr,val], ...]`
void parseRam(const char*& p, std::vector<RamCell>& out) {
    out.clear();
    eat(p, '[');
    skipWs(p);
    if (*p == ']') { ++p; return; }
    while (true) {
        eat(p, '[');
        const uint32_t a = parseUint(p);
        eat(p, ',');
        const uint32_t v = parseUint(p);
        eat(p, ']');
        out.push_back({ uint16_t(a), uint8_t(v) });
        skipWs(p);
        if (*p == ',') { ++p; continue; }
        if (*p == ']') { ++p; break; }
        break;
    }
}

// `{ "pc":N, "s":N, "a":N, "x":N, "y":N, "p":N, "ram":[...] }` — dispatched by
// key so field order is irrelevant.
void parseState(const char*& p, CpuState& st) {
    eat(p, '{');
    while (true) {
        skipWs(p);
        if (*p == '}') { ++p; break; }
        if (*p == '"') {
            ++p;
            const char* k = p;
            while (*p && *p != '"') ++p;
            const size_t kl = size_t(p - k);
            if (*p == '"') ++p;
            eat(p, ':');
            if      (kl == 2 && k[0] == 'p' && k[1] == 'c')                 st.pc = uint16_t(parseUint(p));
            else if (kl == 1 && k[0] == 's')                               st.s  = uint8_t(parseUint(p));
            else if (kl == 1 && k[0] == 'a')                               st.a  = uint8_t(parseUint(p));
            else if (kl == 1 && k[0] == 'x')                               st.x  = uint8_t(parseUint(p));
            else if (kl == 1 && k[0] == 'y')                               st.y  = uint8_t(parseUint(p));
            else if (kl == 1 && k[0] == 'p')                               st.p  = uint8_t(parseUint(p));
            else if (kl == 3 && k[0] == 'r' && k[1] == 'a' && k[2] == 'm') parseRam(p, st.ram);
            else                                                           skipValue(p);
        }
        skipWs(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; break; }
    }
}

// `cycles` is `[[addr,val,"read"|"write"], ...]`; we only need its length (the
// architectural cycle count for the opcode in this state).
int countCycles(const char*& p) {
    eat(p, '[');
    skipWs(p);
    if (*p == ']') { ++p; return 0; }
    int n = 0, depth = 1;             // outer '[' already consumed
    while (*p && depth > 0) {
        if (*p == '"') { skipString(p); continue; }
        if (*p == '[') { if (depth == 1) ++n; ++depth; ++p; continue; }
        if (*p == ']') { --depth; ++p; continue; }
        ++p;
    }
    return n;
}

bool parseVector(const char*& p, Vector& v) {
    skipWs(p);
    if (*p != '{') return false;
    ++p;
    v.name.clear();
    v.cycleCount = -1;
    while (true) {
        skipWs(p);
        if (*p == '}') { ++p; break; }
        if (*p == '"') {
            ++p;
            const char* k = p;
            while (*p && *p != '"') ++p;
            const size_t kl = size_t(p - k);
            const char* key = k;
            if (*p == '"') ++p;
            eat(p, ':');
            if (kl == 4 && !std::strncmp(key, "name", 4)) {
                skipWs(p);
                if (*p == '"') { ++p; const char* s = p; while (*p && *p != '"') ++p; v.name.assign(s, size_t(p - s)); if (*p == '"') ++p; }
            } else if (kl == 7 && !std::strncmp(key, "initial", 7)) {
                parseState(p, v.initial);
            } else if (kl == 5 && !std::strncmp(key, "final", 5)) {
                parseState(p, v.fin);
            } else if (kl == 6 && !std::strncmp(key, "cycles", 6)) {
                v.cycleCount = countCycles(p);
            } else {
                skipValue(p);
            }
        }
        skipWs(p);
        if (*p == ',') { ++p; continue; }
        if (*p == '}') { ++p; break; }
    }
    return true;
}

// ─────────────────────────────── runner ────────────────────────────────────

constexpr uint8_t kPhantomMask = 0x30;   // B (bit4) + unused (bit5)

struct OpResult { int total = 0, passed = 0; };

struct Mismatch {
    std::string name, detail;
    uint8_t ia = 0, ip = 0;   // initial A / P, to characterise divergences
};

bool runVector(M6502& cpu, Memory& mem, const Vector& v, std::string& why) {
    cpu.setProgramCounter(v.initial.pc);
    cpu.setStackPointer(v.initial.s);
    cpu.setAccumulator(v.initial.a);
    cpu.setXRegister(v.initial.x);
    cpu.setYRegister(v.initial.y);
    cpu.setStatusRegister(v.initial.p);
    // Clear the KIL/JAM + STP halt latch. `step()` short-circuits before the
    // opcode fetch while it is set, and only a reset clears it on real
    // silicon — so a single vector that lands on an NMOS JAM ($02/$12/$22/…)
    // or a CMOS STP ($DB) would otherwise freeze the shared CPU for EVERY
    // later vector in the run. Each vector is an independent machine state,
    // so the latch has to be re-armed here alongside the register file.
    cpu.setHalted(false);
    for (const RamCell& c : v.initial.ram) mem.memWrite(c.addr, c.val);

    const int cyc = cpu.run(1);          // execute exactly one instruction

    bool ok = true;
    char buf[256];
    auto fail = [&](const char* what, unsigned got, unsigned want) {
        if (ok) { std::snprintf(buf, sizeof buf, "%s got $%X want $%X", what, got, want); why = buf; }
        ok = false;
    };
    if (cpu.getProgramCounter() != v.fin.pc)   fail("PC",  cpu.getProgramCounter(), v.fin.pc);
    if (cpu.getAccumulator()    != v.fin.a)    fail("A",   cpu.getAccumulator(),    v.fin.a);
    if (cpu.getXRegister()      != v.fin.x)    fail("X",   cpu.getXRegister(),      v.fin.x);
    if (cpu.getYRegister()      != v.fin.y)    fail("Y",   cpu.getYRegister(),      v.fin.y);
    if (cpu.getStackPointer()   != v.fin.s)    fail("SP",  cpu.getStackPointer(),   v.fin.s);
    if (((cpu.getStatusRegister() ^ v.fin.p) & ~kPhantomMask) != 0)
        fail("P", cpu.getStatusRegister() & ~kPhantomMask, v.fin.p & ~kPhantomMask);
    if (v.cycleCount >= 0 && cyc != v.cycleCount) {
        if (ok) { std::snprintf(buf, sizeof buf, "cycles got %d want %d", cyc, v.cycleCount); why = buf; }
        ok = false;
    }
    for (const RamCell& c : v.fin.ram) {
        const uint8_t got = mem.memRead(c.addr);
        if (got != c.val) { if (ok) { std::snprintf(buf, sizeof buf, "RAM[$%04X] got $%02X want $%02X", c.addr, got, c.val); why = buf; } ok = false; }
    }

    // Restore every touched cell so the flat RAM stays deterministic for the
    // next vector (Tom Harte guarantees each `initial.ram` lists every address
    // the opcode reads, so resetting reads ∪ writes back to 0 is sufficient).
    for (const RamCell& c : v.initial.ram) mem.memWrite(c.addr, 0);
    for (const RamCell& c : v.fin.ram)     mem.memWrite(c.addr, 0);
    return ok;
}

std::set<int> parseHexList(const char* s) {
    std::set<int> out;
    while (s && *s) {
        while (*s == ',' || *s == ' ') ++s;
        if (!*s) break;
        out.insert(int(std::strtol(s, nullptr, 16)));
        while (*s && *s != ',') ++s;
    }
    return out;
}

int opcodeFromStem(const std::string& stem) {
    if (stem.size() >= 2 && std::isxdigit((unsigned char)stem[0]) && std::isxdigit((unsigned char)stem[1]))
        return int(std::strtol(stem.substr(0, 2).c_str(), nullptr, 16));
    return -1;
}

}  // namespace

/// ctest's SKIP_RETURN_CODE. Returned whenever the run verified ZERO vectors,
/// so an unfetched corpus shows up as "Skipped" rather than "Passed".
static constexpr int kExitSkip = 77;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <nmos|cmos> <dir> [--max N] [--only hh,..] "
                             "[--skip hh,..] [--verbose] [--examples K]\n", argv[0]);
        return 2;
    }
    const std::string modeStr = argv[1];
    const std::string dir     = argv[2];
    M6502::CpuMode mode;
    if      (modeStr == "nmos") mode = M6502::CpuMode::NMOS;
    else if (modeStr == "cmos") mode = M6502::CpuMode::CMOS;
    else { std::fprintf(stderr, "mode must be nmos|cmos, got '%s'\n", argv[1]); return 2; }

    long       maxPerFile = -1;
    int        examples   = 3;
    bool       verbose    = false;
    std::set<int> only, skip;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--max"      && i + 1 < argc) maxPerFile = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--examples" && i + 1 < argc) examples   = int(std::strtol(argv[++i], nullptr, 10));
        else if (a == "--only"     && i + 1 < argc) only = parseHexList(argv[++i]);
        else if (a == "--skip"     && i + 1 < argc) skip = parseHexList(argv[++i]);
        else if (a == "--verbose")                  verbose = true;
        else { std::fprintf(stderr, "unknown arg '%s'\n", a.c_str()); return 2; }
    }

    namespace fs = std::filesystem;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::fprintf(stderr, "[tomharte] no test data at '%s' — SKIPPED, nothing verified "
                             "(set POM2_FETCH_TOMHARTE=ON or run fetch_tomharte.sh)\n", dir.c_str());
        return kExitSkip;
    }

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::fprintf(stderr, "[tomharte] '%s' holds no .json vectors — SKIPPED, nothing verified\n",
                     dir.c_str());
        return kExitSkip;
    }

    Memory mem;
    mem.setTestMode(true);
    M6502 cpu(&mem);
    cpu.setCpuMode(mode);
    cpu.hardReset();

    std::printf("[tomharte] CPU=%s  dir=%s  files=%zu\n",
                modeStr.c_str(), dir.c_str(), files.size());

    long grandTotal = 0, grandPass = 0, filesRun = 0;
    bool anyFail = false;

    for (const fs::path& f : files) {
        const std::string stem = f.stem().string();
        const int opc = opcodeFromStem(stem);
        if (!only.empty() && (opc < 0 || !only.count(opc))) continue;
        if (skip.count(opc)) { std::printf("  %-2s : SKIP (skiplist)\n", stem.c_str()); continue; }

        std::ifstream in(f, std::ios::binary);
        if (!in) { std::fprintf(stderr, "  %-2s : cannot open %s\n", stem.c_str(), f.c_str()); anyFail = true; continue; }
        std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        const char* p = buf.c_str();
        eat(p, '[');
        OpResult r;
        std::vector<Mismatch> firstFew;
        Vector v;
        while (true) {
            skipWs(p);
            if (*p == ']' || *p == '\0') break;
            if (!parseVector(p, v)) break;
            std::string why;
            const bool ok = runVector(cpu, mem, v, why);
            ++r.total;
            if (ok) ++r.passed;
            else if (int(firstFew.size()) < examples) firstFew.push_back({ v.name, why, v.initial.a, v.initial.p });
            skipWs(p);
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; break; }
            if (maxPerFile > 0 && r.total >= maxPerFile) break;
        }

        ++filesRun;
        grandTotal += r.total;
        grandPass  += r.passed;
        // A file that parsed to ZERO vectors is a failure, not a pass.
        // `r.passed == r.total` is 0 == 0 for a truncated or empty JSON, so
        // an opcode nobody verified used to print OK and count towards
        // `filesRun`, which also disarmed the "nothing verified" skip below.
        // The ctest path is protected by `pom2_download_pinned`'s SHA-256,
        // but the documented manual sweep (tests/fetch_tomharte.sh + this
        // binary) is not: a 404 leaves a 0-byte file behind.
        const bool fileOk = (r.total > 0 && r.passed == r.total);
        if (!fileOk) anyFail = true;
        std::printf("  %-2s : %6d/%-6d %s%s\n", stem.c_str(), r.passed, r.total,
                    fileOk ? "OK" : "FAIL",
                    r.total == 0 ? "  <<< no vectors parsed" : "");
        if (!fileOk || verbose) {
            for (const Mismatch& m : firstFew)
                std::printf("        ✗ \"%s\"  [A0=$%02X P0=$%02X D=%d]  %s\n",
                            m.name.c_str(), m.ia, m.ip, (m.ip & 0x08) ? 1 : 0, m.detail.c_str());
        }
    }

    std::printf("[tomharte] %s: %ld/%ld vectors passed across %ld opcode file(s)%s\n",
                anyFail ? "FAIL" : "OK", grandPass, grandTotal, filesRun,
                anyFail ? "  <<< MISMATCH" : "");
    if (filesRun == 0) {
        std::fprintf(stderr, "[tomharte] no opcode files matched the filter — SKIPPED, "
                             "nothing verified\n");
        return kExitSkip;
    }
    return anyFail ? 1 : 0;
}
