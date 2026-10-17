// POM2 — GPL-3.0-or-later
#pragma once

#include "ByteIO.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2::sessionio {
using Blob = std::vector<uint8_t>;
inline void blob(Blob& out, const Blob& value)
{
    byteio::putU32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
// Bounds are checked before arithmetic, including on wasm32.
struct Reader {
    const uint8_t* data;
    size_t size, pos = 0;
    bool ok = true;
    Reader(const uint8_t* p, size_t n) : data(p), size(n), ok(p || n == 0) {}
    bool need(size_t n) {
        if (!ok || pos > size || n > size - pos) { ok = false; return false; }
        return true;
    }
    uint8_t u8() { return need(1) ? data[pos++] : 0; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= uint32_t(data[pos++]) << (8 * i);
        return value;
    }
    bool blob(Blob& out, size_t limit) {
        const uint32_t n = u32();
        if (n > limit || !need(n)) { ok = false; return false; }
        out.assign(data + pos, data + pos + n); pos += n;
        return true;
    }
    bool done() const { return ok && pos == size; }
};
}
