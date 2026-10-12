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

// Panel catalog + registry — pins src/PanelCatalog.h and src/PanelRegistry.*.
//
// The UI used to describe every panel in six places at once: the settings
// load, the settings save, the palette's command list, the palette's dispatch
// table, the menu rows, and the browser build's "hide everything" block — plus
// a `bool showXxx` member per panel to tie them together. The registry exists
// so there is one description and one flag. These cases pin the properties
// that make the single list trustworthy, and the failures they describe are
// not hypothetical — they are what the six lists had already produced:
//
//   1. Ids, commands and titles are unique. A duplicate command makes one of
//      two panels unreachable from the palette; a duplicate settings key makes
//      two panels share one visibility bit.
//   2. Every panel persists, except the one that documents why it must not.
//      Seven panels had no key at all — the user opened them and they were
//      gone after a restart, and nothing said whether that was a decision.
//   3. Order comes from the CATALOG. Menus, palette, settings file and the
//      draw loop all iterate it, so the table is the thing you read to know
//      what the UI does, in the order it does it.
//   4. `drawAll()` draws what is open — and the one panel that asked to be
//      called while CLOSED, because it has to release a key latch on the
//      frame it is shut.
//   5. A catalog row nothing draws is reported: a menu entry that opens
//      nothing is silent in every other way.
//
// The PanelId ↔ row correspondence itself is a static_assert in the header
// (`panelCatalogIsComplete`), so a forgotten or duplicated row never reaches
// this test — it fails the build.

#include "PanelCatalog.h"
#include "PanelRegistry.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

// ── 1. Catalog invariants ────────────────────────────────────────────────
void testCatalogIsWellFormed()
{
    assert(pom2::kPanelCount > 0);

    std::set<std::string> commands, keys, titles;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        assert(p.command && *p.command);
        assert(p.title && *p.title);
        // The command id is what the palette dispatches and what a future key
        // binding will name — it outlives the title, so it has a shape rather
        // than being free text.
        assert(std::strncmp(p.command, "panel.", 6) == 0 && "commands are namespaced");
        assert(commands.insert(p.command).second && "duplicate panel command");
        if (p.settingsKey) {
            assert(*p.settingsKey);
            assert(std::strncmp(p.settingsKey, "show_", 5) == 0);
            assert(keys.insert(p.settingsKey).second && "duplicate settings key");
        }
        // Two rows with the same label in the same menu is a UI bug that
        // compiles: the user cannot tell which window they are opening.
        assert(titles.insert(p.title).second && "duplicate panel title");
        // The lookup the whole design rests on.
        assert(&pom2::panelInfo(p.id) == &p);
    }

    std::printf("[ OK ] catalog: %zu panels, commands/keys/titles unique\n",
                pom2::kPanelCount);
}

// ── 2. Persistence is the rule; not persisting is the exception ──────────
void testEveryPanelPersistsExceptTheDocumentedOne()
{
    std::size_t unpersisted = 0;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog)
        if (!p.settingsKey) ++unpersisted;

    // Exactly one: the Welcome panel, which a first launch with NO ROM opens
    // from the constructor before settings are read — a stored `false` would
    // cancel the greeting. Any other panel arriving without a key is the old
    // bug coming back (opened by the user, gone next launch), so this counts
    // rather than merely allowing.
    assert(unpersisted == 1 &&
           "a panel with no settings key must be a documented decision");
    std::printf("[ OK ] every panel persists but the one that documents why not\n");
}

// ── 3. Storage: fresh-install defaults, flags, hide-all ──────────────────
void testVisibilityStorage()
{
    pom2::PanelRegistry reg;

    // A fresh install opens exactly the panels the catalog says it does —
    // that used to be three `= true` member initialisers three hundred lines
    // apart in MainWindow.h.
    std::size_t open = 0;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        assert(reg.visible(p.id) == p.defaultOpen);
        if (p.defaultOpen) ++open;
    }
    assert(open == 4 && "the fresh-install set is a decision, not an accident");
    assert(reg.visible(pom2::PanelId::Drives));
    assert(reg.visible(pom2::PanelId::Keyboard));
    assert(!reg.visible(pom2::PanelId::ImageWriter));

    reg.visible(pom2::PanelId::Debugger) = true;
    assert(reg.visible(pom2::PanelId::Debugger));

    // hideAll closes everything — including panels added after it was
    // written, which is exactly what the 28 hand-written assignments it
    // replaced could not do.
    reg.hideAll();
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog)
        assert(!reg.visible(p.id));

    std::printf("[ OK ] defaults from the catalog, flags, hide-all\n");
}

// ── 4. The settings round-trip walks the catalog, in order ───────────────
void testPersistenceLoop()
{
    pom2::PanelRegistry reg;
    std::vector<std::string> seen;
    reg.forEachPersisted([&](const char* key, bool& flag) {
        seen.emplace_back(key);
        flag = true;                       // the load half writes through
    });

    std::size_t expect = 0;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog)
        if (p.settingsKey) ++expect;
    assert(seen.size() == expect);
    assert(seen.size() == pom2::kPanelCount - 1);

    // Catalog order, so the settings file reads like the menus rather than
    // like whatever order the flags were declared in.
    std::size_t i = 0;
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        if (!p.settingsKey) continue;
        assert(seen[i++] == p.settingsKey);
        assert(reg.visible(p.id) && "the callback must see the live flag");
    }

    std::printf("[ OK ] persistence loop: %zu keys, catalog order, live flags\n",
                seen.size());
}

// ── 5. Runtime bits: title, availability, hiding ─────────────────────────
void testRuntimeBits()
{
    pom2::PanelRegistry reg;
    const pom2::PanelId id = pom2::PanelId::Printer;

    assert(reg.title(id) == pom2::panelInfo(id).title);
    assert(reg.available(id) && "no predicate = always available");
    assert(!reg.hidden(id));

    bool plugged = false;
    reg.bind(id, { [&plugged] { return plugged ? std::string("Printer (slot 1)")
                                               : std::string("Printer (no card plugged)"); },
                   [&plugged] { return plugged; } });
    assert(!reg.available(id));
    assert(reg.title(id) == "Printer (no card plugged)");
    plugged = true;
    assert(reg.available(id));
    assert(reg.title(id) == "Printer (slot 1)");

    // hidden is not "unavailable": greyed says "plug something in", hidden
    // says "this cannot exist on this platform".
    reg.hideFromUi(id);
    assert(reg.hidden(id));
    assert(reg.available(id) && "hiding must not change availability");

    std::printf("[ OK ] dynamic titles, availability, hide-from-UI\n");
}

// ── 6. The draw loop ─────────────────────────────────────────────────────
void testDrawLoop()
{
    pom2::PanelRegistry reg;
    reg.hideAll();

    std::vector<std::string> drawn;
    // Give every panel a draw so `undrawn()` is empty at the end, and record
    // the order they fire in.
    for (const pom2::PanelInfo& p : pom2::kPanelCatalog) {
        const char* cmd = p.command;
        reg.setDraw(p.id, [&drawn, cmd] { drawn.emplace_back(cmd); });
    }
    assert(reg.undrawn().empty());

    // Nothing open → nothing drawn. The 43 calls this replaced each carried
    // their own guard; the loop is now the single place that decides.
    reg.drawAll();
    assert(drawn.empty());

    reg.visible(pom2::PanelId::Debugger)  = true;
    reg.visible(pom2::PanelId::MemViewer) = true;
    reg.drawAll();
    assert(drawn.size() == 2);
    // Catalog order, not the order they were opened or bound in.
    assert(drawn[0] == pom2::panelInfo(pom2::PanelId::MemViewer).command);
    assert(drawn[1] == pom2::panelInfo(pom2::PanelId::Debugger).command);

    // The exception the //e keyboard needs: drawn while CLOSED so it can see
    // its own close and release the Open-Apple / Solid-Apple latches. Without
    // this the guest holds a key forever with nothing left to release it.
    drawn.clear();
    reg.setDraw(pom2::PanelId::Keyboard,
                [&drawn] { drawn.emplace_back("keyboard"); }, /*always=*/true);
    reg.hideAll();
    reg.drawAll();
    assert(drawn.size() == 1 && drawn[0] == "keyboard");

    std::printf("[ OK ] draw loop: gated, catalog order, draw-when-closed\n");
}

// ── 7. A panel nothing draws is reported ─────────────────────────────────
void testUndrawnIsReported()
{
    pom2::PanelRegistry reg;
    const auto all = reg.undrawn();
    assert(all.size() == pom2::kPanelCount && "a fresh registry draws nothing");

    reg.setDraw(pom2::PanelId::Debugger, [] {});
    const auto rest = reg.undrawn();
    assert(rest.size() == pom2::kPanelCount - 1);
    for (const std::string& c : rest)
        assert(c != pom2::panelInfo(pom2::PanelId::Debugger).command);

    std::printf("[ OK ] undrawn panels are reported by name\n");
}

}  // namespace

int main()
{
    testCatalogIsWellFormed();
    testEveryPanelPersistsExceptTheDocumentedOne();
    testVisibilityStorage();
    testPersistenceLoop();
    testRuntimeBits();
    testDrawLoop();
    testUndrawnIsReported();
    std::printf("panel_registry: all assertions passed\n");
    return 0;
}
