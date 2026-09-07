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

// Settings — minimal "remember runtime config across sessions" facility.
//
// Stored at `${HOME}/.config/POM2/state.cfg` (or `${HOME}/.pom2_state`
// fallback when XDG dirs aren't available; or in `./pom2_state.cfg` when
// HOME itself is unset) — the directory comes from `pom2::userConfigDir()`,
// which is also where `imgui.ini` goes, and which is `/persistent` (the
// IDBFS mount) under Emscripten. Plain `key=value` lines, one per setting.
// Unknown keys are preserved on round-trip so a future binary that drops
// a setting doesn't lose user data; unknown keys are simply ignored on
// the get-side.
//
// Loading is lossy-safe: a missing file, malformed line, or unparseable
// value all fall back to the caller's default. The host (MainWindow) is
// responsible for sanity-checking each path it reads (e.g. mount only if
// the file still exists).
//
// Saving is atomic: writes to a sibling temporary — `state.cfg.<pid>-<n>.
// pom2tmp`, unique per process and per call, see `tempSiblingPath` — then
// renames it over the live file, so a crash mid-write never corrupts the
// existing config. (A fixed `state.cfg.tmp` was the old name; two POM2
// instances sharing one $HOME both opened it and published a file made of
// both.)

#ifndef POM2_SETTINGS_H
#define POM2_SETTINGS_H

#include <map>
#include <string>

namespace pom2 {

class Settings
{
public:
    /// Load `state.cfg` from the well-known location. Missing or
    /// malformed file → empty store (defaults will apply at the call
    /// site). Returns true if a file was successfully read; false on
    /// missing-file (not an error condition).
    bool load();

    /// Persist the current key/value store. Atomic rename. Returns true
    /// on success; logs a warning on failure (no exceptions).
    ///
    /// A save whose content matches the last one this process wrote is
    /// skipped — it would rewrite the file byte for byte. That is what lets
    /// the browser build call the whole persist path on a heartbeat (its
    /// MainWindow is never destroyed, so there is no "on exit" moment) at the
    /// cost of a map comparison rather than a file write and an IndexedDB
    /// round-trip per beat.
    bool save() const;

    /// Suppress ALL writes (kiosk). Set once when the session is or
    /// becomes settings-read-only; `save()` then returns true without
    /// touching the file. Central because the call sites are spread over
    /// the whole UI and cannot each be relied on to check.
    void setReadOnly(bool ro) { readOnly_ = ro; }
    bool readOnly() const     { return readOnly_; }

    /// Get a value or fall back to the default. Conversion failures
    /// also fall back. Booleans accept "true"/"false"/"1"/"0".
    std::string getString(const std::string& key, const std::string& def = "") const;
    bool        getBool  (const std::string& key, bool        def = false) const;
    int         getInt   (const std::string& key, int         def = 0)     const;
    float       getFloat (const std::string& key, float       def = 0.0f)  const;

    void setString(const std::string& key, std::string value);
    void setBool  (const std::string& key, bool   value);
    void setInt   (const std::string& key, int    value);
    void setFloat (const std::string& key, float  value);

    /// True when the store holds no keys — i.e. `load()` found no file, or
    /// found an empty one. The honest test for "is this a first run?", used
    /// by the browser build to decide whether a returning visitor's panel
    /// layout should be honoured or replaced by the chrome-light default.
    bool empty() const { return store.empty(); }

    /// Resolved file path (visible in About / log).
    std::string getStorePath() const;

private:
    std::map<std::string, std::string> store;
    bool readOnly_ = false;   // see setReadOnly()
    /// Snapshot of `store` as of the last successful write, and whether there
    /// has been one. Mutable because save() is const — it does not change what
    /// the store holds, only what is known about the file.
    mutable std::map<std::string, std::string> lastWritten_;
    mutable bool hasWritten_ = false;
};

} // namespace pom2

#endif // POM2_SETTINGS_H
