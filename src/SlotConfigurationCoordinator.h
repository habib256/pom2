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

// Owns the two configuration values which are not machine topology:
//
//   effectivePlan_  settings after profile/uniqueness policy
//   draft_          staged UI edits, never applied implicitly
//
// SlotBus remains the sole authority for what is actually plugged. A live
// snapshot is copied from it explicitly instead of mutating either map when a
// ROM is missing or a session-only card is auto-provisioned.

#ifndef POM2_SLOT_CONFIGURATION_COORDINATOR_H
#define POM2_SLOT_CONFIGURATION_COORDINATOR_H

#include "SystemProfile.h"

#include <array>
#include <cstddef>
#include <string>

class SlotBus;

namespace pom2 {

class Settings;

class SlotConfigurationCoordinator
{
public:
    using CardMap = std::array<std::string, 8>;

    struct LiveSnapshot {
        CardMap keys{};
        CardMap names{};

        bool plugged(int slot) const noexcept
        {
            return slot > 0 && slot < static_cast<int>(keys.size()) &&
                   !names[static_cast<std::size_t>(slot)].empty();
        }
    };

    /// Rebuild the effective plan from settings, then apply machine-profile
    /// fixtures and the single-instance policy. Also discards any stale draft.
    /// Index zero stays empty.
    const CardMap& resolve(const Settings& settings, SystemProfile profile);

    const CardMap& effectivePlan() const noexcept { return effectivePlan_; }

    /// Explicitly mutable because this is the staged editor value, not live
    /// hardware and not the effective plan used for construction.
    CardMap& draft() noexcept { return draft_; }
    const CardMap& draft() const noexcept { return draft_; }
    void resetDraft() { draft_ = effectivePlan_; }

    /// Copy the actual SlotBus topology. The caller must hold the machine
    /// state lock for the duration of this call.
    LiveSnapshot captureLive(const SlotBus& bus) const;

    static bool isMultiInstance(const std::string& cardKey) noexcept;

    /// The card key the single-instance policy DROPPED from `slot` during the
    /// last resolve(), or empty. The saved `slot_N_card` still names that
    /// card, and the effective plan does not — so a caller that persists the
    /// plan back (the shutdown writer) would silently overwrite the user's
    /// key with "". Ask here before writing an empty slot.
    const std::string& dedupCleared(int slot) const noexcept
    {
        static const std::string kNone;
        return (slot > 0 && slot < static_cast<int>(dedupCleared_.size()))
                   ? dedupCleared_[static_cast<std::size_t>(slot)] : kNone;
    }

    /// True when `cardKey` is single-instance AND some slot other than
    /// `exceptSlot` already holds it in the effective plan. The Abstraction
    /// Levels panel asks before swapping a card for its other-level twin:
    /// the swap would otherwise resolve into a de-dup that empties the OTHER
    /// slot, and that slot's saved key with it.
    bool wouldDuplicate(const std::string& cardKey, int exceptSlot) const noexcept;

private:
    CardMap effectivePlan_{};
    CardMap draft_{};
    CardMap dedupCleared_{};
};

} // namespace pom2

#endif // POM2_SLOT_CONFIGURATION_COORDINATOR_H
