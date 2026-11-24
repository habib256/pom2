// POM2 Apple II Emulator — GPL-3.0-or-later
// Copyright (C) 2026 VERHILLE Arnaud
// cc65 debug information (ld65 --dbgfile / cl65 -g -Wl --dbgfile) turned into
// the two texts the debugger already reads: a `.p2map` source map
// (DebuggerSourceMap) and a symbol list (DebuggerSymbols). Nothing new is
// loaded by the panel — the conversion feeds the existing transactional
// importers, so their bounds and refusals still apply.
//
// [B] cc65 ld65 source @ eebf54af (2026-11): the file is one record per line,
// `kind<TAB>key=value,key=value…`. Strings are quoted and NOT escaped
// (dbgfile.c / segments.c print "%s" verbatim). The records used here:
//   seg   id, start=0x%06lX (absolute), size, addrsize   — segments.c:641
//   span  id, seg, start (offset in the segment), size    — span.c:229
//   line  id, file, line, [type], [count], [span=a+b…]   — lineinfo.c:267
//   file  id, name                                       — fileinfo.c:243
//   scope id, name, [parent]                              — scopes.c
//   sym   id, name, [scope|parent], val, [seg], type=lab|equ|imp — dbgsyms.c:368
// Line types (lidefs.h): 0 assembler source, 1 external (the C source cc65
// passes through), 2 macro expansion, 3 macro parameter.
//
// Choices the format does not make for us:
//  * An address covered by several line records takes the external (C) line
//    first, then the assembler line, then a macro body line; parameter
//    expansions are ignored. Equal kinds: the narrower span wins (a C
//    statement inside a C statement). DebuggerSourceMap refuses overlapping
//    ranges, so the map is built per byte and run-length encoded.
//  * cc65 bank numbers come from the linker config and name nothing POM2
//    knows, so ranges and symbols are written unqualified (the fallback for
//    every bank) unless the caller names a POM2 bank.
//  * Symbols: labels only. An `equ` is as often a constant (COUNT = 5) as an
//    address, and a constant would label zero page. Scoped labels are
//    written the ca65 way, `draw::loop`; a cheap local hangs off its owner,
//    `start@loop`. A name already taken by a different address is skipped
//    and counted, since one conflict refuses a whole DebuggerSymbols import.
#ifndef POM2_CC65_DEBUG_INFO_H
#define POM2_CC65_DEBUG_INFO_H

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pom2::cc65dbg {

struct Conversion {
    std::string text;          ///< .p2map or symbol-list text
    std::size_t entries = 0;   ///< ranges or symbols written
    std::size_t skipped = 0;   ///< records left out (out of range, name conflict)
};

namespace detail {

using Attrs = std::map<std::string, std::string, std::less<>>;

inline bool number(std::string_view text, unsigned long& out)
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2); base = 16;
    }
    if (text.empty()) return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out, base);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

inline bool attr(const Attrs& a, std::string_view key, unsigned long& out)
{
    const auto it = a.find(key);
    return it != a.end() && number(it->second, out);
}

inline std::string text(const Attrs& a, std::string_view key)
{
    const auto it = a.find(key);
    return it == a.end() ? std::string{} : it->second;
}

/// `kind<TAB>k=v,k="v",…` → kind + attributes. False on a malformed line.
inline bool record(std::string_view line, std::string& kind, Attrs& out)
{
    out.clear();
    const auto tab = line.find_first_of("\t ");
    if (tab == std::string_view::npos) { kind = std::string(line); return !kind.empty(); }
    kind = std::string(line.substr(0, tab));
    std::size_t i = tab;
    while (i < line.size() && (line[i] == '\t' || line[i] == ' ')) ++i;
    while (i < line.size()) {
        const auto eq = line.find('=', i);
        if (eq == std::string_view::npos) return false;
        std::string key(line.substr(i, eq - i));
        std::size_t v = eq + 1;
        std::string value;
        if (v < line.size() && line[v] == '"') {
            const auto close = line.find('"', v + 1);
            if (close == std::string_view::npos) return false;
            value = std::string(line.substr(v + 1, close - v - 1));
            i = close + 1;
        } else {
            const auto comma = line.find(',', v);
            const auto stop = comma == std::string_view::npos ? line.size() : comma;
            value = std::string(line.substr(v, stop - v));
            i = stop;
        }
        if (key.empty()) return false;
        out[std::move(key)] = std::move(value);
        if (i < line.size()) {
            if (line[i] != ',') return false;
            ++i;
        }
    }
    return true;
}

struct Seg   { unsigned long start = 0; };
struct Span  { unsigned long seg = 0, start = 0, size = 0; };
struct Line  { unsigned long file = 0, line = 0, type = 0; std::vector<unsigned long> spans; };
struct Scope { std::string name; long parent = -1; };
struct Sym   { std::string name; long scope = -1, parent = -1; unsigned long val = 0; bool label = false; };

struct Info {
    std::map<unsigned long, std::string> files;
    std::map<unsigned long, Seg> segs;
    std::map<unsigned long, Span> spans;
    std::vector<Line> lines;
    std::map<unsigned long, Scope> scopes;
    std::map<unsigned long, Sym> syms;
};

inline bool parse(std::string_view input, Info& info, std::string& error)
{
    std::size_t pos = 0, number_ = 0;
    bool versioned = false;
    std::string kind; Attrs a;
    while (pos < input.size()) {
        auto end = input.find('\n', pos);
        if (end == std::string_view::npos) end = input.size();
        std::string_view line = input.substr(pos, end - pos);
        pos = end + 1; ++number_;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (!record(line, kind, a)) {
            error = "Malformed cc65 debug record at line " + std::to_string(number_) + ".";
            return false;
        }
        unsigned long id = 0;
        if (kind == "version") {
            unsigned long major = 0;
            if (!attr(a, "major", major) || major != 2) {
                error = "Unsupported cc65 debug-info version (POM2 reads major=2).";
                return false;
            }
            versioned = true;
            continue;
        }
        if (!versioned) { error = "Not a cc65 debug file (no version record first)."; return false; }
        const bool hasId = attr(a, "id", id);
        if (kind == "file" && hasId) info.files[id] = text(a, "name");
        else if (kind == "seg" && hasId) { Seg s; if (attr(a, "start", s.start)) info.segs[id] = s; }
        else if (kind == "span" && hasId) {
            Span s;
            if (attr(a, "seg", s.seg) && attr(a, "start", s.start) && attr(a, "size", s.size))
                info.spans[id] = s;
        } else if (kind == "line") {
            Line l;
            if (!attr(a, "file", l.file) || !attr(a, "line", l.line)) continue;
            attr(a, "type", l.type);
            const std::string spans = text(a, "span");
            std::size_t p = 0;
            while (p < spans.size()) {
                const auto plus = spans.find('+', p);
                const auto stop = plus == std::string::npos ? spans.size() : plus;
                unsigned long s = 0;
                if (number(std::string_view(spans).substr(p, stop - p), s)) l.spans.push_back(s);
                p = stop + 1;
            }
            if (!l.spans.empty()) info.lines.push_back(std::move(l));
        } else if (kind == "scope" && hasId) {
            Scope s; s.name = text(a, "name");
            unsigned long parent = 0;
            if (attr(a, "parent", parent)) s.parent = static_cast<long>(parent);
            info.scopes[id] = std::move(s);
        } else if (kind == "sym" && hasId) {
            Sym s; s.name = text(a, "name");
            unsigned long v = 0;
            if (attr(a, "scope", v)) s.scope = static_cast<long>(v);
            if (attr(a, "parent", v)) s.parent = static_cast<long>(v);
            s.label = text(a, "type") == "lab" && attr(a, "val", s.val);
            info.syms[id] = std::move(s);
        }
    }
    if (!versioned) { error = "Not a cc65 debug file (no version record)."; return false; }
    return true;
}

inline std::string scopePath(const Info& info, long id)
{
    std::vector<std::string> parts;
    for (int depth = 0; id >= 0 && depth < 64; ++depth) {
        const auto it = info.scopes.find(static_cast<unsigned long>(id));
        if (it == info.scopes.end()) break;
        if (!it->second.name.empty()) parts.push_back(it->second.name);
        id = it->second.parent;
    }
    std::string path;
    for (auto p = parts.rbegin(); p != parts.rend(); ++p) path += *p + "::";
    return path;
}

inline std::string symbolName(const Info& info, const Sym& sym, int depth = 0)
{
    if (sym.parent >= 0 && depth < 64) {
        const auto owner = info.syms.find(static_cast<unsigned long>(sym.parent));
        if (owner != info.syms.end()) return symbolName(info, owner->second, depth + 1) + sym.name;
    }
    return scopePath(info, sym.scope) + sym.name;
}

inline std::string qualified(std::string_view bank, unsigned addr)
{
    std::ostringstream out;
    if (!bank.empty()) out << bank << ':';
    out << '$' << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << addr;
    return out.str();
}

} // namespace detail

/// The .p2map text for the lines of `dbg`. `bank` is a POM2 bank qualifier
/// ("aux1", "main") or empty for the unqualified fallback.
inline bool toSourceMap(std::string_view dbg, Conversion& out, std::string& error,
                        std::string_view bank = {})
{
    out = {}; error.clear();
    detail::Info info;
    if (!detail::parse(dbg, info, error)) return false;
    // Rank: external (C) 3, assembler 2, macro body 1; parameter expansions skipped.
    struct Owner { uint8_t rank = 0; unsigned long size = 0; long line = -1; };
    std::vector<Owner> owner(0x10000);
    for (std::size_t i = 0; i < info.lines.size(); ++i) {
        const auto& l = info.lines[i];
        const uint8_t rank = l.type == 1 ? 3 : l.type == 0 ? 2 : l.type == 2 ? 1 : 0;
        if (rank == 0 || l.line == 0 || l.line > 1000000) { ++out.skipped; continue; }
        for (unsigned long spanId : l.spans) {
            const auto span = info.spans.find(spanId);
            if (span == info.spans.end() || span->second.size == 0) continue;
            const auto seg = info.segs.find(span->second.seg);
            if (seg == info.segs.end()) continue;
            const unsigned long first = seg->second.start + span->second.start;
            const unsigned long last = first + span->second.size - 1;
            if (last > 0xFFFF) { ++out.skipped; continue; }
            for (unsigned long a = first; a <= last; ++a) {
                Owner& o = owner[a];
                if (rank > o.rank || (rank == o.rank && span->second.size < o.size))
                    o = {rank, span->second.size, static_cast<long>(i)};
            }
        }
    }
    std::ostringstream map;
    map << "# Converted by POM2 from cc65 debug information.\n";
    unsigned a = 0;
    while (a < 0x10000) {
        if (owner[a].line < 0) { ++a; continue; }
        const auto& l = info.lines[static_cast<std::size_t>(owner[a].line)];
        unsigned b = a;
        while (b + 1 < 0x10000 && owner[b + 1].line >= 0) {
            const auto& n = info.lines[static_cast<std::size_t>(owner[b + 1].line)];
            if (n.file != l.file || n.line != l.line) break;
            ++b;
        }
        const auto file = info.files.find(l.file);
        if (file == info.files.end() || file->second.empty() || file->second.size() > 4096) {
            ++out.skipped;
        } else {
            map << detail::qualified(bank, a) << '-' << detail::qualified({}, b)
                << ' ' << std::quoted(file->second) << ' ' << l.line << '\n';
            ++out.entries;
        }
        a = b + 1;
    }
    if (out.entries == 0) {
        error = "The cc65 debug file has no source lines (assemble and link with -g).";
        return false;
    }
    out.text = map.str();
    return true;
}

/// DebuggerSymbols text (`name = $0800`) for the labels of `dbg`.
inline bool toSymbols(std::string_view dbg, Conversion& out, std::string& error,
                      std::string_view bank = {})
{
    out = {}; error.clear();
    detail::Info info;
    if (!detail::parse(dbg, info, error)) return false;
    std::map<std::string, unsigned long> taken;
    std::ostringstream list;
    for (const auto& [id, sym] : info.syms) {
        (void)id;
        if (!sym.label) continue;
        const std::string name = detail::symbolName(info, sym);
        const bool valid = !name.empty() && name.size() <= 255 && sym.val <= 0xFFFF &&
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.$@:")
                == std::string::npos;
        const auto seen = taken.find(name);
        if (!valid || (seen != taken.end() && seen->second != sym.val) || taken.size() >= 65536) {
            ++out.skipped; continue;
        }
        if (seen != taken.end()) continue;           // same name, same address
        taken.emplace(name, sym.val);
        list << name << " = " << detail::qualified(bank, static_cast<unsigned>(sym.val)) << '\n';
        ++out.entries;
    }
    if (out.entries == 0) {
        error = "The cc65 debug file has no labels.";
        return false;
    }
    out.text = list.str();
    return true;
}

/// True when `path` names a cc65 debug file (".dbg", any case).
inline bool isDebugFilePath(std::string_view path)
{
    if (path.size() < 4) return false;
    const auto ext = path.substr(path.size() - 4);
    return ext.size() == 4 && ext[0] == '.' &&
        (ext[1] | 0x20) == 'd' && (ext[2] | 0x20) == 'b' && (ext[3] | 0x20) == 'g';
}

} // namespace pom2::cc65dbg

#endif
