#pragma once
// Shared constants. Quick-command strings now live in plugins/*.pfl and are
// loaded at runtime, so only VNC + default-input values remain here.

#include <string>

namespace Commands {

const std::string VNC_PASSWORD = "88888888";
const int VNC_PORT = 5900;
const std::string DEFAULT_USERNAME = "seewo";
const std::string DEFAULT_IP_RANGE = "172.16.20.6-12";

} // namespace Commands
