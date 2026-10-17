// POM2 — GPL-3.0-or-later
#pragma once
#include <string>
class EmulationController;
namespace pom2 {
bool saveSessionFile(EmulationController& controller, const std::string& path, std::string& error);
bool loadSessionFile(EmulationController& controller, const std::string& path, std::string& error);
}
