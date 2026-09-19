// POM2 — GPL-3.0-or-later
#pragma once
#include "Block512Backing.h"
#include "MediaAutosave.h"
namespace pom2 {
// Runtime transports injected into media; no worker-thread APIs in devices.
std::shared_ptr<Block512Backing::WriteBackExecutor> blockWriteBackExecutor();

/// The floppy autosave's executor (MediaAutosave.h): ONE worker thread,
/// commits in submission order, inline in the browser build. Process-wide.
MediaCommitExecutor& mediaCommitExecutor();
/// Block until every commit submitted to it has run. Shutdown / test hook;
/// the worker never takes `stateMutex`, but this waits on file I/O.
void drainMediaCommits();
}
