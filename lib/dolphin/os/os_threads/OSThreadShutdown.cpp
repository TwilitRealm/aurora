#include "OSThreadShutdown.hpp"

#include <dolphin/os.h>

namespace aurora::os {

std::atomic<bool> IsShuttingDown{false};

}

void OSBeginShutdown(void) { aurora::os::IsShuttingDown.store(true); }
