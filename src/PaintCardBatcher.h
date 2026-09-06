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

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// PaintCardBatcher — shared "copy edits to the graphics card" plumbing for the
// HGR and TMS paint hosts (Pom1HgrPaintHost / Pom1TmsPaintHost).
//
// Both editors funnel every modification through one path: pokes made inside a
// begin/end bracket accumulate into a single vector and commit as ONE locked
// write + snapshot publish (a bulk fill/import/stamp is one publish, not
// thousands); a lone poke outside a bracket commits immediately as a batch of
// one — so there is a single commit entry point per card instead of parallel
// single/batch mechanisms.
//
// The depth counter is REENTRANT on purpose: a nested begin/end pair (e.g. an
// undo's applyOps firing while a shape stroke's batch is still open) must not
// clear the queue or flush it early — only the outermost begin clears and the
// outermost end commits. A plain bool here once let a nested end flip batching
// off so the outer stroke's remaining pokes escaped the batch (round-2 D1).

#ifndef POM1_PAINT_CARD_BATCHER_H
#define POM1_PAINT_CARD_BATCHER_H

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

class PaintCardBatcher
{
public:
    using Writes = std::vector<std::pair<uint16_t, uint8_t>>;
    // Commits a run of (address, value) writes to the card in one shot. Called
    // with the empty vector on an empty bracket — the host commit is expected to
    // no-op on empty (the controller batch writers already do).
    using Commit = std::function<void(const Writes&)>;

    explicit PaintCardBatcher(Commit commit) : commit_(std::move(commit)) {}

    // Queue a write while batching; otherwise commit it immediately as a batch of
    // one. `single_` keeps its capacity across calls, so the immediate path does
    // not reallocate per poke.
    void poke(uint16_t addr, uint8_t value)
    {
        if (depth_ > 0) { batch_.emplace_back(addr, value); return; }
        single_.assign(1, std::make_pair(addr, value));
        commit_(single_);
    }

    void begin() { if (depth_++ == 0) batch_.clear(); }

    void end()
    {
        if (depth_ > 0 && --depth_ == 0) {
            commit_(batch_);
            batch_.clear();
        }
    }

    // Open bracket count. Zero between operations is the invariant that keeps
    // pokes reaching the card: a leaked begin() strands every later poke in
    // `batch_` (unbounded growth, canvas silently diverging from the machine
    // for the rest of the session). Exposed so a caller — and a test — can
    // assert it rather than discover it as a stuck canvas.
    int depth() const { return depth_; }

private:
    Commit  commit_;
    int     depth_ = 0;
    Writes  batch_;    // coalesced writes for the current bracket
    Writes  single_;   // reused 1-element buffer for the immediate path
};

#endif // POM1_PAINT_CARD_BATCHER_H
