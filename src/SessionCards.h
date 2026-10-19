// POM2 — GPL-3.0-or-later
#pragma once
#include "SessionArchive.h"
#include <memory>
class SlotPeripheral;
class EmulationController;
namespace pom2 {
bool describeAdditionalSessionCard(SlotPeripheral* card, SessionArchive::Slot& record);
std::unique_ptr<SlotPeripheral> makeAdditionalSessionCard(const SessionArchive::Slot& record,
                                                         int slot, std::string& error);
std::unique_ptr<SlotPeripheral> prepareAdditionalSessionCard(const SessionArchive::Slot& record,
                                                            int slot, SlotPeripheral* live, std::string& error);
void wireAdditionalSessionCard(EmulationController& controller, SlotPeripheral* card);
}
