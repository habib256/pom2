#ifndef POM2_FUJINET_RUNTIME_H
#define POM2_FUJINET_RUNTIME_H

#include <string>

namespace pom2 {

/// Read-only packaged executable, or empty when this platform has no bundle.
std::string bundledFujiNetExecutable();

/// Copy the packaged runtime to per-user storage so fnconfig.ini and SD/
/// remain writable. No machine lock may be held across this call.
/// Returns false with an empty error when no runtime was bundled.
bool prepareBundledFujiNet(std::string& executable, std::string& error);

} // namespace pom2

#endif
