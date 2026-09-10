// POM2 — GPL-3.0-or-later
#pragma once
#include "Block512Backing.h"
namespace pom2 {
// Runtime transport injected into media; no worker-thread APIs in devices.
std::shared_ptr<Block512Backing::WriteBackExecutor> blockWriteBackExecutor();
}
