// POM2 bank-qualified source ranges. Import is bounded and transactional.
#ifndef POM2_DEBUGGER_SOURCE_MAP_H
#define POM2_DEBUGGER_SOURCE_MAP_H
#include "DebuggerSymbols.h"
#include <algorithm>
#include <iterator>
#include <iomanip>
#include <sstream>
#include <vector>
namespace pom2 {
class DebuggerSourceMap {
public:
    struct Entry { DebugAddress start; uint16_t end; std::string file; unsigned line; };
    bool load(const std::string& text, std::string& error) {
        error.clear();
        if (text.size()>4*1024*1024) { error="Source map exceeds 4 MiB."; return false; }
        std::vector<Entry> entries;
        std::map<DebugBank,uint16_t> ends;
        std::istringstream input(text);
        std::string row; unsigned number=0;
        DebuggerSymbols parser;
        while (std::getline(input,row)) {
            ++number;
            std::istringstream words(row);
            words >> std::ws;
            if (words.eof() || words.peek()=='#') continue;
            Entry entry{}; std::string range, extra; uint16_t end=0;
            words >> range >> std::quoted(entry.file) >> entry.line;
            const auto dash=range.find('-');
            const bool valid = words && !(words >> extra) && entry.line>0 && entry.line<=1000000 &&
                !entry.file.empty() && entry.file.size()<=4096 &&
                parser.resolveLocation(range.substr(0,dash),entry.start) &&
                (dash==std::string::npos || DebuggerSymbols::address(range.substr(dash+1),end));
            entry.end = dash==std::string::npos ? entry.start.addr : end;
            if (!valid || entry.end<entry.start.addr || entries.size()>=65536) {
                error="Invalid source range at line "+std::to_string(number); return false;
            }
            entries.push_back(std::move(entry));
        }
        // Sort independently per bank, reject ambiguous overlapping ranges.
        std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b) {
            return a.start.bank==b.start.bank ? a.start.addr<b.start.addr : a.start.bank<b.start.bank;
        });
        for (const auto& entry:entries) {
            auto previous=ends.find(entry.start.bank);
            if (previous!=ends.end() && previous->second>=entry.start.addr) {
                error="Overlapping source ranges."; return false;
            }
            ends[entry.start.bank]=entry.end;
        }
        entries_=std::move(entries); return true;
    }
    const Entry* lookup(DebugAddress address) const {
        if (const auto* exact=find(address)) return exact;
        address.bank={}; return find(address);
    }
    std::size_t size() const { return entries_.size(); }
private:
    const Entry* find(DebugAddress address) const {
        const auto after=std::upper_bound(entries_.begin(),entries_.end(),address,
            [](const DebugAddress& a,const Entry& b) {
                return a.bank==b.start.bank ? a.addr<b.start.addr : a.bank<b.start.bank;
            });
        if (after==entries_.begin()) return nullptr;
        const auto& entry=*std::prev(after);
        return entry.start.bank==address.bank && address.addr<=entry.end ? &entry : nullptr;
    }
    std::vector<Entry> entries_;
};
}
#endif
