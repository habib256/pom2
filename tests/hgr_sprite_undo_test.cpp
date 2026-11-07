// POM2 — GPL-3.0-or-later
#include "hgrsprite/HgrSpriteEditor.h"
#include <cassert>
#include <cstdio>
#include <chrono>
#include <filesystem>

namespace hgrsprite {
struct HgrSpriteEditorTestAccess {
    static void testGrabbedPalette()
    {
        HgrSpriteEditor editor(nullptr);
        editor.wBytes_ = 3;
        editor.hRows_ = 1;
        editor.destByteCol_ = editor.destRow_ = 0;
        std::vector<uint8_t> memory(65536, 0);
        memory[0x2000] = 0x81;
        memory[0x2001] = 0x80; // Palette flag alone is still an empty shape byte.
        memory[0x2002] = 0x02;
        editor.grabFromPage(memory);
        const auto grabbed = editor.scratch;
        std::vector<uint8_t> bytes;
        int width = 0, height = 0;
        editor.palGroup1_ = false;
        editor.buildSpriteBytes(bytes, width, height);
        assert(width == 3 && height == 1);
        assert((bytes == std::vector<uint8_t>{0x01, 0x00, 0x02}) &&
               "Violet/Green selector left grabbed Blue/Orange palette bits set");
        editor.palGroup1_ = true;
        editor.buildSpriteBytes(bytes, width, height);
        assert((bytes == std::vector<uint8_t>{0x81, 0x00, 0x82}));
        editor.palGroup1_ = false;
        editor.buildSpriteBytes(bytes, width, height);
        assert((bytes == std::vector<uint8_t>{0x01, 0x00, 0x02}));
        assert(editor.scratch == grabbed); // Output selection leaves the source intact.
    }

    // PAINT-1 (bug hunt 2026-11-01 round 3): Ctrl+Z while the pencil is still
    // down. The undo used to reverse the PREVIOUS stroke and leave this one
    // open; its recorded old values were then stale, and undoing it later
    // put back the pixels the first undo had removed.
    static void testUndoMidDrag()
    {
        using hgrpaint::HgrColor;
        HgrSpriteEditor editor(nullptr);
        const auto blank = editor.scratch;
        editor.beginStroke();
        assert(editor.recordPlot(0, 0, HgrColor::White));
        editor.commitStroke();
        const auto first = editor.scratch;
        // Second stroke, button still held, over the SAME byte.
        editor.beginStroke();
        editor.dragging = true;
        assert(editor.recordPlot(1, 0, HgrColor::White));
        const auto second = editor.scratch;
        editor.doUndo();   // Ctrl+Z mid-drag
        assert(!editor.dragging && "the open drag was not closed");
        assert(editor.scratch == first && "Ctrl+Z mid-drag did not undo the open stroke");
        editor.doUndo();
        assert(editor.scratch == blank && "the earlier stroke came back wrong");
        editor.doRedo();
        editor.doRedo();
        assert(editor.scratch == second);

        // A bulk op during a drag keeps the drag's pixels as their own group.
        editor.beginStroke();
        editor.dragging = true;
        assert(editor.recordPlot(3, 0, HgrColor::White));
        const auto beforeClear = editor.scratch;
        editor.transformClear();
        editor.doUndo();
        assert(editor.scratch == beforeClear);
        editor.doUndo();
        assert(editor.scratch == second && "the drag's pixels left the history");
    }

    static void run()
    {
        testGrabbedPalette();
        testUndoMidDrag();
        HgrSpriteEditor editor(nullptr);
        using hgrpaint::HgrColor;
        // Preserve an existing pixel alongside the new stroke in the same byte.
        hgrpaint::plotPage(editor.scratch.data(), 6, 0, HgrColor::White);
        const auto before = editor.scratch;
        editor.beginStroke();
        for (const int x : {0, 1, 2})
            for (const int y : {0, 1})
                assert(editor.recordPlot(x, y, HgrColor::White));
        editor.commitStroke();
        const auto drawn = editor.scratch;
        assert(drawn != before);
        for (int repeat = 0; repeat < 2; ++repeat) {
            editor.doUndo();
            assert(editor.scratch == before && "undo left part of the pencil stroke behind");
            editor.doRedo();
            assert(editor.scratch == drawn);
        }
        // Eraser strokes also touch a byte repeatedly and must reverse cleanly.
        editor.beginStroke();
        for (const int x : {0, 1, 2})
            assert(editor.recordPlot(x, 0, HgrColor::Black));
        editor.commitStroke();
        editor.doUndo();
        assert(editor.scratch == drawn);
        // Bulk edits record one edit per byte and share the same replay path.
        editor.transformClear();
        editor.doUndo();
        assert(editor.scratch == drawn);
        assert(editor.redo.size() == 1);

        // A read error must preserve the picture and both history stacks.
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() /
            ("pom2-sprite-read-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(fs::create_directory(dir));
        const auto undoCount = editor.undo.size();
        const auto redoCount = editor.redo.size();
        editor.performFileAction(HgrSpriteEditor::FileAction::LoadSprite, dir.string());
        assert(editor.scratch == drawn && "failed read overwrote the sprite");
        assert(editor.undo.size() == undoCount && editor.redo.size() == redoCount);
        assert(editor.status.find("Loaded") == std::string::npos);

        // A valid short file still loads and zero-pads the remaining region.
        const auto file = dir / "short.hgrspr";
        std::FILE* out = std::fopen(file.string().c_str(), "wb");
        assert(out);
        assert(std::fputc(0x42, out) != EOF);
        assert(std::fclose(out) == 0);
        editor.performFileAction(HgrSpriteEditor::FileAction::LoadSprite, file.string());
        const auto loaded = editor.snapshotRegion();
        assert(!loaded.empty() && loaded.front().second == 0x42);
        for (size_t i = 1; i < loaded.size(); ++i) assert(loaded[i].second == 0);
        editor.doUndo();
        assert(editor.scratch == drawn);
        fs::remove(file);
        fs::remove(dir);
    }
};
}

int main()
{
    hgrsprite::HgrSpriteEditorTestAccess::run();
    std::puts("hgr_sprite_undo: OK");
}
