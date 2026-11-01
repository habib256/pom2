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

// SettingRebuild — the one rule a setting the MACHINE has to take must
// follow: write it, ask for the rebuild, and on a refusal leave nothing
// behind.
//
// Same shape and same reason as `MouseGrab.h` / `KeyChord.h`: the policy used
// to live inside an ImGui callback, where no test could reach it, and two of
// the five call sites had drifted. The Abstraction Levels panel's CCS 7710 and
// DAN ][ firmware toggles saved the key BEFORE the rebuild and never undid it
// — so when `restartEmulationFromSettings()` refused (a dirty medium that
// cannot be flushed), the user was told "machine paused; apply again once
// fixed", the live machine rightly kept the old firmware, and the next launch
// quietly came up running the abstraction level they had been told could not
// be applied. The other three toggles went through `swapSlotCardVariant`,
// which states the contract: "the persisted key has to go back too or the
// refused change would apply silently on the next launch".

#ifndef POM2_SETTING_REBUILD_H
#define POM2_SETTING_REBUILD_H

#include "Settings.h"

#include <functional>
#include <string>
#include <utility>

namespace pom2 {

/// Stage `value` under `key`, hand the machine to `rebuild`, and on a refusal
/// put the key back EXACTLY as it was — an absent key stays absent, which is
/// a different statement from an empty one (`Settings::restore`). Returns what
/// `rebuild` returned; a null `rebuild` counts as a refusal, so a caller that
/// forgot to wire it undoes its write rather than persisting a change nothing
/// applied.
///
/// Deliberately does NOT save: the key is saved by the caller, and only once
/// the machine took it (`MainWindow::setSlotCardFromCli` says why — "saving
/// first left a refused card in state.cfg, to come back at the next launch").
inline bool applySettingWithRebuild(Settings& settings, const std::string& key,
                                    std::string value,
                                    const std::function<bool()>& rebuild)
{
    const Settings::Saved saved = settings.capture(key);
    settings.setString(key, std::move(value));
    if (rebuild && rebuild()) return true;
    settings.restore(key, saved);
    return false;
}

}  // namespace pom2

#endif  // POM2_SETTING_REBUILD_H
