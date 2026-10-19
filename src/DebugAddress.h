// POM2 debugger physical address identity. No bus accesses or emulator dependencies.
#ifndef POM2_DEBUG_ADDRESS_H
#define POM2_DEBUG_ADDRESS_H
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <charconv>
#include <string_view>
#include <system_error>
#include <string>
#include <tuple>
namespace pom2 {
struct DebugBank {
    enum Kind : uint8_t { Any, Main, Aux, Language, AuxLanguage, Rom, Slot, Io, Flat, Ignored };
    Kind kind = Any;
    uint8_t index = 0, segment = 0;
    bool operator<(const DebugBank& b) const {
        return std::tie(kind,index,segment) < std::tie(b.kind,b.index,b.segment);
    }
    bool operator==(const DebugBank& b) const { return kind==b.kind && index==b.index && segment==b.segment; }
    bool operator!=(const DebugBank& b) const { return !(*this==b); }
    std::string text() const {
        switch (kind) {
        case Any: return "*"; case Main: return "main"; case Flat: return "flat";
        case Io: return "io"; case Ignored: return "ignored";
        case Aux: return "aux" + std::to_string(index);
        case Language: return "lc" + std::to_string(segment);
        case AuxLanguage: return "aux" + std::to_string(index) + "lc" + std::to_string(segment);
        case Rom: return "rom" + std::to_string(index);
        case Slot: return "slot" + std::to_string(index);
        }
        return "?";
    }
    static bool parse(std::string_view text, DebugBank& out) {
        DebugBank bank;
        if (text == "*") bank.kind = Any;
        else if (text == "main") bank.kind = Main;
        else if (text == "flat") bank.kind = Flat;
        else if (text == "io") bank.kind = Io;
        else if (text == "ignored") bank.kind = Ignored;
        else if (text.substr(0,3) == "aux") {
            text.remove_prefix(3);
            unsigned index = 0;
            const auto number = std::from_chars(text.data(),text.data()+text.size(),index,10);
            if (number.ec != std::errc{} || index > 127 ||
                (text.size() > 1 && text.front() == '0' && number.ptr > text.data()+1)) return false;
            bank.index = static_cast<uint8_t>(index);
            text.remove_prefix(static_cast<std::size_t>(number.ptr-text.data()));
            if (text.empty()) bank.kind = Aux;
            else {
                if (text.size() != 3 || text.substr(0,2) != "lc" || text[2] < '0' || text[2] > '2') return false;
                bank.kind = AuxLanguage;
                bank.segment = static_cast<uint8_t>(text[2]-'0');
            }
        } else if (text.size() == 3 && text.substr(0,2) == "lc" && text[2] >= '0' && text[2] <= '2') {
            bank.kind = Language; bank.segment = static_cast<uint8_t>(text[2]-'0');
        } else if (text.size() == 4 && text.substr(0,3) == "rom" && text[3] >= '0' && text[3] <= '1') {
            bank.kind = Rom; bank.index = static_cast<uint8_t>(text[3]-'0');
        } else if (text.size() == 5 && text.substr(0,4) == "slot" && text[4] >= '1' && text[4] <= '7') {
            bank.kind = Slot; bank.index = static_cast<uint8_t>(text[4]-'0');
        } else return false;
        out = bank;
        return true;
    }
};
struct DebugAddress {
    uint16_t addr = 0;
    DebugBank bank{};
    bool operator<(const DebugAddress& b) const {
        return addr != b.addr ? addr < b.addr : bank < b.bank;
    }
    bool operator==(const DebugAddress& b) const { return addr==b.addr && bank==b.bank; }
    bool operator!=(const DebugAddress& b) const { return !(*this==b); }
    std::string text() const {
        char hex[8]; std::snprintf(hex,sizeof(hex),":$%04X",addr);
        return bank.text()+hex;
    }
};
}
#endif
