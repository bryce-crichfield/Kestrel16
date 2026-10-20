// Targets.h -- the machines KestrelValidator knows.

#pragma once

#include "Target.h"

#include <memory>
#include <string>

namespace Kestrel {

// The reference machine (Kestrel::Machine). Its limit counts instructions.
std::unique_ptr<Target> MakeEmulatorTarget();

// The gate-level component (normally "Kestrel16 Computer"), loaded from componentsDirectory. Its
// limit counts clock cycles. Returns null, with the reason in problem, if it can't be loaded.
std::unique_ptr<Target> MakeSimulatorTarget(const std::string& component, std::string& problem);

}  // namespace Kestrel
