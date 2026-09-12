#!/usr/bin/env bash
# POM2 — coordinator lock-scope scan.
#
# Every pom2::*Coordinator capture/apply call takes the machine lock itself.
# `stateMutex` is NON-RECURSIVE, so such a call placed inside an existing
# lock scope hangs the UI thread and the emulator with it — and no test
# catches it, because nothing drives the ImGui panels (TODO § [Arch]).
#
# That is not hypothetical: wiring PrinterCoordinator into
# renderFujiNetPanelWindow put a captureHost() inside a
# lock_guard(stateMutex()) scope, the full suite stayed green, and the panel
# would have hung on first open.
#
# Run after touching any MainWindow*.cpp coordinator call site:
#     tools/check_coordinator_locks.sh
#
# Exit 0 = clean, 1 = at least one call site sits inside an open lock scope.
#
# BOTH lock spellings must be checked. The first version of this scan looked
# only for `lockState()` and therefore missed the bug above, which uses the
# bare `stateMutex()`.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

python3 - "$@" <<'PY'
import glob
import re
import sys

# Calls that resolve a card under the machine lock. Add new coordinators here.
COORD = (
    'mouseCoordinator_->',
    'printerCoordinator_->',
    'audioCoordinator_->',
    'devicePanelCoordinator_->',
    'slotProvisioningCoordinator_->',
    'slotConfigCoordinator_->',
    'slotCardFactory_->',
    'debugCoordinator_->',
    'networkCoordinator_->',
    'storageCoordinator_->',
    'slotConfigurationCoordinator_->',
    'slotProvisioningCoordinator_->',
    'slotRebuildCoordinator_->',
    'debugCoordinator_->',
)

# Members that deliberately take NO lock, so they are legal inside one.
# Keep this list short and justify every entry.
# Two shapes qualify, and nothing else:
#   1. plain accessors/mutators over the coordinator's own fields;
#   2. methods that take `SlotBus&` / `Settings&` DIRECTLY instead of an
#      EmulationController — that signature IS the contract saying "the caller
#      already holds the lock and proves it by handing me the bus".
# Check the declaration before adding an entry: if it takes an
# EmulationController it locks, and it does not belong here.
LOCK_FREE = (
    'resetFeedCursor',               # 1. plain member reset
    'markAutoProvisionedHdv',        # 1.
    'markAutoProvisionedSmartPort',  # 1.
    'autoProvisionedHdvSlot',        # 1.
    'autoProvisionedSmartPortSlot',  # 1.
    'clearAutoProvisioned',          # 1.
    'captureRebuildSnapshot',        # 2. takes SlotBus&
    'restoreRebuildSnapshot',        # 2. takes SlotBus&
    'restoreMediaFromSettings',      # 2. takes SlotBus&
    'persistRebuildSettings',        # 2. takes Settings& + a value snapshot
    'persistSessionSettings',        # 2.
    # 2. const, takes `const Settings&`, returns a POD. Two getFloat/getBool
    #    lookups in an in-memory std::map — Settings::load() is the separate
    #    disk step — so no controller, no machine lock and no file I/O. Its
    #    three call sites sit in plugSlotsFromSettings, whose StateAccess
    #    parameter correctly makes the whole body count as locked; the calls
    #    are safe there for the same reason persistRebuildSettings is.
    'restoreCardSettings',
    'persistDiskIIDrive',            # 2. takes SlotBus&
    'flushAll',                      # 2. takes SlotBus&
    'topology',                      # 2. takes SlotBus&
    # 4. SlotCardFactory::create takes only a Request — no controller, no bus,
    #    so it cannot take the lock. It DOES read ROM files, which is why it
    #    is called from plugSlotsFromSettings under the rebuild's lock: that
    #    is the documented profile-switch exception (the CPU worker is stopped
    #    across a rebuild anyway). See CLAUDE.md, "Never hold stateMutex
    #    across file I/O".
    'create',
    # NetworkCoordinator's host-only state: no controller, so no machine lock.
    # setHelperPath does search PATH, which is filesystem I/O — acceptable at
    # its one locked call site for the same reason SlotCardFactory::create's
    # ROM reads are: it is the profile-switch rebuild, where the CPU worker is
    # stopped anyway. It is not acceptable anywhere else.
    'setHelperPath',
    'helperPath',
    'helperResolved',
    'serialDevices',
    'rescanSerialDevices',
    'setStatus',
    'clearStatus',
    'captureLive',                   # 2. takes SlotBus&
    'resolve',                       # 1. plan resolution over Settings
    'effectivePlan',                 # 1.
    'draft',                         # 1.
    'resetDraft',                    # 1.
    # 3. A third shape, and the clearest: the name ends in `Locked` and the
    #    method takes a `const StateAccess&` — a token that exists only
    #    because the caller took the lock. These MUST be called inside one,
    #    so flagging them would be backwards.
    'beginLocked',
    'publishLocked',
    'prepareAfterFlush',             # 1. mutates coordinator state + hooks
    'phase',                         # 1.
    'generation',                    # 1.
)

FUNC_START = re.compile(r'^[A-Za-z_][\w:<>,&*\s]*::\w+\(')

def enclosing_function(lines, idx):
    for j in range(idx, -1, -1):
        if FUNC_START.match(lines[j]):
            return j
    return 0

def lock_still_open(lines, fn_start, call_idx):
    """Nearest preceding lock, then check whether its block has closed."""
    # A function that RECEIVES a StateAccess is locked for its whole body —
    # the parameter is the proof of ownership, and there is no lockState()
    # call inside for the search below to find. MainWindow::plugSlotsFromSettings
    # is the one that matters; treat the whole body as inside the lock.
    signature = lines[fn_start]
    if 'StateAccess' in signature:
        return fn_start + 1

    lock = None
    for j in range(call_idx - 1, fn_start - 1, -1):
        code = lines[j].split('//')[0]
        if 'lockState()' in code or 'stateMutex()' in code:
            lock = j
            break
    if lock is None:
        return None
    # Count braces CHARACTER by character, not per line. `} else {` closes the
    # lock's block and opens another, netting zero — a per-line counter never
    # sees the depth go negative and reports a scope that has in fact ended.
    depth = 0
    for j in range(lock, call_idx):
        code = lines[j].split('//')[0]
        for ch in code:
            if ch == '{':
                depth += 1
            elif ch == '}':
                depth -= 1
                if j > lock and depth < 0:
                    return None      # scope closed before the call
    return lock + 1

bad = 0
checked = 0
for path in sorted(glob.glob('src/MainWindow*.cpp')):
    lines = open(path, encoding='utf-8').read().split('\n')
    for i, line in enumerate(lines):
        if not any(c in line for c in COORD):
            continue
        if any(x in line for x in LOCK_FREE):
            continue
        checked += 1
        held = lock_still_open(lines, enclosing_function(lines, i), i)
        if held is not None:
            bad += 1
            print(f"DEADLOCK RISK  {path}:{i + 1}")
            print(f"    call : {line.strip()[:96]}")
            print(f"    lock : {path}:{held} is still open here")

if bad:
    print(f"\n{bad} call site(s) inside an open lock scope, of {checked} checked.")
    print("Move the call after the lock scope closes, or take the value from a")
    print("snapshot captured before the lock was acquired.")
    sys.exit(1)

print(f"clean — {checked} coordinator call site(s), none inside a lock scope")
PY
